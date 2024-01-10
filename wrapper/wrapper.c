#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// #include "pex.h"

#define MAX_NEW_ARGV 32
#define ENV_SKIP_INTERCEPTION "SKIP_INTERCEPTION=1"
#define LTO_PLUGIN_PATH "/usr/lib/bfd-plugins/liblto_plugin.so"

char *const gcc_compiler_list[] = {"gcc", "g++", "c++", "cc", "xgcc", "xg++", NULL};
char *const binutils_list[] = {"ar", "nm", "ranlib", NULL};
char *const binutils_new_list[] = {"nm-new", NULL};

int strings_equal(const char *str1, const char *str2) {
    if (strcmp(str1, str2) == 0) {
        return 1;
    }
    return 0;
}

int strings_equal_n(const char *str1, const char *str2) {
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

int file_exists(const char *path) {
    int res = 0;
    if (access(path, R_OK) == 0) {
        res |= 4;
    }
    // if (access(path, W_OK) == 0) {
    //     res |= 2;
    // }
    if (access(path, X_OK) == 0) {
        res |= 1;
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

    int envc;
    char *pathname = argv[0];
    argv++;
    argc--;

    char *new_pathname = NULL;
    int new_argc = 0;
    char **new_argv = NULL;
    int new_envc = 0;
    char **new_envp = NULL;

    for (envc = 0; envp[envc]; envc++) {
        if (strings_equal(envp[envc], ENV_SKIP_INTERCEPTION)) {
            goto skip_interception;
        }
    }

    const char *basename_slash = get_basename(pathname, '/');
    const char *basename_dash = get_basename(basename_slash, '-');

    int gcc_compiler = match_list(basename_dash, gcc_compiler_list);
    int binutils = match_list(basename_dash, binutils_list);
    int binutils_new = match_list(basename_slash, binutils_new_list);

    if (binutils || binutils_new) {
        int lto_plugin_available = 0;
        for (int i = 0; i < argc && argv[i]; i++) {
            if (!strings_equal(argv[i], "--plugin")) {
                continue;
            }
            if (!argv[i + 1]) {
                continue;
            }
            i++;
            if (!file_exists(argv[i])) {
                continue;
            }
            if (strings_equal_n(get_basename(argv[i], '/'), "liblto_plugin.so")) {
                lto_plugin_available = 1;
            }
        }
        if (!lto_plugin_available) {
            new_argv = malloc((argc + 3) * sizeof(char *));
            for (int i = 0; i < argc && argv[i]; i++) {
                new_argv[new_argc++] = argv[i];
            }
            int dirname_len = strrchr(pathname, '/') - pathname;
            char *new_lto_plugin_path = malloc(dirname_len + strlen("/liblto_plugin.so") + 1);
            strncpy(new_lto_plugin_path, pathname, dirname_len);
            strcat(new_lto_plugin_path, "/liblto_plugin.so");
            if (file_exists(new_lto_plugin_path)) {
                lto_plugin_available = 1;
                new_argv[new_argc++] = "--plugin";
                new_argv[new_argc++] = new_lto_plugin_path;
            } else {
                free(new_lto_plugin_path);
            }
            if (!lto_plugin_available) {
                char *wrapper_pathname = insert_wrapper(pathname, "gcc-", binutils);
                if (!binutils_new && (file_exists(wrapper_pathname) & 1)) { // Check if gcc wrapper is available
                    lto_plugin_available = 1;
                    new_pathname = wrapper_pathname;
                    // If gcc wrapper is available, also modify argv[0]
                    // new_argv[0] = insert_wrapper(argv[0], "gcc-", binutils);
                } else {
                    free(wrapper_pathname);
                }
            }
            if (!lto_plugin_available) {
                new_argv[new_argc++] = "--plugin";
                new_argv[new_argc++] = LTO_PLUGIN_PATH;
            }
            new_argv[new_argc] = NULL;
        }
    } else if (gcc_compiler) {
        new_argv = malloc((argc + MAX_NEW_ARGV) * sizeof(char *));

        new_argv[new_argc++] = argv[0];
        new_argv[new_argc++] = "-march=native";
        new_argv[new_argc++] = "-mtune=native";

        for (int i = 1; i < argc && argv[i]; i++) {
            // Remove -O*, -march and -mtune
            if ((strings_equal_n(argv[i], "-O") && !strings_equal(argv[i], "-Ofast")) ||
                strings_equal_n(argv[i], "-march=") ||
                strings_equal_n(argv[i], "-mtune=")) {
                continue;
            }
            new_argv[new_argc++] = argv[i];
        }

        // Add new arguments
        new_argv[new_argc++] = "-pipe";
        new_argv[new_argc++] = "-Wno-error";
        new_argv[new_argc++] = "-O3";
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

    new_envp = malloc((envc + 2) * sizeof(char *));
    for (int i = 0; i < envc && envp[i]; i++) {
        new_envp[new_envc++] = envp[i];
    }
    new_envp[new_envc++] = ENV_SKIP_INTERCEPTION;
    new_envp[new_envc] = NULL;

skip_interception:
    if (!new_pathname) {
        new_pathname = pathname;
    }
    if (!new_argc) {
        new_argv = argv;
    }
    if (!new_envc) {
        new_envp = envp;
    }

    return execve(new_pathname, new_argv, new_envp);

    // if (new_pathname != pathname) {
    //     free(new_pathname);
    // }
    // if (new_argc) {
    //     free(new_argv);
    // }
    // if (new_envc) {
    //     free(new_envp);
    // }
}
