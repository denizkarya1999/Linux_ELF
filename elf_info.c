/*
 * Inspect an ELF file as data. The inspected program is never executed.
 * Works on GNU/Linux and Android, reading ELF32/ELF64 in either byte order.
 *
 * Reading order:
 *   1. Copy the file into a bounded buffer.
 *   2. Read the ELF header to learn the file's layout and target architecture.
 *   3. Walk program headers to find the interpreter and dynamic-linking data.
 *   4. Translate the dynamic string table's address and print library names.
 *
 * Program headers describe loading; section headers mainly describe linking
 * and debugging. We use program headers, so section names are not needed.
 * This reports useful metadata, not a guarantee that the file can run locally.
 */
#define _POSIX_C_SOURCE 200809L /* Request POSIX.1-2008 declarations before including any system headers. */
#include <elf.h> /* Define the ELF identification bytes, machine IDs, segment types, and dynamic tags. */
#include <inttypes.h> /* Provide fixed-width integer types and matching printf format macros. */
#include <stdio.h> /* Declare file I/O, formatted output, and diagnostic functions. */
#include <stdlib.h> /* Declare memory-management functions and process exit-status constants. */
#include <string.h> /* Declare string comparisons, byte comparisons, and character searches. */

/* Keep memory usage bounded when inspecting an unexpectedly large file. */
/* Express the 256 MiB input limit in bytes using unsigned-long arithmetic. */
#define MAX_FILE_SIZE (256UL * 1024 * 1024)

/* These settings describe the input file, not the machine running this tool. */
typedef struct {
    unsigned char *data;       /* Owned file buffer; main() frees it. */
    size_t size;               /* Number of bytes actually read. */
    int is64, little;          /* ELF class and byte order from e_ident. */
    uint64_t phoff;            /* File offset of the program-header table. */
    unsigned phsize, phcount;  /* Size of one entry, and number of entries. */
} Elf;

/* Normalize ELF32/ELF64 segment fields into one representation. */
typedef struct {
    /* offset is a position in the file; address is a virtual memory address.
     * size is the stored byte count; memory_size includes zero-filled memory. */
    uint64_t type, offset, address, size, memory_size;
} Segment;

/* Parsing helpers return 1 for success and 0 for failure (unlike exit codes). */
/* Write a parser diagnostic and return the helper convention for failure. */
static int error(const char *message)
{
    /* Send the supplied text to standard error with the tool's name as a prefix. */
    fprintf(stderr, "elf_info: %s\n", message);
    return 0; /* Report failure to a caller using this helper's true/false convention. */
}

/* Subtraction avoids overflow from checking offset + length directly. */
/* Validate a proposed file range without reading its contents. */
static int in_file(const Elf *elf, uint64_t offset, uint64_t length)
{
    /* Check offset before subtracting, then ensure the length fits in the remaining bytes. */
    return offset <= elf->size && length <= elf->size - offset;
}

/*
 * Decode bytes instead of casting to ELF structs. This handles another CPU's
 * byte order and avoids unaligned reads. Callers validate the containing range.
 */
/* Decode a validated 2-, 4-, or 8-byte integer from the specified file position. */
static uint64_t number(const Elf *elf, uint64_t offset, unsigned width)
{
    uint64_t value = 0; /* Start the result with every bit clear. */
    for (unsigned i = 0; i < width; ++i) { /* Consume exactly as many bytes as the field requires. */
        /* Consume the most significant byte first, regardless of file order.
         * Shifting by eight opens space for the next byte in the result. */
        /* Reverse traversal for little-endian input so the highest-value byte is read first. */
        unsigned index = elf->little ? width - 1 - i : i;
        /* Shift earlier bytes upward and insert the current byte into the low eight bits. */
        value = (value << 8) | elf->data[(size_t)offset + index];
    }
    return value; /* Return the decoded integer in a host-native uint64_t. */
}

/* Seekable files let us check the size before allocating the whole buffer. */
/* Open the readable input and copy its contents into the owned Elf buffer. */
static int read_file(Elf *elf, const char *path)
{
    /* Open the input in binary read mode without modifying or executing it. */
    FILE *file = fopen(path, "rb");
    if (!file) { /* Detect an unsuccessful open before using the file handle. */
        /* Include the operating system's explanation, such as a missing file or denied access. */
        perror("elf_info: open");
        return 0; /* Report failure to a caller using this helper's true/false convention. */
    }
    /* Seek to the end to measure length; reject input that does not support seeking. */
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file); /* Close the opened file handle and release its associated resources. */
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("cannot seek input file");
    }
    /* The file position after seeking to the end equals its length in bytes. */
    long size = ftell(file); /* Read the end-of-file position; a negative result indicates failure. */
    /* Reject an invalid length or a file exceeding the allocation limit. */
    if (size < 0 || (unsigned long)size > MAX_FILE_SIZE) {
        fclose(file); /* Close the opened file handle and release its associated resources. */
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("cannot determine size, or file exceeds the 256 MiB limit");
    }
    elf->size = (size_t)size; /* Store the validated byte length for all subsequent range checks. */
    /* Avoid malloc(0): empty input will be rejected later as a non-ELF file. */
    /* Allocate the input buffer, requesting at least one byte even for an empty file. */
    elf->data = malloc(elf->size ? elf->size : 1);
    if (!elf->data) { /* Detect allocation failure before trying to read into the buffer. */
        fclose(file); /* Close the opened file handle and release its associated resources. */
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("cannot allocate file buffer");
    }
    /* A short read can happen if the file shrinks or an I/O error occurs.
     * main() still releases the allocated buffer when this function fails. */
    /* Return to the beginning and only attempt the read if repositioning succeeds. */
    int ok = fseek(file, 0, SEEK_SET) == 0 &&
             /* Require the expected byte count and no stream error for a successful read. */
             fread(elf->data, 1, elf->size, file) == elf->size && !ferror(file);
    fclose(file); /* Close the opened file handle and release its associated resources. */
    /* Return success for a full read or print a failure diagnostic otherwise. */
    return ok ? 1 : error("cannot read complete input file");
}

/* Validate the header and save the settings used to decode the remaining file. */
static int read_header(Elf *elf)
{
    /* The first 16 bytes are e_ident. They include the magic bytes 0x7f ELF,
     * class (32/64-bit), encoding (byte order), and identification version. */
    /* Reject a short identification block or mismatched ELF magic bytes. */
    if (elf->size < EI_NIDENT || memcmp(elf->data, ELFMAG, SELFMAG))
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("not an ELF file, or truncated ELF identification");
    /* Accept only the two class layouts supported by this reader. */
    if (elf->data[EI_CLASS] != ELFCLASS32 && elf->data[EI_CLASS] != ELFCLASS64)
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("unsupported ELF class");
    /* Require an explicitly supported little-endian or big-endian encoding. */
    if (elf->data[EI_DATA] != ELFDATA2LSB && elf->data[EI_DATA] != ELFDATA2MSB)
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("unsupported ELF byte order");
    /* Save whether addresses and offsets follow the ELF64 layout. */
    elf->is64 = elf->data[EI_CLASS] == ELFCLASS64;
    /* Save the input's byte order, independently of the host CPU. */
    elf->little = elf->data[EI_DATA] == ELFDATA2LSB;

    /* The remaining header has fixed offsets defined by the ELF format.
     * Check its full range before number() reads any multi-byte field. */
    unsigned header_size = elf->is64 ? 64 : 52; /* Select the complete header size for this ELF class. */
    /* Check that the whole header exists before reading any numeric fields from it. */
    if (!in_file(elf, 0, header_size))
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("truncated ELF header");
    /* Require the supported version in both e_ident and the main e_version field. */
    if (elf->data[EI_VERSION] != EV_CURRENT || number(elf, 20, 4) != EV_CURRENT)
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("unsupported ELF version");
    /* Check that e_ehsize agrees with the known size of the selected header layout. */
    if (number(elf, elf->is64 ? 52 : 40, 2) != header_size)
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("invalid ELF header size");

    /* e_phoff, e_phentsize, and e_phnum locate the program-header entries.
     * ELF64 uses 8-byte offsets, while ELF32 uses 4-byte offsets. */
    /* Read e_phoff: the file position where the program-header array begins. */
    elf->phoff = number(elf, elf->is64 ? 32 : 28, elf->is64 ? 8 : 4);
    /* Read e_phentsize: the size in bytes of a single program-header entry. */
    elf->phsize = (unsigned)number(elf, elf->is64 ? 54 : 42, 2);
    /* Read e_phnum: the number of entries in that array. */
    elf->phcount = (unsigned)number(elf, elf->is64 ? 56 : 44, 2);
    /* PN_XNUM stores the real count in section header zero. This small reader
     * rejects that extension explicitly instead of assuming an incorrect count. */
    /* Detect the special count requiring an unsupported section-header lookup. */
    if (elf->phcount == PN_XNUM)
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("extended program-header numbering is not supported");
    /* Zero entries are valid for files such as relocatable object files.
     * Otherwise validate the whole table before segment() indexes into it. */
    if (elf->phcount && /* Check placement and extent only when the file declares one or more entries. */
        /* Reject table/header overlap or a program-header entry size this reader cannot decode. */
        (elf->phoff < header_size || elf->phsize != (elf->is64 ? 56U : 32U) ||
         /* Calculate the table length in 64 bits and require it to fit inside the file. */
         !in_file(elf, elf->phoff, (uint64_t)elf->phsize * elf->phcount)))
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("invalid or truncated program-header table");
    return 1; /* Report success to a caller using this helper's true/false convention. */
}

/*
 * ELF32 and ELF64 store these fields at different offsets. In particular,
 * ELF64 puts p_flags immediately after p_type, shifting the following fields.
 * read_header() must have validated the table, and index must be < phcount.
 */
/* Decode one entry from the already validated program-header array. */
static Segment segment(const Elf *elf, unsigned index)
{
    /* Locate entry index by stepping through fixed-size program headers. */
    uint64_t base = elf->phoff + (uint64_t)index * elf->phsize;
    unsigned width = elf->is64 ? 8 : 4; /* Select the width used for segment addresses, offsets, and sizes. */
    Segment s; /* Reserve a normalized result whose members will all be assigned below. */
    s.type = number(elf, base, 4); /* Read p_type, which occupies four bytes in either ELF class. */
    /* Read p_offset, the start of this segment's stored file bytes. */
    s.offset = number(elf, base + (elf->is64 ? 8 : 4), width);
    /* Read p_vaddr, the virtual address associated with the segment. */
    s.address = number(elf, base + (elf->is64 ? 16 : 8), width);
    /* Read p_filesz, the number of bytes present in the file. */
    s.size = number(elf, base + (elf->is64 ? 32 : 16), width);
    /* Read p_memsz, which can include additional zero-filled memory. */
    s.memory_size = number(elf, base + (elf->is64 ? 40 : 20), width);
    return s; /* Return the fully populated segment description by value. */
}

/* e_machine identifies the CPU instruction set required by the input file.
 * Unknown values remain useful because inspect() also prints their number. */
/* Return a readable name for a numeric ELF machine identifier. */
static const char *machine_name(uint64_t machine)
{
    switch (machine) { /* Select the name using the input file's e_machine value. */
    case EM_386: return "x86"; /* Map this ELF machine identifier to the readable CPU label returned here. */
    /* Map this ELF machine identifier to the readable CPU label returned here. */
    case EM_X86_64: return "x86-64";
    case EM_ARM: return "ARM"; /* Map this ELF machine identifier to the readable CPU label returned here. */
    /* Map this ELF machine identifier to the readable CPU label returned here. */
    case EM_AARCH64: return "AArch64 (ARM64)";
    /* Map this ELF machine identifier to the readable CPU label returned here. */
    case EM_RISCV: return "RISC-V";
    /* Map this ELF machine identifier to the readable CPU label returned here. */
    case EM_MIPS: return "MIPS";
    /* Map this ELF machine identifier to the readable CPU label returned here. */
    case EM_PPC: return "PowerPC";
    /* Map this ELF machine identifier to the readable CPU label returned here. */
    case EM_PPC64: return "PowerPC64";
    /* Keep a fallback for unlisted IDs; inspect() also prints the original numeric ID. */
    default: return "Other/unknown";
    }
}

/* ET_DYN alone cannot distinguish a shared library from a PIE executable.
 * PIE means position-independent executable: its load address can vary. */
static const char *type_name(uint64_t type) /* Return a description of the input file's ELF type. */
{
    switch (type) { /* Choose a description from the e_type field. */
    /* Identify a file intended for linking rather than direct execution. */
    case ET_REL: return "Relocatable object";
    case ET_EXEC: return "Executable"; /* Identify the ELF executable file type. */
    /* Keep both possible meanings instead of claiming this tag alone distinguishes them. */
    case ET_DYN: return "Shared object or PIE executable";
    case ET_CORE: return "Core dump"; /* Identify a core-file type rather than an ordinary program image. */
    /* Keep a fallback for unlisted IDs; inspect() also prints the original numeric ID. */
    default: return "Other/unknown";
    }
}

/* Escape terminal control bytes in names supplied by the inspected file.
 * The caller must first verify that the text has a terminating null byte.
 * Backslashes are escaped too, distinguishing literal text from our escapes. */
/* Print a validated null-terminated string using visible escapes for unusual bytes. */
static void print_text(const unsigned char *text)
{
    for (; *text; ++text) { /* Advance one byte at a time until the previously validated null terminator. */
        /* Double a literal backslash so it cannot look like an escape generated by this tool. */
        if (*text == '\\') fputs("\\\\", stdout);
        else if (*text >= 32 && *text <= 126) putchar(*text); /* Print ordinary ASCII characters directly. */
        /* Render other bytes as hexadecimal escapes rather than terminal control sequences. */
        else printf("\\x%02x", (unsigned)*text);
    }
}

/*
 * DT_STRTAB is a virtual address, not a file offset. A PT_LOAD segment maps
 * bytes from p_offset in the file to p_vaddr in memory. The translation is:
 *     string file offset = p_offset + (string address - p_vaddr)
 * inspect() has already checked that each relevant segment fits in the file.
 */
/* Accept the virtual start and byte length of the string table to translate. */
static int string_table(const Elf *elf, uint64_t address, uint64_t size,
                        /* Receive a pointer where the function writes the resulting file offset. */
                        uint64_t *offset)
{
    /* Visit each entry in the validated program-header array. */
    for (unsigned i = 0; i < elf->phcount; ++i) {
        Segment s = segment(elf, i); /* Decode this entry into the shared Segment representation. */
        /* Skip segments that cannot contain the requested virtual address. */
        if (s.type != PT_LOAD || address < s.address) continue;
        /* Measure the requested address's distance from the segment's virtual start. */
        uint64_t delta = address - s.address;
        /* Require the entire table to be stored in this segment. Its memory
         * region may be larger, but the zero-filled tail has no file bytes. */
        /* Require the whole table, not just its first byte, to fit in the stored segment. */
        if (delta <= s.size && size <= s.size - delta) {
            /* Write the translated file offset through the caller's output pointer. */
            *offset = s.offset + delta;
            return 1; /* Report success to a caller using this helper's true/false convention. */
        }
    }
    /* Print this diagnostic and propagate failure to the calling parser stage. */
    return error("dynamic string table is not in a file-backed load segment");
}

/* Read direct DT_NEEDED dependencies only; do not load or search for libraries. */
/* Resolve and print direct library dependencies from a validated dynamic segment. */
static int print_libraries(const Elf *elf, Segment dynamic)
{
    /* Each dynamic entry is a tag/value pair: 8 bytes for ELF32, 16 for ELF64. */
    /* Each entry contains two equally wide fields: a tag followed by its value. */
    unsigned width = elf->is64 ? 8 : 4, stride = width * 2;
    /* Reject an empty segment or one ending with an incomplete dynamic entry. */
    if (!dynamic.size || dynamic.size % stride)
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("invalid dynamic segment size");

    /* Track the string-table address, byte size, file offset, and dynamic-list end. */
    uint64_t address = 0, size = 0, table = 0, end = 0;
    /* Record whether required metadata and dependency markers have been encountered. */
    int has_address = 0, has_size = 0, terminated = 0, needed = 0;
    /* First pass: find the table before resolving any DT_NEEDED offsets.
     * Tags need not arrive in that order. DT_STRSZ gives the table's byte size,
     * and DT_NULL terminates the list even if the segment has padding after it.
     * Separate presence flags allow address zero to be treated as a value. */
    /* Scan one complete entry per iteration without passing the segment boundary. */
    for (uint64_t i = 0; i < dynamic.size; i += stride) {
        /* Read the tag that determines how to interpret the accompanying value. */
        uint64_t tag = number(elf, dynamic.offset + i, width);
        /* Read the second field immediately after the tag. */
        uint64_t value = number(elf, dynamic.offset + i + width, width);
        /* Save the terminating entry's offset and stop before any trailing padding. */
        if (tag == DT_NULL) { end = i; terminated = 1; break; }
        /* Save the string table's virtual address and mark that field present. */
        if (tag == DT_STRTAB) { address = value; has_address = 1; }
        /* Save the string table's byte length and mark that field present. */
        if (tag == DT_STRSZ) { size = value; has_size = 1; }
        if (tag == DT_NEEDED) needed = 1; /* Remember that at least one library name will need resolution. */
    }
    /* Reject a list that reached the boundary without its required end marker. */
    if (!terminated) return error("dynamic segment has no DT_NULL terminator");
    /* Report a valid empty dependency list without resolving a string table. */
    if (!needed) { puts("  (none listed)"); return 1; }
    /* Nonempty dependency lists require a table address and a nonzero length. */
    if (!has_address || !has_size || !size)
        /* Print this diagnostic and propagate failure to the calling parser stage. */
        return error("missing dynamic string table information");
    /* Translate the address into a file offset and propagate a translation failure. */
    if (!string_table(elf, address, size, &table)) return 0;

    /* Second pass: each DT_NEEDED value is an offset within the string table,
     * not an absolute address. Preserve the dependency order from the file. */
    /* Revisit only the entries before the DT_NULL marker found in the first pass. */
    for (uint64_t i = 0; i < end; i += stride) {
        /* Ignore tags that do not name a directly required library. */
        if (number(elf, dynamic.offset + i, width) != DT_NEEDED) continue;
        /* Read the library name's offset relative to the string table. */
        uint64_t name = number(elf, dynamic.offset + i + width, width);
        /* Find the null terminator inside the remaining table bytes before
         * passing a string to print_text(), which reads until that terminator. */
        /* Require a valid offset and a null terminator before the table ends. */
        if (name >= size || !memchr(elf->data + table + name, 0, (size_t)(size - name)))
            /* Print this diagnostic and propagate failure to the calling parser stage. */
            return error("invalid or unterminated library name");
        fputs("  ", stdout); /* Indent the library name in the dependency list. */
        /* Print the validated name with terminal-control bytes escaped. */
        print_text(elf->data + table + name);
        putchar('\n'); /* Finish the current output line with a newline. */
    }
    return 1; /* Report success to a caller using this helper's true/false convention. */
}

/* Validate relevant segments, then print the metadata and dependency report. */
static int inspect(const Elf *elf, const char *path)
{
    /* PT_INTERP contains the loader path; PT_DYNAMIC contains linking metadata.
     * Keep explicit presence flags because a zero-initialized segment alone
     * cannot tell us whether its header was actually present in the input. */
    /* Initialize storage for the two optional segment descriptions. */
    Segment interpreter = {0}, dynamic = {0};
    /* Track whether those segments actually appear, independently of their field values. */
    int has_interpreter = 0, has_dynamic = 0;
    /* Visit each entry in the validated program-header array. */
    for (unsigned i = 0; i < elf->phcount; ++i) {
        Segment s = segment(elf, i); /* Decode this entry into the shared Segment representation. */
        /* Other segment types (notes, TLS, etc.) are outside this tool's scope. */
        /* Ignore segments that this report does not inspect. */
        if (s.type != PT_LOAD && s.type != PT_INTERP && s.type != PT_DYNAMIC) continue;
        /* Validate the full stored range before examining any relevant segment bytes. */
        if (!in_file(elf, s.offset, s.size))
            /* Print this diagnostic and propagate failure to the calling parser stage. */
            return error("segment extends beyond the file");
        /* A load segment may reserve extra zero-filled memory, but its stored
         * bytes cannot exceed the memory region that is meant to hold them. */
        /* A load segment's memory region must hold all of its stored bytes. */
        if (s.type == PT_LOAD && s.size > s.memory_size)
            /* Print this diagnostic and propagate failure to the calling parser stage. */
            return error("load segment file size exceeds memory size");
        if (s.type == PT_INTERP) { /* Handle the segment containing the requested dynamic-loader pathname. */
            /* An interpreter must be a single, nonempty, terminated path. */
            /* Reject duplicates, zero-length data, and paths beginning with a null byte. */
            if (has_interpreter || !s.size || !elf->data[s.offset] ||
                /* Require the loader pathname to terminate inside its declared segment. */
                !memchr(elf->data + s.offset, 0, (size_t)s.size))
                /* Print this diagnostic and propagate failure to the calling parser stage. */
                return error("duplicate, empty, or unterminated interpreter segment");
            interpreter = s; has_interpreter = 1; /* Save the interpreter's location and mark it as found. */
        }
        if (s.type == PT_DYNAMIC) { /* Handle the segment containing the dynamic-linking tag/value list. */
            /* Reject multiple dynamic segments rather than choosing one arbitrarily. */
            if (has_dynamic) return error("duplicate dynamic segment");
            /* Save the dynamic segment for dependency parsing after validation completes. */
            dynamic = s; has_dynamic = 1;
        }
    }

    /* Header offsets 16, 18, and 24 hold e_type, e_machine, and e_entry.
     * PRIu64/PRIx64 choose the correct printf format for uint64_t on each host. */
    /* Print the inspected path safely, including any unusual bytes in its name. */
    fputs("File:         ", stdout); print_text((const unsigned char *)path); putchar('\n');
    /* Display the ELF32 or ELF64 layout selected from the input header. */
    printf("Class:        ELF%d\n", elf->is64 ? 64 : 32);
    /* Display the input byte order used by the decoder. */
    printf("Byte order:   %s-endian\n", elf->little ? "Little" : "Big");
    /* Read the two-byte e_type and e_machine fields from the validated header. */
    uint64_t type = number(elf, 16, 2), machine = number(elf, 18, 2);
    /* Print both the readable file-type label and its exact numeric identifier. */
    printf("Type:         %s (%" PRIu64 ")\n", type_name(type), type);
    /* Print the file's required CPU architecture, which may differ from the host CPU. */
    printf("Architecture: %s (%" PRIu64 ")\n", machine_name(machine), machine);
    /* Both GNU/Linux and Android commonly use the generic System V tag (0).
     * Do not infer the operating system from this byte alone. */
    /* Display the OS/ABI byte without making an unsupported operating-system claim. */
    printf("OS/ABI tag:   %u (not a reliable OS detector)\n", (unsigned)elf->data[EI_OSABI]);
    /* Print e_entry in hexadecimal; zero can indicate that no entry point is specified. */
    printf("Entry point:  0x%" PRIx64 "\n", number(elf, 24, elf->is64 ? 8 : 4));
    printf("Interpreter:  "); /* Print the label before displaying the loader path or absence marker. */
    /* No interpreter is normal for some files, including shared libraries;
     * it does not by itself prove that an executable is statically linked. */
    /* Print the already validated pathname when PT_INTERP is present. */
    if (has_interpreter) print_text(elf->data + interpreter.offset);
    else fputs("(none listed)", stdout); /* Explicitly report that no interpreter segment was found. */
    /* Begin the list of libraries requested directly by this file. */
    puts("\nLibraries (direct DT_NEEDED entries):");
    /* Parse dynamic dependencies and return that stage's success or failure. */
    if (has_dynamic) return print_libraries(elf, dynamic);
    /* Distinguish an absent dynamic segment from a present but empty dependency list. */
    puts("  (no dynamic segment)");
    return 1; /* Report success to a caller using this helper's true/false convention. */
}

int main(int argc, char **argv) /* Receive the invocation name and user arguments from process startup. */
{
    /* Accept exactly one file path, or --help. The file need only be readable;
     * it does not need executable permission or the same architecture as us. */
    /* Recognize exactly one --help argument before interpreting arguments as filenames. */
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        /* Explain the one-file interface and its read-only inspection behavior. */
        puts("Usage: elf_info FILE\nInspect ELF metadata without executing FILE.");
        return EXIT_SUCCESS; /* Finish successfully and report the shell's success status. */
    }
    if (argc != 2) { /* Require exactly one input path after the program name for normal inspection. */
        /* Send the invalid-argument usage message to standard error. */
        fputs("Usage: elf_info FILE\n", stderr);
        return EXIT_FAILURE; /* Finish with a failure status so the shell can detect the error. */
    }
    /* Zero initialization makes free(elf.data) safe even if opening fails.
     * && stops at the first failure, so later stages never see invalid state. */
    Elf elf = {0}; /* Initialize all context members, including the owned pointer, to zero. */
    /* Read, validate, and report in order; && stops immediately when any stage fails. */
    int ok = read_file(&elf, argv[1]) && read_header(&elf) && inspect(&elf, argv[1]);
    free(elf.data); /* Release the owned buffer after success or failure; free(NULL) is safe. */
    /* Translate helper success/failure to the shell's 0/nonzero exit status. */
    /* Convert helper-style success into the shell's zero/nonzero exit convention. */
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
