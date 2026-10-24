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
#define _POSIX_C_SOURCE 200809L
#include <elf.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep memory usage bounded when inspecting an unexpectedly large file. */
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
static int error(const char *message)
{
    fprintf(stderr, "elf_info: %s\n", message);
    return 0;
}

/* Subtraction avoids overflow from checking offset + length directly. */
static int in_file(const Elf *elf, uint64_t offset, uint64_t length)
{
    return offset <= elf->size && length <= elf->size - offset;
}

/*
 * Decode bytes instead of casting to ELF structs. This handles another CPU's
 * byte order and avoids unaligned reads. Callers validate the containing range.
 */
static uint64_t number(const Elf *elf, uint64_t offset, unsigned width)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
        /* Consume the most significant byte first, regardless of file order.
         * Shifting by eight opens space for the next byte in the result. */
        unsigned index = elf->little ? width - 1 - i : i;
        value = (value << 8) | elf->data[(size_t)offset + index];
    }
    return value;
}

/* Seekable files let us check the size before allocating the whole buffer. */
static int read_file(Elf *elf, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror("elf_info: open");
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return error("cannot seek input file");
    }
    /* The file position after seeking to the end equals its length in bytes. */
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > MAX_FILE_SIZE) {
        fclose(file);
        return error("cannot determine size, or file exceeds the 256 MiB limit");
    }
    elf->size = (size_t)size;
    /* Avoid malloc(0): empty input will be rejected later as a non-ELF file. */
    elf->data = malloc(elf->size ? elf->size : 1);
    if (!elf->data) {
        fclose(file);
        return error("cannot allocate file buffer");
    }
    /* A short read can happen if the file shrinks or an I/O error occurs.
     * main() still releases the allocated buffer when this function fails. */
    int ok = fseek(file, 0, SEEK_SET) == 0 &&
             fread(elf->data, 1, elf->size, file) == elf->size && !ferror(file);
    fclose(file);
    return ok ? 1 : error("cannot read complete input file");
}

static int read_header(Elf *elf)
{
    /* The first 16 bytes are e_ident. They include the magic bytes 0x7f ELF,
     * class (32/64-bit), encoding (byte order), and identification version. */
    if (elf->size < EI_NIDENT || memcmp(elf->data, ELFMAG, SELFMAG))
        return error("not an ELF file, or truncated ELF identification");
    if (elf->data[EI_CLASS] != ELFCLASS32 && elf->data[EI_CLASS] != ELFCLASS64)
        return error("unsupported ELF class");
    if (elf->data[EI_DATA] != ELFDATA2LSB && elf->data[EI_DATA] != ELFDATA2MSB)
        return error("unsupported ELF byte order");
    elf->is64 = elf->data[EI_CLASS] == ELFCLASS64;
    elf->little = elf->data[EI_DATA] == ELFDATA2LSB;

    /* The remaining header has fixed offsets defined by the ELF format.
     * Check its full range before number() reads any multi-byte field. */
    unsigned header_size = elf->is64 ? 64 : 52;
    if (!in_file(elf, 0, header_size))
        return error("truncated ELF header");
    if (elf->data[EI_VERSION] != EV_CURRENT || number(elf, 20, 4) != EV_CURRENT)
        return error("unsupported ELF version");
    if (number(elf, elf->is64 ? 52 : 40, 2) != header_size)
        return error("invalid ELF header size");

    /* e_phoff, e_phentsize, and e_phnum locate the program-header entries.
     * ELF64 uses 8-byte offsets, while ELF32 uses 4-byte offsets. */
    elf->phoff = number(elf, elf->is64 ? 32 : 28, elf->is64 ? 8 : 4);
    elf->phsize = (unsigned)number(elf, elf->is64 ? 54 : 42, 2);
    elf->phcount = (unsigned)number(elf, elf->is64 ? 56 : 44, 2);
    /* PN_XNUM stores the real count in section header zero. This small reader
     * rejects that extension explicitly instead of assuming an incorrect count. */
    if (elf->phcount == PN_XNUM)
        return error("extended program-header numbering is not supported");
    /* Zero entries are valid for files such as relocatable object files.
     * Otherwise validate the whole table before segment() indexes into it. */
    if (elf->phcount &&
        (elf->phoff < header_size || elf->phsize != (elf->is64 ? 56U : 32U) ||
         !in_file(elf, elf->phoff, (uint64_t)elf->phsize * elf->phcount)))
        return error("invalid or truncated program-header table");
    return 1;
}

/*
 * ELF32 and ELF64 store these fields at different offsets. In particular,
 * ELF64 puts p_flags immediately after p_type, shifting the following fields.
 * read_header() must have validated the table, and index must be < phcount.
 */
static Segment segment(const Elf *elf, unsigned index)
{
    uint64_t base = elf->phoff + (uint64_t)index * elf->phsize;
    unsigned width = elf->is64 ? 8 : 4;
    Segment s;
    s.type = number(elf, base, 4);
    s.offset = number(elf, base + (elf->is64 ? 8 : 4), width);
    s.address = number(elf, base + (elf->is64 ? 16 : 8), width);
    s.size = number(elf, base + (elf->is64 ? 32 : 16), width);
    s.memory_size = number(elf, base + (elf->is64 ? 40 : 20), width);
    return s;
}

/* e_machine identifies the CPU instruction set required by the input file.
 * Unknown values remain useful because inspect() also prints their number. */
static const char *machine_name(uint64_t machine)
{
    switch (machine) {
    case EM_386: return "x86";
    case EM_X86_64: return "x86-64";
    case EM_ARM: return "ARM";
    case EM_AARCH64: return "AArch64 (ARM64)";
    case EM_RISCV: return "RISC-V";
    case EM_MIPS: return "MIPS";
    case EM_PPC: return "PowerPC";
    case EM_PPC64: return "PowerPC64";
    default: return "Other/unknown";
    }
}

/* ET_DYN alone cannot distinguish a shared library from a PIE executable.
 * PIE means position-independent executable: its load address can vary. */
static const char *type_name(uint64_t type)
{
    switch (type) {
    case ET_REL: return "Relocatable object";
    case ET_EXEC: return "Executable";
    case ET_DYN: return "Shared object or PIE executable";
    case ET_CORE: return "Core dump";
    default: return "Other/unknown";
    }
}

/* Escape terminal control bytes in names supplied by the inspected file.
 * The caller must first verify that the text has a terminating null byte.
 * Backslashes are escaped too, distinguishing literal text from our escapes. */
static void print_text(const unsigned char *text)
{
    for (; *text; ++text) {
        if (*text == '\\') fputs("\\\\", stdout);
        else if (*text >= 32 && *text <= 126) putchar(*text);
        else printf("\\x%02x", (unsigned)*text);
    }
}

/*
 * DT_STRTAB is a virtual address, not a file offset. A PT_LOAD segment maps
 * bytes from p_offset in the file to p_vaddr in memory. The translation is:
 *     string file offset = p_offset + (string address - p_vaddr)
 * inspect() has already checked that each relevant segment fits in the file.
 */
static int string_table(const Elf *elf, uint64_t address, uint64_t size,
                        uint64_t *offset)
{
    for (unsigned i = 0; i < elf->phcount; ++i) {
        Segment s = segment(elf, i);
        if (s.type != PT_LOAD || address < s.address) continue;
        uint64_t delta = address - s.address;
        /* Require the entire table to be stored in this segment. Its memory
         * region may be larger, but the zero-filled tail has no file bytes. */
        if (delta <= s.size && size <= s.size - delta) {
            *offset = s.offset + delta;
            return 1;
        }
    }
    return error("dynamic string table is not in a file-backed load segment");
}

/* Read direct DT_NEEDED dependencies only; do not load or search for libraries. */
static int print_libraries(const Elf *elf, Segment dynamic)
{
    /* Each dynamic entry is a tag/value pair: 8 bytes for ELF32, 16 for ELF64. */
    unsigned width = elf->is64 ? 8 : 4, stride = width * 2;
    if (!dynamic.size || dynamic.size % stride)
        return error("invalid dynamic segment size");

    uint64_t address = 0, size = 0, table = 0, end = 0;
    int has_address = 0, has_size = 0, terminated = 0, needed = 0;
    /* First pass: find the table before resolving any DT_NEEDED offsets.
     * Tags need not arrive in that order. DT_STRSZ gives the table's byte size,
     * and DT_NULL terminates the list even if the segment has padding after it.
     * Separate presence flags allow address zero to be treated as a value. */
    for (uint64_t i = 0; i < dynamic.size; i += stride) {
        uint64_t tag = number(elf, dynamic.offset + i, width);
        uint64_t value = number(elf, dynamic.offset + i + width, width);
        if (tag == DT_NULL) { end = i; terminated = 1; break; }
        if (tag == DT_STRTAB) { address = value; has_address = 1; }
        if (tag == DT_STRSZ) { size = value; has_size = 1; }
        if (tag == DT_NEEDED) needed = 1;
    }
    if (!terminated) return error("dynamic segment has no DT_NULL terminator");
    if (!needed) { puts("  (none listed)"); return 1; }
    if (!has_address || !has_size || !size)
        return error("missing dynamic string table information");
    if (!string_table(elf, address, size, &table)) return 0;

    /* Second pass: each DT_NEEDED value is an offset within the string table,
     * not an absolute address. Preserve the dependency order from the file. */
    for (uint64_t i = 0; i < end; i += stride) {
        if (number(elf, dynamic.offset + i, width) != DT_NEEDED) continue;
        uint64_t name = number(elf, dynamic.offset + i + width, width);
        /* Find the null terminator inside the remaining table bytes before
         * passing a string to print_text(), which reads until that terminator. */
        if (name >= size || !memchr(elf->data + table + name, 0, (size_t)(size - name)))
            return error("invalid or unterminated library name");
        fputs("  ", stdout);
        print_text(elf->data + table + name);
        putchar('\n');
    }
    return 1;
}

static int inspect(const Elf *elf, const char *path)
{
    /* PT_INTERP contains the loader path; PT_DYNAMIC contains linking metadata.
     * Keep explicit presence flags because a zero-initialized segment alone
     * cannot tell us whether its header was actually present in the input. */
    Segment interpreter = {0}, dynamic = {0};
    int has_interpreter = 0, has_dynamic = 0;
    for (unsigned i = 0; i < elf->phcount; ++i) {
        Segment s = segment(elf, i);
        /* Other segment types (notes, TLS, etc.) are outside this tool's scope. */
        if (s.type != PT_LOAD && s.type != PT_INTERP && s.type != PT_DYNAMIC) continue;
        if (!in_file(elf, s.offset, s.size))
            return error("segment extends beyond the file");
        /* A load segment may reserve extra zero-filled memory, but its stored
         * bytes cannot exceed the memory region that is meant to hold them. */
        if (s.type == PT_LOAD && s.size > s.memory_size)
            return error("load segment file size exceeds memory size");
        if (s.type == PT_INTERP) {
            /* An interpreter must be a single, nonempty, terminated path. */
            if (has_interpreter || !s.size || !elf->data[s.offset] ||
                !memchr(elf->data + s.offset, 0, (size_t)s.size))
                return error("duplicate, empty, or unterminated interpreter segment");
            interpreter = s; has_interpreter = 1;
        }
        if (s.type == PT_DYNAMIC) {
            if (has_dynamic) return error("duplicate dynamic segment");
            dynamic = s; has_dynamic = 1;
        }
    }

    /* Header offsets 16, 18, and 24 hold e_type, e_machine, and e_entry.
     * PRIu64/PRIx64 choose the correct printf format for uint64_t on each host. */
    fputs("File:         ", stdout); print_text((const unsigned char *)path); putchar('\n');
    printf("Class:        ELF%d\n", elf->is64 ? 64 : 32);
    printf("Byte order:   %s-endian\n", elf->little ? "Little" : "Big");
    uint64_t type = number(elf, 16, 2), machine = number(elf, 18, 2);
    printf("Type:         %s (%" PRIu64 ")\n", type_name(type), type);
    printf("Architecture: %s (%" PRIu64 ")\n", machine_name(machine), machine);
    /* Both GNU/Linux and Android commonly use the generic System V tag (0).
     * Do not infer the operating system from this byte alone. */
    printf("OS/ABI tag:   %u (not a reliable OS detector)\n", (unsigned)elf->data[EI_OSABI]);
    printf("Entry point:  0x%" PRIx64 "\n", number(elf, 24, elf->is64 ? 8 : 4));
    printf("Interpreter:  ");
    /* No interpreter is normal for some files, including shared libraries;
     * it does not by itself prove that an executable is statically linked. */
    if (has_interpreter) print_text(elf->data + interpreter.offset);
    else fputs("(none listed)", stdout);
    puts("\nLibraries (direct DT_NEEDED entries):");
    if (has_dynamic) return print_libraries(elf, dynamic);
    puts("  (no dynamic segment)");
    return 1;
}

int main(int argc, char **argv)
{
    /* Accept exactly one file path, or --help. The file need only be readable;
     * it does not need executable permission or the same architecture as us. */
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: elf_info FILE\nInspect ELF metadata without executing FILE.");
        return EXIT_SUCCESS;
    }
    if (argc != 2) {
        fputs("Usage: elf_info FILE\n", stderr);
        return EXIT_FAILURE;
    }
    /* Zero initialization makes free(elf.data) safe even if opening fails.
     * && stops at the first failure, so later stages never see invalid state. */
    Elf elf = {0};
    int ok = read_file(&elf, argv[1]) && read_header(&elf) && inspect(&elf, argv[1]);
    free(elf.data);
    /* Translate helper success/failure to the shell's 0/nonzero exit status. */
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
