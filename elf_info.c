/*
 * Inspect an ELF file as data. The inspected program is never executed.
 * Works on GNU/Linux and Android, reading ELF32/ELF64 in either byte order.
 */
#define _POSIX_C_SOURCE 200809L
#include <elf.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep memory usage bounded when inspecting an unexpectedly large file. */
#define MAX_FILE_SIZE (256UL * 1024 * 1024)

typedef struct {
    unsigned char *data;
    size_t size;
    int is64, little;
    uint64_t phoff;
    unsigned phsize, phcount;
} Elf;

typedef struct {
    uint64_t type, offset, address, size, memory_size;
} Segment;

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
        unsigned index = elf->little ? width - 1 - i : i;
        value = (value << 8) | elf->data[(size_t)offset + index];
    }
    return value;
}

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
    long size = ftell(file);
    if (size < 0 || (unsigned long)size > MAX_FILE_SIZE) {
        fclose(file);
        return error("cannot determine size, or file exceeds the 256 MiB limit");
    }
    elf->size = (size_t)size;
    elf->data = malloc(elf->size ? elf->size : 1);
    if (!elf->data) {
        fclose(file);
        return error("cannot allocate file buffer");
    }
    int ok = fseek(file, 0, SEEK_SET) == 0 &&
             fread(elf->data, 1, elf->size, file) == elf->size && !ferror(file);
    fclose(file);
    return ok ? 1 : error("cannot read complete input file");
}

static int read_header(Elf *elf)
{
    if (elf->size < EI_NIDENT || memcmp(elf->data, ELFMAG, SELFMAG))
        return error("not an ELF file, or truncated ELF identification");
    if (elf->data[EI_CLASS] != ELFCLASS32 && elf->data[EI_CLASS] != ELFCLASS64)
        return error("unsupported ELF class");
    if (elf->data[EI_DATA] != ELFDATA2LSB && elf->data[EI_DATA] != ELFDATA2MSB)
        return error("unsupported ELF byte order");
    elf->is64 = elf->data[EI_CLASS] == ELFCLASS64;
    elf->little = elf->data[EI_DATA] == ELFDATA2LSB;

    unsigned header_size = elf->is64 ? 64 : 52;
    if (!in_file(elf, 0, header_size))
        return error("truncated ELF header");
    if (elf->data[EI_VERSION] != EV_CURRENT || number(elf, 20, 4) != EV_CURRENT)
        return error("unsupported ELF version");
    if (number(elf, elf->is64 ? 52 : 40, 2) != header_size)
        return error("invalid ELF header size");

    elf->phoff = number(elf, elf->is64 ? 32 : 28, elf->is64 ? 8 : 4);
    elf->phsize = (unsigned)number(elf, elf->is64 ? 54 : 42, 2);
    elf->phcount = (unsigned)number(elf, elf->is64 ? 56 : 44, 2);
    if (elf->phcount == PN_XNUM)
        return error("extended program-header numbering is not supported");
    if (elf->phcount &&
        (elf->phoff < header_size || elf->phsize != (elf->is64 ? 56U : 32U) ||
         !in_file(elf, elf->phoff, (uint64_t)elf->phsize * elf->phcount)))
        return error("invalid or truncated program-header table");
    return 1;
}

/* ELF32 and ELF64 store these fields at different offsets. */
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

/* Escape terminal control bytes in names supplied by the inspected file. */
static void print_text(const unsigned char *text)
{
    for (; *text; ++text) {
        if (*text == '\\') fputs("\\\\", stdout);
        else if (*text >= 32 && *text <= 126) putchar(*text);
        else printf("\\x%02x", (unsigned)*text);
    }
}

/* DT_STRTAB is a virtual address; translate it through a file-backed PT_LOAD. */
static int string_table(const Elf *elf, uint64_t address, uint64_t size,
                        uint64_t *offset)
{
    for (unsigned i = 0; i < elf->phcount; ++i) {
        Segment s = segment(elf, i);
        if (s.type != PT_LOAD || address < s.address) continue;
        uint64_t delta = address - s.address;
        if (delta <= s.size && size <= s.size - delta) {
            *offset = s.offset + delta;
            return 1;
        }
    }
    return error("dynamic string table is not in a file-backed load segment");
}

static int print_libraries(const Elf *elf, Segment dynamic)
{
    unsigned width = elf->is64 ? 8 : 4, stride = width * 2;
    if (!dynamic.size || dynamic.size % stride)
        return error("invalid dynamic segment size");

    uint64_t address = 0, size = 0, table = 0, end = 0;
    int has_address = 0, has_size = 0, terminated = 0, needed = 0;
    /* Find the table before resolving any DT_NEEDED string offsets. */
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

    for (uint64_t i = 0; i < end; i += stride) {
        if (number(elf, dynamic.offset + i, width) != DT_NEEDED) continue;
        uint64_t name = number(elf, dynamic.offset + i + width, width);
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
    Segment interpreter = {0}, dynamic = {0};
    int has_interpreter = 0, has_dynamic = 0;
    for (unsigned i = 0; i < elf->phcount; ++i) {
        Segment s = segment(elf, i);
        if (s.type != PT_LOAD && s.type != PT_INTERP && s.type != PT_DYNAMIC) continue;
        if (!in_file(elf, s.offset, s.size))
            return error("segment extends beyond the file");
        if (s.type == PT_LOAD && s.size > s.memory_size)
            return error("load segment file size exceeds memory size");
        if (s.type == PT_INTERP) {
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

    fputs("File:         ", stdout); print_text((const unsigned char *)path); putchar('\n');
    printf("Class:        ELF%d\n", elf->is64 ? 64 : 32);
    printf("Byte order:   %s-endian\n", elf->little ? "Little" : "Big");
    uint64_t type = number(elf, 16, 2), machine = number(elf, 18, 2);
    printf("Type:         %s (%" PRIu64 ")\n", type_name(type), type);
    printf("Architecture: %s (%" PRIu64 ")\n", machine_name(machine), machine);
    printf("OS/ABI tag:   %u (not a reliable OS detector)\n", (unsigned)elf->data[EI_OSABI]);
    printf("Entry point:  0x%" PRIx64 "\n", number(elf, 24, elf->is64 ? 8 : 4));
    printf("Interpreter:  ");
    if (has_interpreter) print_text(elf->data + interpreter.offset);
    else fputs("(none listed)", stdout);
    puts("\nLibraries (direct DT_NEEDED entries):");
    if (has_dynamic) return print_libraries(elf, dynamic);
    puts("  (no dynamic segment)");
    return 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: elf_info FILE\nInspect ELF metadata without executing FILE.");
        return EXIT_SUCCESS;
    }
    if (argc != 2) {
        fputs("Usage: elf_info FILE\n", stderr);
        return EXIT_FAILURE;
    }
    Elf elf = {0};
    int ok = read_file(&elf, argv[1]) && read_header(&elf) && inspect(&elf, argv[1]);
    free(elf.data);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
