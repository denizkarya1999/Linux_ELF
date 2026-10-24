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
#define _POSIX_C_SOURCE 200809L /* Request POSIX.1-2008 declarations before including any system headers. */
#include <float.h>    /* Floating-point precision and exponent limits. */
#include <limits.h>   /* CHAR_BIT and the signedness of plain char. */
#include <stdbool.h> /* Make bool available as the readable spelling of C's _Bool type. */
#include <stddef.h>   /* size_t, ptrdiff_t, max_align_t, and offsetof(). */
#include <stdint.h> /* Declare exact-width integer types and integer-constant macros. */
#include <stdio.h> /* Declare file I/O, formatted output, and diagnostic functions. */
#include <stdlib.h> /* Declare memory-management functions and process exit-status constants. */
#include <string.h> /* Declare string comparisons, byte comparisons, and character searches. */
#include <sys/types.h> /* off_t: file-offset type supplied by the C library. */
#include <time.h>      /* time_t: calendar-time type supplied by the C library. */
#include <wchar.h> /* Declare wide-character facilities, including wchar_t. */

/* A typedef lets sizeof and _Alignof describe a function pointer as a type.
 * C does not require function pointers and object pointers to have equal sizes. */
/* Name a pointer to a function that takes no arguments and returns no value. */
typedef void (*function_pointer)(void);

/* Same fields, different order. The compiler may insert gaps for alignment
 * and trailing padding so consecutive array elements are aligned correctly.
 * No packed attributes are used: these show the compiler's normal layout. */
struct Sample { /* Define an example with a small field before a double, allowing alignment gaps. */
    char tag; /* Declare the character member; char occupies exactly one C byte. */
    double value; /* Declare the floating-point member, whose alignment can require preceding padding. */
    int count; /* Declare the integer member that appears in both examples. */
};

struct Reordered { /* Define the same fields in another order to compare padding. */
    double value; /* Declare the floating-point member, whose alignment can require preceding padding. */
    int count; /* Declare the integer member that appears in both examples. */
    char tag; /* Declare the character member; char occupies exactly one C byte. */
};

/* Compiler macros identify the build target, which can differ from the host
 * kernel's architecture when running a 32-bit program or using emulation. */
/* Return a read-only architecture label chosen by the compiler target macros. */
static const char *architecture(void)
{
#if defined(__aarch64__) /* Select this branch when compiling for AArch64. */
    return "AArch64 (ARM64)"; /* Identify the 64-bit ARM instruction-set target. */
#elif defined(__x86_64__) /* Otherwise test whether the target is x86-64. */
    return "x86-64"; /* Identify the x86-64 instruction-set target. */
#elif defined(__arm__) /* Otherwise test for a 32-bit ARM target. */
    return "ARM"; /* Identify the ARM instruction-set target. */
#elif defined(__i386__) /* Otherwise test for a 32-bit x86 target. */
    return "x86"; /* Identify the 32-bit x86 instruction-set target. */
#elif defined(__riscv) /* Otherwise test for a RISC-V target. */
    return "RISC-V"; /* Use the family name; pointer storage width is reported separately. */
#else /* Compile this fallback branch when the preceding condition did not match. */
    return "Other/unknown"; /* Avoid guessing when no recognized target macro is present. */
#endif /* End this compile-time selection block. */
}

/* LP64 means 32-bit int, 64-bit long, and 64-bit pointer storage. Classify
 * measured widths instead of assuming that every 64-bit CPU uses LP64. */
static const char *data_model(void) /* Classify measured integer and object-pointer storage widths. */
{
    /* Convert int's size in C bytes into its storage width in bits. */
    size_t int_bits = sizeof(int) * CHAR_BIT;
    size_t long_bits = sizeof(long) * CHAR_BIT; /* Convert long's byte size into its storage width in bits. */
    /* Measure pointer storage, not the number of usable virtual-address bits. */
    size_t pointer_bits = sizeof(void *) * CHAR_BIT;
    /* Recognize the 32-bit int / 64-bit long / 64-bit pointer combination. */
    if (int_bits == 32 && long_bits == 64 && pointer_bits == 64) return "LP64";
    /* Recognize the model where int, long, and pointers all occupy 32 bits. */
    if (int_bits == 32 && long_bits == 32 && pointer_bits == 32) return "ILP32";
    /* Recognize the 32-bit int / 32-bit long / 64-bit pointer combination. */
    if (int_bits == 32 && long_bits == 32 && pointer_bits == 64) return "LLP64";
    return "Other/unclassified"; /* Keep a fallback for measured layouts outside these three models. */
}

/* Reading an object's bytes through unsigned char is permitted by C.
 * Do not cast its address to an unrelated integer type or assume host order. */
static void print_byte_order(void) /* Examine a known integer's representation and print its byte order. */
{
    /* Use four distinct byte values so their order is easy to recognize. */
    const uint32_t value = UINT32_C(0x01020304);
    /* Inspect the integer through a character pointer, which C permits. */
    const unsigned char *bytes = (const unsigned char *)&value;
    /* Expected increasing-address sequence for little-endian storage. */
    const unsigned char little[] = {4, 3, 2, 1};
    /* Expected increasing-address sequence for big-endian storage. */
    const unsigned char big[] = {1, 2, 3, 4};
    /* Keep a fallback for representations matching neither expected pattern. */
    const char *order = "Other/unknown";
    /* Apply these patterns only when there are four eight-bit C bytes. */
    if (CHAR_BIT == 8 && sizeof(value) == 4) {
        /* memcmp() returns zero when all four bytes match the little-endian pattern. */
        if (!memcmp(bytes, little, 4)) order = "Little-endian";
        /* If the first pattern differs, compare against the big-endian pattern. */
        else if (!memcmp(bytes, big, 4)) order = "Big-endian";
    }
    /* Display the classification obtained from the actual memory representation. */
    printf("Byte order:   %s\n", order);
    /* Explain the address order used by the following byte dump. */
    printf("0x01020304 in memory (lowest address first):");
    /* Print each byte as hexadecimal, advancing from the lowest address upward. */
    for (size_t i = 0; i < sizeof(value); ++i) printf(" %02x", (unsigned)bytes[i]);
    putchar('\n'); /* Finish the current output line with a newline. */
}

/* sizeof includes any padding within the type. _Alignof is the alignment
 * required by that type, not the address or observed alignment of one object.
 * #type turns the macro argument into a label; %zu prints size_t correctly. */
#define PRINT_TYPE(type) \
    printf("%-20s %6zu %10zu\n", #type, sizeof(type), _Alignof(type))

static void print_types(void) /* Print the measured size and required alignment of each selected type. */
{
    /* Start the table and state the unit used by both numeric columns. */
    puts("\nType sizes and alignment (C bytes):");
    /* Align column headings to match the rows emitted by PRINT_TYPE. */
    printf("%-20s %6s %10s\n", "Type", "Size", "Alignment");
    PRINT_TYPE(bool); /* Measure the Boolean type used for true/false values. */
    PRINT_TYPE(char); /* Measure plain char; its signedness is reported separately. */
    PRINT_TYPE(short); /* Measure the short integer type without assuming a fixed width. */
    PRINT_TYPE(int); /* Measure the ordinary signed integer type. */
    PRINT_TYPE(long); /* Measure long, whose width commonly differs between data models. */
    PRINT_TYPE(long long); /* Measure the long long integer type. */
    PRINT_TYPE(uint32_t); /* Measure an exact-width unsigned 32-bit integer. */
    PRINT_TYPE(uint64_t); /* Measure an exact-width unsigned 64-bit integer. */
    PRINT_TYPE(size_t); /* Measure the unsigned type used by sizeof and memory-size arguments. */
    PRINT_TYPE(ptrdiff_t); /* Measure the signed type used for differences between pointers into an array. */
    PRINT_TYPE(void *); /* Measure a generic object pointer rather than the object it points to. */
    PRINT_TYPE(function_pointer); /* Measure a function pointer using the typedef defined above. */
    PRINT_TYPE(float); /* Measure float storage; its precision is reported in a later table. */
    PRINT_TYPE(double); /* Measure double storage and alignment. */
    PRINT_TYPE(long double); /* Measure long double without assuming its storage size determines precision. */
    PRINT_TYPE(wchar_t); /* Measure the implementation's wide-character type. */
    PRINT_TYPE(off_t); /* Measure the file-offset type exposed by the build's headers and feature settings. */
    /* Measure the calendar-time type exposed by the build's headers and feature settings. */
    PRINT_TYPE(time_t);
    PRINT_TYPE(max_align_t); /* Measure the type whose alignment covers C's fundamental scalar alignments. */
}

/* offsetof measures each field's byte offset. Subtracting the field sizes
 * from the struct size counts all inter-field and trailing padding. Never
 * serialize raw structs assuming this padding is stable or initialized. */
static void print_layout(const char *name, size_t size, size_t alignment,
                         size_t tag, size_t value, size_t count)
{
    /* Print the structure's descriptive name, complete byte size, and alignment. */
    printf("%s: size=%zu, alignment=%zu\n", name, size, alignment);
    /* Print each member's distance in bytes from the structure's beginning. */
    printf("  Field offsets: tag=%zu, value=%zu, count=%zu\n", tag, value, count);
    /* Subtract the member sizes from the full structure size to count all padding. */
    printf("  Total padding: %zu bytes\n", size - sizeof(char) - sizeof(double) - sizeof(int));
}

/* Receive the argument count and argument array; argv[0] names this invocation. */
int main(int argc, char **argv)
{
    /* Recognize one --help argument; strcmp() returns zero for equal strings. */
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        /* Explain that this tool describes itself and does not accept a target file. */
        puts("Usage: abi_info\nReport this executable's C type layout and byte order.");
        return EXIT_SUCCESS; /* Finish successfully and report the shell's success status. */
    }
    if (argc != 1) { /* Reject extra arguments because the normal report needs none. */
        fputs("Usage: abi_info\n", stderr); /* Send an invalid-invocation message to standard error. */
        return EXIT_FAILURE; /* Finish with a failure status so the shell can detect the error. */
    }

    /* Title the report as information about this particular build. */
    puts("C ABI information for this executable");
    /* Print the target architecture selected from compiler macros. */
    printf("Architecture: %s\n", architecture());
    /* These labels identify the build environment, not the currently running
     * OS version. Android's API macro is the minimum API targeted at compile time. */
#if defined(__ANDROID__) /* Test Android first because Android also defines the Linux target macro. */
    puts("Build target: Android"); /* Label this executable as an Android-targeted build. */
    /* Print the compile-time API target, not the device's current Android API level. */
    printf("Minimum API:  %d\n", __ANDROID_API__);
#elif defined(__linux__) /* Otherwise test for a non-Android Linux build. */
    puts("Build target: Linux"); /* Name Linux without guessing which distribution is currently running. */
#else /* Compile this fallback branch when the preceding condition did not match. */
    puts("Build target: Other/unknown"); /* Use a fallback if neither known platform macro is defined. */
#endif /* End this compile-time selection block. */
#if defined(__BIONIC__) /* Check whether compilation used Android's Bionic C library headers. */
    /* Report Bionic without claiming a runtime library version. */
    puts("C library:    Bionic (build headers)");
#elif defined(__GLIBC__) /* Otherwise check for GNU C Library headers. */
    /* Print the header version, which may differ from the installed runtime version. */
    printf("C library:    glibc %d.%d (build headers)\n", __GLIBC__, __GLIBC_MINOR__);
#else /* Compile this fallback branch when the preceding condition did not match. */
    /* Avoid incorrectly labeling C libraries that define neither known macro. */
    puts("C library:    Other/unknown (build headers)");
#endif /* End this compile-time selection block. */
    /* Clang also defines GCC compatibility macros, so check Clang first. */
#if defined(__clang__) /* Prefer Clang's own identity over its GCC compatibility macros. */
    /* Print the compiler's major, minor, and patch version components. */
    printf("Compiler:     Clang %d.%d.%d\n", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__) /* Use GCC's version macros when this is not a Clang build. */
    /* Print the version reported by the compiler's GCC macros. */
    printf("Compiler:     GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif /* End this compile-time selection block. */
    printf("Data model:   %s\n", data_model()); /* Print the model inferred from the measured type sizes. */
    printf("Bits per byte: %d\n", CHAR_BIT); /* State how many bits make up one C byte on this target. */
    /* Print pointer storage in bits, using %zu for the size_t expression. */
    printf("Pointer storage: %zu bits\n", sizeof(void *) * CHAR_BIT);
    /* A negative minimum identifies signed plain char; otherwise it is unsigned. */
    printf("Plain char:   %s\n", CHAR_MIN < 0 ? "Signed" : "Unsigned");
    print_byte_order(); /* Run the byte-order probe and print the bytes supporting its result. */
    print_types(); /* Print the type-size and alignment table for this build. */

    /* Storage size alone does not reveal floating-point precision. For
     * example, two ABIs can use 16 bytes for long double but different formats.
     * MANT_DIG counts significand digits in FLT_RADIX, usually base 2. */
    /* Print the base used for floating-point significands, normally two. */
    printf("\nFloating-point radix: %d\n", FLT_RADIX);
    /* Label precision and exponent columns; the digit counts use FLT_RADIX. */
    printf("%-20s %18s %14s\n", "Type", "Significand digits", "Max exponent");
    /* Print float precision and the implementation's maximum exponent parameter. */
    printf("%-20s %18d %14d\n", "float", FLT_MANT_DIG, FLT_MAX_EXP);
    /* Print the corresponding double precision and exponent parameter. */
    printf("%-20s %18d %14d\n", "double", DBL_MANT_DIG, DBL_MAX_EXP);
    /* Reveal precision differences that equal long-double storage sizes can hide. */
    printf("%-20s %18d %14d\n", "long double", LDBL_MANT_DIG, LDBL_MAX_EXP);

    /* Start the comparison of the two ordinary, unpacked structures. */
    puts("\nStructure layout (C bytes):");
    /* Pass the first structure's description to the layout printer. */
    print_layout("Sample { char tag; double value; int count; }",
                 /* Pass its complete byte size and required alignment. */
                 sizeof(struct Sample), _Alignof(struct Sample),
                 /* Pass the offsets of the character and floating-point members. */
                 offsetof(struct Sample, tag), offsetof(struct Sample, value),
                 /* Pass the integer member's offset and complete the function call. */
                 offsetof(struct Sample, count));
    /* Print the same fields reordered with the double first. */
    print_layout("Reordered { double value; int count; char tag; }",
                 /* Measure the reordered type instead of assuming its size stayed the same. */
                 sizeof(struct Reordered), _Alignof(struct Reordered),
                 /* Locate the same named fields even though their declaration order changed. */
                 offsetof(struct Reordered, tag), offsetof(struct Reordered, value),
                 /* Supply the remaining offset to complete the second structure report. */
                 offsetof(struct Reordered, count));

    /* State the experiment's limit: data layout describes only part of an ABI. */
    puts("\nMatching data layouts alone do not guarantee binary compatibility.");
    return EXIT_SUCCESS; /* Finish successfully and report the shell's success status. */
}
