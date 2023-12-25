#include "pex.h"

const char *
pex_execve(int flags, const char *executable, char *const *argv, char *const *envp,
        const char *pname, const char *outname, const char *errname,
        int *status, int *err) {
    struct pex_obj *obj;
    const char *errmsg;

    obj = pex_init(0, pname, NULL);
    errmsg = pex_run_in_environment (obj, flags, executable, argv, envp, outname, errname, err);
    if (errmsg == NULL) {
        if (!pex_get_status(obj, 1, status)) {
            *err = 0;
            errmsg = "pex_get_status failed";
        }
    }
    pex_free(obj);
    return errmsg;
}