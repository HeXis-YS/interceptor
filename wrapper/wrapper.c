#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pex.h"

#define MAX_NEW_ARGV 32
#define NO_INTERCEPTION_ENV "NO_INTERCEPTION=1"
#define LTO_PLUGIN_PATH "/usr/lib/bfd-plugins/liblto_plugin.so"

char *const gcc_compiler_list[] = {"gcc", "g++", "c++", "cc", "xgcc", "xg++", NULL};
char *const binutils_list[] = {"ar", "nm", "ranlib", NULL};
char *const binutils_new_list[] = {"nm-new", NULL};
char *const configure_list[] = {"configure", NULL};

int strings_equal(const char *str1, const char *str2) {
    if (strcmp(str1, str2) == 0) {
        return 1;
    }
    return 0;
}

int strings_equal_partial(const char *str1, const char *str2) {
    if (strncmp(str1, str2, strlen(str2)) == 0) {
        return 1;
    }
    return 0;
}

int match_list(const char *str, char *const list[]) {
    for (int i = 0; list[i]; i++) {
        if (strings_equal(str, list[i])) {
            return i + 1;
        }
    }
    return 0;
}

char *insert_wrapper(const char *str1, const char *str2, int index) {
    int str1_len = strlen(str1);
    int new_len = str1_len + strlen(str2) + 1;
    int insert_pos = str1_len - strlen(binutils_list[index - 1]);
    char *res = (char *)malloc(new_len);
    if (!res) {
        perror("Memory allocation failed");
        exit(EXIT_FAILURE);
    }
    memset(res, '\0', new_len);
    strncpy(res, str1, insert_pos);
    strcat(res, str2);
    strcat(res, str1 + insert_pos);

    return res;
}

char *get_basename(const char *path, const char delimiter) {
    char *last_char = strrchr(path, delimiter);
    if (last_char) {
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
    argv++;
    argc--;
    const char *basemame_slash = get_basename(pathname, '/');
    const char *basename_dash = get_basename(basemame_slash, '-');

    int gcc_compiler = match_list(basename_dash, gcc_compiler_list);
    int binutils = match_list(basename_dash, binutils_list);
    int binutils_new = match_list(basemame_slash, binutils_new_list);
    int configure = match_list(get_basename(argv[0], '/'), configure_list);

    char *new_pathname = NULL;
    int new_argc = 0;
    char **new_argv = NULL;
    int new_envc = 0;
    char **new_envp = NULL;
    if (configure) {
        int envc = 0;
        while (envp[envc++]);
        new_envp = malloc((envc + 2) * sizeof(char *));
        for (int i = 0; i < envc; i++) {
            new_envp[new_envc++] = envp[i];
        }
        new_envp[new_envc++] = NO_INTERCEPTION_ENV;
        new_envp[new_envc] = NULL;
    } else if (binutils || binutils_new) {
        new_argv = malloc((argc + 3) * sizeof(char *));
        int lto_plugin_available = 0;
        for (int i = 0; i < argc && argv[i]; i++) {
            // fprintf(stderr,"%d %s\n", i, argv[i]);
            if (strings_equal(argv[i], "--plugin")) {
                if (argv[i + 1] == NULL) {
                    continue;
                }
                if (file_exist(argv[i + 1])) {
                    char *plugin_basename = get_basename(argv[i + 1], '/');
                    if (strings_equal_partial(plugin_basename, "liblto_plugin.so")) {
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
        for (int i = 0; envp[i]; i++) {
            if strings_equal (envp[i], NO_INTERCEPTION_ENV) {
                goto skip_interception;
            }
        }

        new_argv = malloc((argc + MAX_NEW_ARGV) * sizeof(char *))

            new_argv[new_argc++] = argv[0];
        new_argv[new_argc++] = "-march=native";
        new_argv[new_argc++] = "-mtune=native";

        for (int i = 1; i < argc && argv[i]; i++) {
            if (strings_equal(argv[i], "-O4")) {
                new_argc = 0;
                free(new_argv);
                goto skip_interception;
            }
            // Remove -O*, -march and -mtune
            if ((strings_equal_partial(argv[i], "-O") && !strings_equal(argv[i], "-Ofast")) ||
                strings_equal_partial(argv[i], "-march=") ||
                strings_equal_partial(argv[i], "-mtune=")) {
                continue;
            }
            new_argv[new_argc++] = argv[i];
        }

        // Add new arguments
        new_argv[new_argc++] = "-pipe";
        new_argv[new_argc++] = "-Wno-error";
        new_argv[new_argc++] = "-O4";
        // new_argv[new_argc++] = "-flto";
        // new_argv[new_argc++] = "-fno-fat-lto-objects";
        // new_argv[new_argc++] = "-flto-partition=none";
        // new_argv[new_argc++] = "-flto-compression-level=0";
        // new_argv[new_argc++] = "-fuse-linker-plugin";
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
        new_argv[new_argc++] = "-ffunction-sections";
        new_argv[new_argc++] = "-fdata-sections";
        new_argv[new_argc++] = "-mtls-dialect=gnu2";
        new_argv[new_argc++] = "-malign-data=cacheline";
        new_argv[new_argc++] = "-Wl,-O2";
        new_argv[new_argc++] = "-Wl,--gc-sections";

        new_argv[new_argc] = NULL;
    }

skip_interception:
    int status, err;
    const char *err_msg;
    int exit_code = FATAL_EXIT_CODE;

    if (!new_pathname) {
        new_pathname = pathname;
    }
    if (!new_argc) {
        new_argv = argv;
    }
    if (!new_envc) {
        new_envp = envp;
    }

    err_msg = pex_execve(PEX_LAST | PEX_SEARCH, new_pathname, new_argv, new_envp, new_argv[0], NULL, NULL, &status, &err);

    if (new_pathname != pathname) {
        free(new_pathname);
    }
    if (new_argc) {
        free(new_argv);
    }
    if (new_envc) {
        free(new_envp);
    }

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
