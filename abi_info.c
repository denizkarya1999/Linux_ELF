/*
 * Report the C data layout used by THIS executable on GNU/Linux or Android.
 * ABI means Application Binary Interface: the rules compiled code follows
 * when sharing data and calling other compiled code.
 *
 * This tool measures part of an ABI: type sizes, alignment, byte order, and
 * structure layout. Matching these values does not prove binary compatibility;
 * CPU instructions, calling conventions, loaders, and libraries also matter.
 * Build and run a separate executable on each target, then compare reports.
 */
#define _POSIX_C_SOURCE 200809L
#include <float.h>    /* Floating-point precision and exponent limits. */
#include <limits.h>   /* CHAR_BIT and the signedness of plain char. */
#include <stdbool.h>
#include <stddef.h>   /* size_t, ptrdiff_t, max_align_t, and offsetof(). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> /* off_t: file-offset type supplied by the C library. */
#include <time.h>      /* time_t: calendar-time type supplied by the C library. */
#include <wchar.h>

/* A typedef lets sizeof and _Alignof describe a function pointer as a type.
 * C does not require function pointers and object pointers to have equal sizes. */
typedef void (*function_pointer)(void);

/* Same fields, different order. The compiler may insert gaps for alignment
 * and trailing padding so consecutive array elements are aligned correctly.
 * No packed attributes are used: these show the compiler's normal layout. */
struct Sample {
    char tag;
    double value;
    int count;
};

struct Reordered {
    double value;
    int count;
    char tag;
};

/* Compiler macros identify the build target, which can differ from the host
 * kernel's architecture when running a 32-bit program or using emulation. */
static const char *architecture(void)
{
#if defined(__aarch64__)
    return "AArch64 (ARM64)";
#elif defined(__x86_64__)
    return "x86-64";
#elif defined(__arm__)
    return "ARM";
#elif defined(__i386__)
    return "x86";
#elif defined(__riscv)
    return "RISC-V";
#else
    return "Other/unknown";
#endif
}

/* LP64 means 32-bit int, 64-bit long, and 64-bit pointer storage. Classify
 * measured widths instead of assuming that every 64-bit CPU uses LP64. */
static const char *data_model(void)
{
    size_t int_bits = sizeof(int) * CHAR_BIT;
    size_t long_bits = sizeof(long) * CHAR_BIT;
    size_t pointer_bits = sizeof(void *) * CHAR_BIT;
    if (int_bits == 32 && long_bits == 64 && pointer_bits == 64) return "LP64";
    if (int_bits == 32 && long_bits == 32 && pointer_bits == 32) return "ILP32";
    if (int_bits == 32 && long_bits == 32 && pointer_bits == 64) return "LLP64";
    return "Other/unclassified";
}

/* Reading an object's bytes through unsigned char is permitted by C.
 * Do not cast its address to an unrelated integer type or assume host order. */
static void print_byte_order(void)
{
    const uint32_t value = UINT32_C(0x01020304);
    const unsigned char *bytes = (const unsigned char *)&value;
    const unsigned char little[] = {4, 3, 2, 1};
    const unsigned char big[] = {1, 2, 3, 4};
    const char *order = "Other/unknown";
    if (CHAR_BIT == 8 && sizeof(value) == 4) {
        if (!memcmp(bytes, little, 4)) order = "Little-endian";
        else if (!memcmp(bytes, big, 4)) order = "Big-endian";
    }
    printf("Byte order:   %s\n", order);
    printf("0x01020304 in memory (lowest address first):");
    for (size_t i = 0; i < sizeof(value); ++i) printf(" %02x", (unsigned)bytes[i]);
    putchar('\n');
}

/* sizeof includes any padding within the type. _Alignof is the alignment
 * required by that type, not the address or observed alignment of one object.
 * #type turns the macro argument into a label; %zu prints size_t correctly. */
#define PRINT_TYPE(type) \
    printf("%-20s %6zu %10zu\n", #type, sizeof(type), _Alignof(type))

static void print_types(void)
{
    puts("\nType sizes and alignment (C bytes):");
    printf("%-20s %6s %10s\n", "Type", "Size", "Alignment");
    PRINT_TYPE(bool);
    PRINT_TYPE(char);
    PRINT_TYPE(short);
    PRINT_TYPE(int);
    PRINT_TYPE(long);
    PRINT_TYPE(long long);
    PRINT_TYPE(uint32_t);
    PRINT_TYPE(uint64_t);
    PRINT_TYPE(size_t);
    PRINT_TYPE(ptrdiff_t);
    PRINT_TYPE(void *);
    PRINT_TYPE(function_pointer);
    PRINT_TYPE(float);
    PRINT_TYPE(double);
    PRINT_TYPE(long double);
    PRINT_TYPE(wchar_t);
    PRINT_TYPE(off_t);
    PRINT_TYPE(time_t);
    PRINT_TYPE(max_align_t);
}

/* offsetof measures each field's byte offset. Subtracting the field sizes
 * from the struct size counts all inter-field and trailing padding. Never
 * serialize raw structs assuming this padding is stable or initialized. */
static void print_layout(const char *name, size_t size, size_t alignment,
                         size_t tag, size_t value, size_t count)
{
    printf("%s: size=%zu, alignment=%zu\n", name, size, alignment);
    printf("  Field offsets: tag=%zu, value=%zu, count=%zu\n", tag, value, count);
    printf("  Total padding: %zu bytes\n", size - sizeof(char) - sizeof(double) - sizeof(int));
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: abi_info\nReport this executable's C type layout and byte order.");
        return EXIT_SUCCESS;
    }
    if (argc != 1) {
        fputs("Usage: abi_info\n", stderr);
        return EXIT_FAILURE;
    }

    puts("C ABI information for this executable");
    printf("Architecture: %s\n", architecture());
    /* These labels identify the build environment, not the currently running
     * OS version. Android's API macro is the minimum API targeted at compile time. */
#if defined(__ANDROID__)
    puts("Build target: Android");
    printf("Minimum API:  %d\n", __ANDROID_API__);
#elif defined(__linux__)
    puts("Build target: Linux");
#else
    puts("Build target: Other/unknown");
#endif
#if defined(__BIONIC__)
    puts("C library:    Bionic (build headers)");
#elif defined(__GLIBC__)
    printf("C library:    glibc %d.%d (build headers)\n", __GLIBC__, __GLIBC_MINOR__);
#else
    puts("C library:    Other/unknown (build headers)");
#endif
    /* Clang also defines GCC compatibility macros, so check Clang first. */
#if defined(__clang__)
    printf("Compiler:     Clang %d.%d.%d\n", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    printf("Compiler:     GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif
    printf("Data model:   %s\n", data_model());
    printf("Bits per byte: %d\n", CHAR_BIT);
    printf("Pointer storage: %zu bits\n", sizeof(void *) * CHAR_BIT);
    printf("Plain char:   %s\n", CHAR_MIN < 0 ? "Signed" : "Unsigned");
    print_byte_order();
    print_types();

    /* Storage size alone does not reveal floating-point precision. For
     * example, two ABIs can use 16 bytes for long double but different formats.
     * MANT_DIG counts significand digits in FLT_RADIX, usually base 2. */
    printf("\nFloating-point radix: %d\n", FLT_RADIX);
    printf("%-20s %18s %14s\n", "Type", "Significand digits", "Max exponent");
    printf("%-20s %18d %14d\n", "float", FLT_MANT_DIG, FLT_MAX_EXP);
    printf("%-20s %18d %14d\n", "double", DBL_MANT_DIG, DBL_MAX_EXP);
    printf("%-20s %18d %14d\n", "long double", LDBL_MANT_DIG, LDBL_MAX_EXP);

    puts("\nStructure layout (C bytes):");
    print_layout("Sample { char tag; double value; int count; }",
                 sizeof(struct Sample), _Alignof(struct Sample),
                 offsetof(struct Sample, tag), offsetof(struct Sample, value),
                 offsetof(struct Sample, count));
    print_layout("Reordered { double value; int count; char tag; }",
                 sizeof(struct Reordered), _Alignof(struct Reordered),
                 offsetof(struct Reordered, tag), offsetof(struct Reordered, value),
                 offsetof(struct Reordered, count));

    puts("\nMatching data layouts alone do not guarantee binary compatibility.");
    return EXIT_SUCCESS;
}
