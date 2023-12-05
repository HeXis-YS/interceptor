#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Build:
// gcc -shared -fPIC -o interceptor.so interceptor.c -ldl
// Usage:
// LD_PRELOAD=$(realpath ./interceptor.so) LD_LIBRARY_PATH=$(realpath .) your_program

#define LOG_FILE "/tmp/interceptor.log"
#define LTO_PLUGIN_PATH "/usr/lib/bfd-plugins/liblto_plugin.so"

// Pointer of original function
typedef int (*execve_type)(const char *, char *const[], char *const[]);

char *const gcc_compiler_list[] = {"gcc", "g++", "c++", "cc", "xgcc", "xg++", NULL};
char *const binutils_list[] = {"ar", "nm", "ranlib", NULL};
char *const new_list[] = {"nm-new", NULL};

int match_list(const char *str, char *const list[]) {
    int i = 0;
    while (list[i] != NULL) {
        if (strcmp(str, list[i]) == 0) {
            return i + 1;
        }
        i++;
    }
    return 0;
}

char *insert_wrapper(const char *str1, const char *str2, int index) {
    int str1_len = strlen(str1);
    int str2_len = strlen(str2);
    int pos = str1_len - strlen(binutils_list[index - 1]);
    char *res = (char *)malloc(str1_len + str2_len + 1);
    if (res == NULL) {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    memset(res, '\0', str1_len + str2_len + 1);
    strncpy(res, str1, str1_len);
    strcat(res, str2);
    strncat(res, str1 + str1_len, strlen(binutils_list[index - 1]));

    return res;
}

char *basename_char(const char *path, const char delimiter) {
    char *last_char = strrchr(path, delimiter);
    if (last_char != NULL) {
        return last_char + 1;
    }
    return (char *)path;
}

// Modified execve function
int process(const char *path, char *const argv[], char *const envp[], const char *func) {
#ifdef LOG_FILE
    FILE *output_file = fopen(LOG_FILE, "a");
    if (output_file != NULL) {
        fprintf(output_file, "[%s] \"%s\" {", func, path);
        for (int i = 0; argv[i] != NULL; i++) {
            fprintf(output_file, " \"%s\"", argv[i]);
        }
        fprintf(output_file, " }\n");
        fclose(output_file);
    } else {
        perror("Error opening output file");
    }
#endif

    execve_type original_function = dlsym(RTLD_NEXT, func);
    const char *basemame_slash = basename_char(path, '/');
    const char *basename_dash = basename_char(basemame_slash, '-');
    int is_execve = 0;
    int gcc_wrapper = 0;
    int gcc_flags = 0;
    int new_bin = 0;
    if (strcmp(func, "execve") == 0) {
        is_execve = 1;
        gcc_flags = match_list(basename_dash, gcc_compiler_list);
    }
    gcc_wrapper = match_list(basename_dash, binutils_list);
    new_bin = match_list(basemame_slash, new_list);

    int new_argc;
    char *new_argv[1024];
    if (gcc_flags) {
        new_argv[0] = argv[0];
        new_argc = 1;

        for (int i = 1; argv[i] != NULL; i++) {
            // Remove -O*, -march and -mtune
            if ((strncmp(argv[i], "-O", 2) == 0) || (strncmp(argv[i], "-march=", 7) == 0) || (strncmp(argv[i], "-mtune=", 7) == 0)) {
                continue;
            }
            new_argv[new_argc++] = argv[i];
        }

        // Add new arguments
        new_argv[new_argc++] = "-pipe";
        new_argv[new_argc++] = "-Wno-error";
        new_argv[new_argc++] = "-march=native";
        new_argv[new_argc++] = "-mtune=native";
        new_argv[new_argc++] = "-O3";
        new_argv[new_argc++] = "-flto";
        new_argv[new_argc++] = "-fno-fat-lto-objects";
        new_argv[new_argc++] = "-flto-partition=none";
        new_argv[new_argc++] = "-flto-compression-level=0";
        new_argv[new_argc++] = "-fuse-linker-plugin";
        if (gcc_flags >= 5) {
            new_argv[new_argc++] = "-fuse-ld=gold";
        }
        new_argv[new_argc++] = "-fgraphite-identity";
        new_argv[new_argc++] = "-floop-nest-optimize";
        new_argv[new_argc++] = "-fipa-pta";
        new_argv[new_argc++] = "-fno-semantic-interposition";
        new_argv[new_argc++] = "-fno-common";
        new_argv[new_argc++] = "-fdevirtualize-at-ltrans";
        new_argv[new_argc++] = "-fno-plt";
        new_argv[new_argc] = NULL;

        return original_function(path, new_argv, envp);
    } else if (gcc_wrapper && (strncmp(basename_dash - 4, "gcc-", 4) != 0)) {
        if (is_execve && !new_bin) {
            char *new_pathname = insert_wrapper(path, "gcc-", gcc_wrapper);
            if (access(new_pathname, F_OK) == 0) { // Check if gcc wrapper is available
                // If gcc wrapper is available, also modify argv[0]
                new_argv[0] = insert_wrapper(argv[0], "gcc-", gcc_wrapper);
                new_argc = 1;
                for (int i = 1; argv[i] != NULL; i++) { // Copy argv[]
                    new_argv[new_argc++] = argv[i];
                }
                new_argv[new_argc] = NULL;

                return original_function(new_pathname, new_argv, envp);
            }
        }

        // if the call is implicit call
        int plugin_available = 0;
        new_argv[0] = argv[0];
        new_argc = 1;
        for (int i = 1; argv[i] != NULL; i++) { // Copy argv
            new_argv[new_argc++] = argv[i];
            if (plugin_available || (strcmp(argv[i], "--plugin") != 0)) { // Check if plugin is specified in the arguments
                continue;
            }
            if (access(argv[++i], F_OK) == 0) { // Check if plugin is available
                new_argv[new_argc++] = argv[i]; // If availavle, the copy it
            } else {
                new_argv[new_argc++] = LTO_PLUGIN_PATH; // If not available, use predefined
            }
            plugin_available = 1;
        }

        if (!plugin_available) { // If plugin is not specified, add it manually
            new_argv[new_argc++] = "--plugin";
            new_argv[new_argc++] = LTO_PLUGIN_PATH;
        }
        new_argv[new_argc] = NULL;
        return original_function(path, new_argv, envp);
    }

    return original_function(path, argv, envp);
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    return process(pathname, argv, envp, __func__);
}

int execvp(const char *file, char *const argv[]) {
    return process(file, argv, NULL, __func__);
}