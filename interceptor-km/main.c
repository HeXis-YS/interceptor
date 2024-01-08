#include <linux/kernel.h>
#include <linux/module.h>

#include <khook/engine.h>

#define pr_fmt(fmt) "<%s> " fmt, __func__
////////////////////////////////////////////////////////////////////////////////
// Function Hooks
#include <linux/binfmts.h>
#include <linux/compat.h>
#include <linux/highmem.h>
#include <linux/user_namespace.h>

struct user_arg_ptr {
#ifdef CONFIG_COMPAT
    bool is_compat;
#endif
    union {
        const char __user *const __user *native;
#ifdef CONFIG_COMPAT
        const compat_uptr_t __user *compat;
#endif
    } ptr;
};

static int do_execve(struct filename *, const char __user *const __user *, const char __user *const __user *);
static int do_execveat_common(int, struct filename *, struct user_arg_ptr, struct user_arg_ptr, int);
static const char __user *get_user_arg_ptr(struct user_arg_ptr, int);
static bool valid_arg_len(struct linux_binprm *, long);
static void put_arg_page(struct page *);
static void flush_arg_page(struct linux_binprm *, unsigned long, struct page *);
static int count(struct user_arg_ptr, int);
static int bprm_stack_limits(struct linux_binprm *);
static int copy_strings(int, struct user_arg_ptr, struct linux_binprm *);

KHOOK_EXT(struct linux_binprm *, alloc_bprm, int, struct filename *);
static struct linux_binprm *khook_alloc_bprm(int fd, struct filename *filename) {
    return KHOOK_ORIGIN(alloc_bprm, fd, filename);
}
#define alloc_bprm(fd, filename) khook_alloc_bprm(fd, filename)

KHOOK_EXT(void, free_bprm, struct linux_binprm *);
static void khook_free_bprm(struct linux_binprm *bprm) {
    return KHOOK_ORIGIN(free_bprm, bprm);
}
#define free_bprm(bprm) khook_free_bprm(bprm)

KHOOK_EXT(int, bprm_execve, struct linux_binprm *, int, struct filename *, int);
static int khook_bprm_execve(struct linux_binprm *bprm, int fd, struct filename *filename, int flags) {
    return KHOOK_ORIGIN(bprm_execve, bprm, fd, filename, flags);
}
#define bprm_execve(bprm, fd, filename, flags) khook_bprm_execve(bprm, fd, filename, flags)

KHOOK_EXT(struct page *, get_arg_page, struct linux_binprm *, unsigned long, int);
static struct page *khook_get_arg_page(struct linux_binprm *bprm, unsigned long pos, int write) {
    return KHOOK_ORIGIN(get_arg_page, bprm, pos, write);
}
#define get_arg_page(bprm, pos, write) khook_get_arg_page(bprm, pos, write)

KHOOK(getname);
static struct filename *khook_getname(const char __user *filename) {
    return KHOOK_ORIGIN(getname, filename);
}
#define getname(filename) khook_getname(filename)

KHOOK(getname_kernel);
struct filename *khook_getname_kernel(const char *filename) {
    return KHOOK_ORIGIN(getname_kernel, filename);
}
#define getname_kernel(filename) khook_getname_kernel(filename)

KHOOK(putname);
static void khook_putname(struct filename *name) {
    return KHOOK_ORIGIN(putname, name);
}
#define putname(name) khook_putname(name)

KHOOK(is_rlimit_overlimit);
static bool khook_is_rlimit_overlimit(struct ucounts *ucounts, enum rlimit_type type, unsigned long rlimit) {
    return KHOOK_ORIGIN(is_rlimit_overlimit, ucounts, type, rlimit);
}
#define is_rlimit_overlimit(ucounts, type, rlimit) khook_is_rlimit_overlimit(ucounts, type, rlimit)

////////////////////////////////////////////////////////////////////////////////
// Linux v6.1.0

static int do_execve(struct filename *filename, const char __user *const __user *__argv, const char __user *const __user *__envp) {
    struct user_arg_ptr argv = {.ptr.native = __argv};
    struct user_arg_ptr envp = {.ptr.native = __envp};
    return do_execveat_common(AT_FDCWD, filename, argv, envp, 0);
}

static bool valid_arg_len(struct linux_binprm *bprm, long len) {
    return len <= MAX_ARG_STRLEN;
}

static const char __user *get_user_arg_ptr(struct user_arg_ptr argv, int nr) {
    const char __user *native;

#ifdef CONFIG_COMPAT
    if (unlikely(argv.is_compat)) {
        compat_uptr_t compat;
        if (get_user(compat, argv.ptr.compat + nr)) {
            return ERR_PTR(-EFAULT);
        }
        return compat_ptr(compat);
    }
#endif
    if (get_user(native, argv.ptr.native + nr)) {
        return ERR_PTR(-EFAULT);
    }
    return native;
}

static void put_arg_page(struct page *page) {
    put_page(page);
}

static void flush_arg_page(struct linux_binprm *bprm, unsigned long pos, struct page *page) {
    flush_cache_page(bprm->vma, pos, page_to_pfn(page));
}

static int count(struct user_arg_ptr argv, int max) {
    int i = 0;
    if (argv.ptr.native != NULL) {
        for (;;) {
            const char __user *p = get_user_arg_ptr(argv, i);
            if (!p) {
                break;
            }
            if (IS_ERR(p)) {
                return -EFAULT;
            }
            if (i >= max) {
                return -E2BIG;
            }
            ++i;
            if (fatal_signal_pending(current)) {
                return -ERESTARTNOHAND;
            }
            cond_resched();
        }
    }
    return i;
}

static int bprm_stack_limits(struct linux_binprm *bprm) {
    unsigned long limit, ptr_size;
    limit = _STK_LIM / 4 * 3;
    limit = min(limit, bprm->rlim_stack.rlim_cur / 4);
    limit = max_t(unsigned long, limit, ARG_MAX);
    ptr_size = (max(bprm->argc, 1) + bprm->envc) * sizeof(void *);
    if (limit <= ptr_size) {
        return -E2BIG;
    }
    limit -= ptr_size;
    bprm->argmin = bprm->p - limit;
    return 0;
}

static int copy_strings(int argc, struct user_arg_ptr argv, struct linux_binprm *bprm) {
    struct page *kmapped_page = NULL;
    char *kaddr = NULL;
    unsigned long kpos = 0;
    int ret;
    while (argc-- > 0) {
        const char __user *str;
        int len;
        unsigned long pos;
        ret = -EFAULT;
        str = get_user_arg_ptr(argv, argc);
        if (IS_ERR(str)) {
            goto out;
        }
        len = strnlen_user(str, MAX_ARG_STRLEN);
        if (!len) {
            goto out;
        }
        ret = -E2BIG;
        if (!valid_arg_len(bprm, len)) {
            goto out;
        }
        /* We're going to work our way backwards. */
        pos = bprm->p;
        str += len;
        bprm->p -= len;
#ifdef CONFIG_MMU
        if (bprm->p < bprm->argmin) {
            goto out;
        }
#endif
        while (len > 0) {
            int offset, bytes_to_copy;
            if (fatal_signal_pending(current)) {
                ret = -ERESTARTNOHAND;
                goto out;
            }
            cond_resched();
            offset = pos % PAGE_SIZE;
            if (offset == 0) {
                offset = PAGE_SIZE;
            }
            bytes_to_copy = offset;
            if (bytes_to_copy > len) {
                bytes_to_copy = len;
            }
            offset -= bytes_to_copy;
            pos -= bytes_to_copy;
            str -= bytes_to_copy;
            len -= bytes_to_copy;
            if (!kmapped_page || kpos != (pos & PAGE_MASK)) {
                struct page *page;
                page = get_arg_page(bprm, pos, 1);
                if (!page) {
                    ret = -E2BIG;
                    goto out;
                }
                if (kmapped_page) {
                    flush_dcache_page(kmapped_page);
                    kunmap_local(kaddr);
                    put_arg_page(kmapped_page);
                }
                kmapped_page = page;
                kaddr = kmap_local_page(kmapped_page);
                kpos = pos & PAGE_MASK;
                flush_arg_page(bprm, kpos, kmapped_page);
            }
            if (copy_from_user(kaddr + offset, str, bytes_to_copy)) {
                ret = -EFAULT;
                goto out;
            }
        }
    }
    ret = 0;
out:
    if (kmapped_page) {
        flush_dcache_page(kmapped_page);
        kunmap_local(kaddr);
        put_arg_page(kmapped_page);
    }
    return ret;
}

////////////////////////////////////////////////////////////////////////////////

#define MAX_NEW_ARGV 32
#define INTERCEPTOR_WRAPPER_PATH "/usr/bin/interceptor"

char *const gcc_compiler_list[] = {"gcc", "g++", "c++", "cc", "xgcc", "xg++", NULL};
char *const binutils_list[] = {"ar", "nm", "ranlib", NULL};
char *const binutils_new_list[] = {"nm-new", NULL};

static int match_list(const char *str, char *const list[]) {
    for (int i = 0; list[i] != NULL; i++) {
        if (strcmp(str, list[i]) == 0) {
            return i + 1;
        }
    }
    return 0;
}

static char *get_basename(const char *path, const char delimiter) {
    char *last_char = strrchr(path, delimiter);
    if (last_char != NULL) {
        return last_char + 1;
    }
    return (char *)path;
}

static int file_exists(const char *path) {
    struct file *file = filp_open(path, O_RDONLY, 0);
    if (!IS_ERR(file)) {
        filp_close(file, NULL);
        return 1;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////

static int do_execveat_common(int fd, struct filename *filename, struct user_arg_ptr argv, struct user_arg_ptr envp, int flags) {
    struct linux_binprm *bprm;
    int retval;

    if (IS_ERR(filename)) {
        return PTR_ERR(filename);
    }

    if ((current->flags & PF_NPROC_EXCEEDED) && is_rlimit_overlimit(current_ucounts(), UCOUNT_RLIMIT_NPROC, rlimit(RLIMIT_NPROC))) {
        retval = -EAGAIN;
        goto out_ret;
    }

    int call_wrapper = 0;
    struct filename *original_filename = NULL;
    char *pathname = filename->name;
    const char *basemame_slash = get_basename(pathname, '/');
    const char *basename_dash = get_basename(basemame_slash, '-');
    pr_info("%s %s",current->comm, pathname);
    if (file_exists(INTERCEPTOR_WRAPPER_PATH)) {
        call_wrapper = match_list(basename_dash, binutils_list);
        if (call_wrapper && (strncmp(basename_dash - 4, "gcc-", 4) == 0)) {
            call_wrapper = 0;
        }
        call_wrapper += match_list(basename_dash, gcc_compiler_list);
        call_wrapper += match_list(basemame_slash, binutils_new_list);
    }
    if (strcmp(current->comm, "interceptor") == 0 ||
        strcmp(current->comm, "lto-wrapper") == 0) {
        call_wrapper = 0;
    }
    if (call_wrapper) {
        original_filename = filename;
        filename = getname_kernel(INTERCEPTOR_WRAPPER_PATH);
    }

    current->flags &= ~PF_NPROC_EXCEEDED;

    bprm = alloc_bprm(fd, filename);
    if (IS_ERR(bprm)) {
        retval = PTR_ERR(bprm);
        goto out_ret;
    }

    retval = count(argv, MAX_ARG_STRINGS);
    if (retval == 0) {
        pr_warn_once("process '%s' launched '%s' with NULL argv: empty string added\n", current->comm, bprm->filename);
    } else if (retval < 0) {
        goto out_free;
    }
    bprm->argc = retval;
    if (call_wrapper) {
        bprm->argc += 1;
    }

    retval = count(envp, MAX_ARG_STRINGS);
    if (retval < 0) {
        goto out_free;
    }
    bprm->envc = retval;

    retval = bprm_stack_limits(bprm);
    if (retval < 0) {
        goto out_free;
    }

    retval = copy_string_kernel(bprm->filename, bprm);
    if (retval < 0) {
        goto out_free;
    }
    bprm->exec = bprm->p;

    retval = copy_strings(bprm->envc, envp, bprm);
    if (retval < 0) {
        goto out_free;
    }

    if (call_wrapper) {
        retval = copy_strings(bprm->argc - 1, argv, bprm);
        copy_string_kernel(original_filename->name, bprm);
    } else {
        retval = copy_strings(bprm->argc, argv, bprm);
    }
    if (retval < 0) {
        goto out_free;
    }

    if (bprm->argc == 0) {
        retval = copy_string_kernel("", bprm);
        if (retval < 0) {
            goto out_free;
        }
        bprm->argc = 1;
    }

    retval = bprm_execve(bprm, fd, filename, flags);
out_free:
    free_bprm(bprm);

out_ret:
    putname(filename);
    if (call_wrapper) {
        putname(original_filename);
    }
    return retval;
}

//////////////////////////////////////////////////////

KHOOK_EXT(long, __x64_sys_execve, const struct pt_regs *regs);
static long khook___x64_sys_execve(const struct pt_regs *regs) {
    const char __user *pathname = (const char __user *)regs->di;
    const char __user *const __user *argv = (const char __user *const __user *)regs->si;
    const char __user *const __user *envp = (const char __user *const __user *)regs->dx;
    return do_execve(getname(pathname), argv, envp);
}

int init_module(void) {
    return khook_init();
}

void cleanup_module(void) {
    khook_cleanup();
}

MODULE_LICENSE("GPL");
