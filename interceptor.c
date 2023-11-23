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

// Pointer of original function
typedef int (*original_function_type)(const char *, char *const[], char *const[]);

char *strinsert(const char *str1, const char *str2, int pos) {
    int str1_len = strlen(str1);
    int str2_len = strlen(str2);
    char *res = (char *)malloc(str1_len + str2_len + 1);
    if (res == NULL) {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    memset(res, '\0', str1_len + str2_len + 1);
    strncpy(res, str1, str1_len - pos);
    strcat(res, str2);
    strncat(res, str1 + str1_len - pos, pos);

    return res;
}

// Modified execve function
int execve(const char *pathname, char *const argv[], char *const envp[]) {
#ifdef DEBUG
#ifdef LOG_FILE
    FILE *output_file = fopen(LOG_FILE, "a");
    if (output_file != NULL) {
#else  // #ifdef LOG_FILE
    FILE *output_file = stdout;
#endif // #ifdef LOG_FILE
#ifdef VERBOSE
        fprintf(output_file, "Pathname: %s\n", pathname);
        fprintf(output_file, "Arguments:\n");
        for (int i = 0; argv[i] != NULL; i++) {
            fprintf(output_file, "  argv[%d]: %s\n", i, argv[i]);
        }
        fprintf(output_file, "Environment:\n");
        for (int i = 0; envp[i] != NULL; i++) {
            fprintf(output_file, "  envp[%d]: %s\n", i, envp[i]);
        }
#else  // #ifdef VERBOSE
    fprintf(output_file, "%s", pathname);
    for (int i = 0; argv[i] != NULL; i++) {
        fprintf(output_file, " %s", argv[i]);
    }
#endif // #ifdef VERBOSE
        fprintf(output_file, "\n");
#ifdef LOG_FILE
        fclose(output_file);
    } else {
        perror("Error opening output file");
    }
#endif // #ifdef LOG_FILE
#endif // #ifdef DEBUG

    original_function_type original_function = dlsym(RTLD_NEXT, "execve");
    int name_len = strlen(argv[0]);
    char *name_end = argv[0] + name_len;
    int gcc_wrapper = 0;
    int gcc_flags = 0;
    if ((name_len >= 6) && (strcmp(name_end - 6, "ranlib") == 0)) { // Match ranlib
        gcc_wrapper = 6;
    } else if ((name_len >= 4) && ((strcmp(name_end - 4, "xgcc") == 0) || (strcmp(name_end - 4, "xg++") == 0))) {
        gcc_flags = 2;
    } else if ((name_len >= 3) && ((strcmp(name_end - 3, "gcc") == 0) || (strcmp(name_end - 3, "g++") == 0) || (strcmp(name_end - 3, "c++") == 0))) { // Match gcc, g++, c++
        gcc_flags = 1;
    } else if (name_len >= 2) {
        if ((strcmp(name_end - 2, "cc") == 0)) { // Match cc
            gcc_flags = 1;
        } else if ((strcmp(name_end - 2, "ar") == 0) || (strcmp(name_end - 2, "nm") == 0)) { // Match ar and nm
            gcc_wrapper = 2;
        }
    }

    int new_argc;
    char *new_argv[1024];
    if (gcc_flags != 0) {
        new_argc = 0;

        for (int i = 0; argv[i] != NULL; i++) {
            // Remove -O*, -march and -mtune
            if ((strncmp(argv[i], "-O", 2) == 0) || (strncmp(argv[i], "-march=", 7) == 0) || (strncmp(argv[i], "-mtune=", 7) == 0)) {
                continue;
            }
            new_argv[new_argc++] = argv[i];
        }

        // Add new arguments
        new_argv[new_argc++] = "-Wno-error";
        new_argv[new_argc++] = "-march=native";
        new_argv[new_argc++] = "-mtune=native";
        new_argv[new_argc++] = "-O3";
        new_argv[new_argc++] = "-flto";
        new_argv[new_argc++] = "-flto-partition=one";
        if (gcc_flags != 2) {
            new_argv[new_argc++] = "-fuse-ld=gold";
            new_argv[new_argc++] = "-fuse-linker-plugin";
        }
        new_argv[new_argc++] = "-fgraphite-identity";
        new_argv[new_argc++] = "-floop-nest-optimize";
        new_argv[new_argc++] = "-fipa-pta";
        new_argv[new_argc++] = "-fno-semantic-interposition";
        new_argv[new_argc++] = "-fno-common";
        new_argv[new_argc++] = "-fdevirtualize-at-ltrans";
        new_argv[new_argc++] = "-fno-plt";
        new_argv[new_argc] = NULL;

        return original_function(pathname, new_argv, envp);
    } else if (gcc_wrapper != 0) {
        char *new_pathname = strinsert(pathname, "-gcc", gcc_wrapper);

        if (access(new_pathname, F_OK) == 0) {
            new_argc = 1;
            char *new_argv0 = strinsert(argv[0], "-gcc", gcc_wrapper);

            new_argv[0] = new_argv0;
            do {
                new_argv[new_argc] = argv[new_argc];
            } while (argv[new_argc++] != NULL);

            return original_function(new_pathname, new_argv, envp);
        }
    }

    return original_function(pathname, argv, envp);
}
