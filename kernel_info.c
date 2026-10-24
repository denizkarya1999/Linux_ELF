/* Build separately for Android or GNU/Linux. */
#define _POSIX_C_SOURCE 200809L /* Request POSIX.1-2008 declarations before including any system headers. */
#include <stdio.h> /* Declare file I/O, formatted output, and diagnostic functions. */
#include <stdlib.h> /* Declare memory-management functions and process exit-status constants. */
#include <string.h> /* Declare string comparisons, byte comparisons, and character searches. */
#include <sys/utsname.h> /* Declare uname() and the structure it fills with kernel information. */
#ifdef __ANDROID__ /* Compile the following branch only for an Android-targeted build. */
/* Declare Android property access and the maximum property-value buffer size. */
#include <sys/system_properties.h>
#else /* Compile this fallback branch when the preceding condition did not match. */
/* Read a distribution setting without executing os-release as a script. */
/* Look up one key and write into the caller's buffer; its capacity must be at least one byte. */
static void os_value(const char *key, char *out, size_t size)
{
    /* Open the preferred distribution-information file for reading. */
    FILE *file = fopen("/etc/os-release", "r");
    /* If that open fails, try the vendor's fallback location. */
    if (!file) file = fopen("/usr/lib/os-release", "r");
    if (!file) return; /* Leave the caller's existing fallback value intact if neither file opens. */

    char *line = NULL; /* Let getline() allocate a line buffer; NULL means no buffer exists yet. */
    /* Track the buffer capacity and the number of key characters to compare. */
    size_t capacity = 0, key_length = strlen(key);
    /* Read a complete line, growing the buffer if needed, until EOF or a read error. */
    while (getline(&line, &capacity, file) != -1) {
        /* Require the exact key followed by '='; a matching prefix alone is insufficient. */
        if (strncmp(line, key, key_length) || line[key_length] != '=')
            continue; /* Skip this nonmatching line and read the next one. */

        char *value = line + key_length + 1; /* Point past the key and '=' to the beginning of the value. */
        /* Remember and skip an opening quote, or use zero if the value is unquoted. */
        char quote = (*value == '\'' || *value == '"') ? *value++ : 0;
        size_t n = 0; /* Count the characters copied into the output buffer. */
        /* Remove quotes and escapes, leaving room for the final null byte. */
        /* Stop at end-of-text, a line ending, or the last byte reserved for the null terminator. */
        while (*value && *value != '\n' && *value != '\r' && n + 1 < size) {
            if (quote && *value == quote) break; /* Stop when the matching closing quote is reached. */
            /* Consider an escape only outside single quotes and when another character follows. */
            if (*value == '\\' && quote != '\'' && value[1] &&
                /* Skip the escaping backslash only where the quoting rules permit it. */
                (!quote || strchr("\"\\$`", value[1]))) ++value;
            /* Copy one character, then advance both the output index and the input pointer. */
            out[n++] = *value++;
        }
        out[n] = '\0'; /* End the result as a C string; later duplicate keys can replace this result. */
    }
    free(line); /* Release the buffer allocated or resized by getline(). */
    fclose(file); /* Close the opened file handle and release its associated resources. */
}
#endif /* End this compile-time selection block. */

int main(void) /* Begin the executable; this program does not read command-line arguments. */
{
    /* uname provides kernel information on both platforms. */
    struct utsname kernel; /* Reserve storage for the fields uname() will fill. */
    /* Pass the structure's address and detect a failed kernel-information request. */
    if (uname(&kernel) == -1) {
        perror("uname"); /* Print a label followed by the system's explanation of the failure. */
        return EXIT_FAILURE; /* Finish with a failure status so the shell can detect the error. */
    }

#ifdef __ANDROID__ /* Compile the following branch only for an Android-targeted build. */
    char name[] = "Android"; /* Use Android as the OS name for the Android build. */
    /* Start both property buffers empty so missing values have a predictable fallback. */
    char version[PROP_VALUE_MAX] = "", sdk[PROP_VALUE_MAX] = "";
    /* Read the phone's Android release string into version. */
    __system_property_get("ro.build.version.release", version);
    __system_property_get("ro.build.version.sdk", sdk); /* Read the phone's Android API level into sdk. */
#else /* Compile this fallback branch when the preceding condition did not match. */
    /* Supply a default OS name and an initially unknown version. */
    char name[1024] = "GNU/Linux", version[1024] = "";
    os_value("NAME", name, sizeof(name)); /* Replace the default name when the distribution supplies NAME. */
    /* Read the descriptive version string, including a codename if present. */
    os_value("VERSION", version, sizeof(version));
    /* When VERSION is empty, try the shorter VERSION_ID value. */
    if (!version[0]) os_value("VERSION_ID", version, sizeof(version));
#endif /* End this compile-time selection block. */

    /* Print the OS name, falling back to GNU/Linux if the string is empty. */
    printf("OS:           %s\n", name[0] ? name : "GNU/Linux");
    /* Print the version or an explicit Unknown label instead of a blank value. */
    printf("OS version:   %s\n", version[0] ? version : "Unknown");
#ifdef __ANDROID__ /* Compile the following branch only for an Android-targeted build. */
    /* Print the Android device's API level when available. */
    printf("API level:    %s\n", sdk[0] ? sdk : "Unknown");
#endif /* End this compile-time selection block. */
    /* Print the kernel name and release returned by uname(). */
    printf("Kernel:       %s %s\n", kernel.sysname, kernel.release);
    /* Print the kernel build description, not the distribution version. */
    printf("Kernel build: %s\n", kernel.version);
    /* Print the machine architecture as reported by the running kernel. */
    printf("Architecture: %s\n", kernel.machine);
    return EXIT_SUCCESS; /* Finish successfully and report the shell's success status. */
}
