/* Build separately for Android or GNU/Linux. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#else
/* Read a distribution setting without executing os-release as a script. */
static void os_value(const char *key, char *out, size_t size)
{
    FILE *file = fopen("/etc/os-release", "r");
    if (!file) file = fopen("/usr/lib/os-release", "r");
    if (!file) return;

    char *line = NULL;
    size_t capacity = 0, key_length = strlen(key);
    while (getline(&line, &capacity, file) != -1) {
        if (strncmp(line, key, key_length) || line[key_length] != '=')
            continue;

        char *value = line + key_length + 1;
        char quote = (*value == '\'' || *value == '"') ? *value++ : 0;
        size_t n = 0;
        /* Remove quotes and escapes, leaving room for the final null byte. */
        while (*value && *value != '\n' && *value != '\r' && n + 1 < size) {
            if (quote && *value == quote) break;
            if (*value == '\\' && quote != '\'' && value[1] &&
                (!quote || strchr("\"\\$`", value[1]))) ++value;
            out[n++] = *value++;
        }
        out[n] = '\0';
    }
    free(line);
    fclose(file);
}
#endif

int main(void)
{
    /* uname provides kernel information on both platforms. */
    struct utsname kernel;
    if (uname(&kernel) == -1) {
        perror("uname");
        return EXIT_FAILURE;
    }

#ifdef __ANDROID__
    char name[] = "Android";
    char version[PROP_VALUE_MAX] = "", sdk[PROP_VALUE_MAX] = "";
    __system_property_get("ro.build.version.release", version);
    __system_property_get("ro.build.version.sdk", sdk);
#else
    char name[1024] = "GNU/Linux", version[1024] = "";
    os_value("NAME", name, sizeof(name));
    os_value("VERSION", version, sizeof(version));
    if (!version[0]) os_value("VERSION_ID", version, sizeof(version));
#endif

    printf("OS:           %s\n", name[0] ? name : "GNU/Linux");
    printf("OS version:   %s\n", version[0] ? version : "Unknown");
#ifdef __ANDROID__
    printf("API level:    %s\n", sdk[0] ? sdk : "Unknown");
#endif
    printf("Kernel:       %s %s\n", kernel.sysname, kernel.release);
    printf("Kernel build: %s\n", kernel.version);
    printf("Architecture: %s\n", kernel.machine);
    return EXIT_SUCCESS;
}
