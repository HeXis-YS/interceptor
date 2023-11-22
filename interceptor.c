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

    int name_len = strlen(argv[0]);
    char *name_end = argv[0] + name_len;
    int gcc_wrapper = 0;
    int gcc_flags = 0;
    if ((name_len >= 6) && (strcmp(name_end - 6, "ranlib") == 0)) { // Match ranlib
        gcc_wrapper = 6;
    } else if ((name_len >= 3) && ((strcmp(name_end - 3, "gcc") == 0) || (strcmp(name_end - 3, "g++") == 0) || (strcmp(name_end - 3, "c++") == 0))) { // Match gcc, g++, c++
        gcc_flags = 1;
    } else if (name_len >= 2) {
        if ((strcmp(name_end - 2, "cc") == 0)) { // Match cc
            gcc_flags = 1;
        } else if ((strcmp(name_end - 2, "ar") == 0) || (strcmp(name_end - 2, "nm") == 0)) { // Match ar and nm
            gcc_wrapper = 2;
        }
    }

    if (gcc_flags != 0) {
        int new_argc = 0;
        char *new_argv[1024];

        for (int i = 0; argv[i] != NULL; i++) {
            // Remove -O*, -march and -mtune
            if ((strncmp(argv[i], "-O", 2) == 0) || (strncmp(argv[i], "-march=", 7) == 0) || (strncmp(argv[i], "-mtune=", 7) == 0)) {
                continue;
            }
            new_argv[new_argc++] = argv[i];
        }

        // Add new arguments
        new_argv[new_argc++] = "-march=native";
        new_argv[new_argc++] = "-mtune=native";
        new_argv[new_argc++] = "-O3";
        new_argv[new_argc++] = "-flto";
        new_argv[new_argc++] = "-fgraphite-identity";
        new_argv[new_argc++] = "-floop-nest-optimize";
        new_argv[new_argc++] = "-fipa-pta";
        new_argv[new_argc++] = "-fno-semantic-interposition";
        new_argv[new_argc++] = "-fno-common";
        new_argv[new_argc++] = "-fdevirtualize-at-ltrans";
        new_argv[new_argc++] = "-fno-plt";
        new_argv[new_argc] = NULL;

        original_function_type original_function = dlsym(RTLD_NEXT, "execve");
        return original_function(pathname, new_argv, envp);
    } else if (gcc_wrapper != 0) {
        int pathname_len = strlen(pathname);
        char *new_pathname = (char *)malloc(pathname_len + 5);
        if (new_pathname == NULL) {
            perror("Memory allocation failed");
            exit(EXIT_FAILURE);
        }

        memset(new_pathname, '\0', pathname_len + 5);
        strncpy(new_pathname, pathname, pathname_len - gcc_wrapper);
        strcat(new_pathname, "gcc-");
        strncat(new_pathname, pathname + pathname_len - gcc_wrapper, gcc_wrapper);

        if (access(new_pathname, F_OK) == 0) {
            original_function_type original_function = dlsym(RTLD_NEXT, "execve");
            return original_function(new_pathname, argv, envp);
        }
    }

    original_function_type original_function = dlsym(RTLD_NEXT, "execve");
    return original_function(pathname, argv, envp);
}
