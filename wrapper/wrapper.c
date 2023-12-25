#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pex.h"

#define MAX_NEW_ARGV 32
#define LTO_PLUGIN_PATH "/usr/lib/bfd-plugins/liblto_plugin.so"

char *const gcc_compiler_list[] = {"gcc", "g++", "c++", "cc", "xgcc", "xg++", NULL};
char *const binutils_list[] = {"ar", "nm", "ranlib", NULL};
char *const binutils_new_list[] = {"nm-new", NULL};

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
    if (!res) {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    memset(res, '\0', str1_len + str2_len + 1);
    strncpy(res, str1, pos);
    strcat(res, str2);
    strcat(res, str1 + pos);

    return res;
}

char *get_basename(const char *path, const char delimiter) {
    char *last_char = strrchr(path, delimiter);
    if (last_char != NULL) {
        return last_char + 1;
    }
    return (char *)path;
}

int file_exist(const char *path) {
    int res = 0;
    if (access(path, F_OK) == 0) {
        res++;
        if (access(path, X_OK) == 0) {
            res++;
        }
    }
    return res;
}

int main(int argc, char *argv[], char *envp[]) {
    // for (int i = 0; i < argc; i++) {
    //     printf("%s\n", argv[i]);
    // }
    // for (int i = 0; envp[i]; i++) {
    //     printf("%s\n", envp[i]);
    // }
    char *pathname = argv[0];
    char *new_pathname = argv[0];
    argv++;
    argc--;
    const char *basemame_slash = get_basename(pathname, '/');
    const char *basename_dash = get_basename(basemame_slash, '-');

    int gcc_compiler = match_list(basename_dash, gcc_compiler_list);
    int binutils = match_list(basename_dash, binutils_list);
    int binutils_new = match_list(basemame_slash, binutils_new_list);

    int new_argc = 0;
    char **new_argv = malloc((argc + MAX_NEW_ARGV) * sizeof(char *));
    if (binutils || binutils_new) {
        int lto_plugin_available = 0;
        for (int i = 0; i < argc && argv[i]; i++) {
            // fprintf(stderr,"%d %s\n", i, argv[i]);
            if (strcmp(argv[i], "--plugin") == 0) {
                if (argv[i + 1] == NULL) {
                    continue;
                }
                if (file_exist(argv[i + 1])) {
                    char *plugin_basename = get_basename(argv[i + 1], '/');
                    if (strncmp(plugin_basename, "liblto_plugin.so", 16) == 0) {
                        lto_plugin_available = 1;
                    }
                    new_argv[new_argc++] = argv[i++];
                } else {
                    i++;
                    continue;
                }
            }
            new_argv[new_argc++] = argv[i];
        }

        if (!lto_plugin_available) {
            char *wrapper_pathname = insert_wrapper(pathname, "gcc-", binutils);
            if (!binutils_new && (file_exist(wrapper_pathname) == 2)) { // Check if gcc wrapper is available
                new_pathname = wrapper_pathname;
                // If gcc wrapper is available, also modify argv[0]
                new_argv[0] = insert_wrapper(argv[0], "gcc-", binutils);
            } else {
                free(wrapper_pathname);
                new_argv[new_argc++] = "--plugin";
                new_argv[new_argc++] = LTO_PLUGIN_PATH;
            }
        }
        new_argv[new_argc] = NULL;
    } else if (gcc_compiler) {
        for (int i = 0; i < argc && argv[i]; i++) {
            if (strcmp(argv[i], "-O4") == 0) {
                new_argc = 0;
                goto skip_interception;
            }
            // Remove -O*, -march and -mtune
            if ((strcmp(argv[i], "-pipe") == 0) ||
                (strncmp(argv[i], "-O", 2) == 0) ||
                (strncmp(argv[i], "-march=", 7) == 0) ||
                (strncmp(argv[i], "-mtune=", 7) == 0)) {
                continue;
            }
            new_argv[new_argc++] = argv[i];
        }

        // Add new arguments
        // new_argv[new_argc++] = "-pipe";
        new_argv[new_argc++] = "-Wno-error";
        new_argv[new_argc++] = "-march=native";
        new_argv[new_argc++] = "-mtune=native";
        new_argv[new_argc++] = "-O4";
        new_argv[new_argc++] = "-flto";
        new_argv[new_argc++] = "-fno-fat-lto-objects";
        // new_argv[new_argc++] = "-flto-partition=none";
        new_argv[new_argc++] = "-flto-compression-level=0";
        new_argv[new_argc++] = "-fuse-linker-plugin";
        // if (gcc_compiler < 5) {
        //     new_argv[new_argc++] = "-fuse-ld=gold";
        // }
        new_argv[new_argc++] = "-fgraphite-identity";
        new_argv[new_argc++] = "-floop-nest-optimize";
        new_argv[new_argc++] = "-fipa-pta";
        new_argv[new_argc++] = "-fno-semantic-interposition";
        new_argv[new_argc++] = "-fno-common";
        new_argv[new_argc++] = "-fdevirtualize-at-ltrans";
        new_argv[new_argc++] = "-fno-plt";
        // new_argv[new_argc++] = "-ffunction-sections";
        // new_argv[new_argc++] = "-fdata-sections";
        // new_argv[new_argc++] = "-Wl,--gc-sections";
        new_argv[new_argc] = NULL;
    }

skip_interception:
    int status, err;
    const char *err_msg;
    int exit_code = FATAL_EXIT_CODE;

    if (new_argc && new_argv) {
        err_msg = pex_execve(PEX_LAST | PEX_SEARCH, new_pathname, new_argv, envp, new_argv[0], NULL, NULL, &status, &err);
    } else {
        err_msg = pex_execve(PEX_LAST | PEX_SEARCH, pathname, argv, envp, argv[0], NULL, NULL, &status, &err);
    }

    free(new_argv);

    if (err_msg) {
        fprintf(stderr, "Error running %s: %s\n", argv[0], err_msg);
    } else if (status) {
        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            fprintf(stderr, "%s terminated with signal %d [%s]%s\n",
                    argv[0], sig, strsignal(sig),
                    WCOREDUMP(status) ? ", core dumped" : "");
        } else if (WIFEXITED(status)) {
            exit_code = WEXITSTATUS(status);
        }
    } else {
        exit_code = SUCCESS_EXIT_CODE;
    }
    return exit_code;
}