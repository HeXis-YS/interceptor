#include <libiberty/libiberty.h>

#define SUCCESS_EXIT_CODE 0
#define FATAL_EXIT_CODE 1

const char *
pex_execve(int, const char *, char *const *, char *const *,
        const char *, const char *, const char *,
        int *, int *);