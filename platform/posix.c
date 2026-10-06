// Shared POSIX platform code: the os_* file interface of src/io.c
// Included by each POSIX program after the sources it needs, before its
// own platform functions and entry point.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Nanosecond timestamps: POSIX 2008 names, but macOS predates them
#ifdef __APPLE__
#  define st_atim st_atimespec
#  define st_mtim st_mtimespec
#endif

// Path of the output file being written, deleted if a signal interrupts
// the program, and null once the file is kept. The signal handler is why
// this one variable is global.
static char *volatile pending_output;

struct os {
    i32   outfd;     // descriptor of the created output file, or -1
    b32   defperms;  // it was created with OS_DEFPERMS, not owner-only
    byte *lo;        // zip: the unclaimed middle of its memory
    byte *hi;
};

static s8 cstr(char *z)
{
    s8 r = {(u8 *)z, 0};
    for (; z[r.len]; r.len++) {}
    return r;
}

static char *tocstr(arena *a, s8 s)
{
    char *r = (char *)newstr(a, s.len+1);
    bytecopy(r, s.s, s.len);
    r[s.len] = 0;
    return r;
}

// Signals that commonly end a run early: hangup, the terminal, kill, a
// closed pipe, and resource limits
static int const cleanup_signals[] = {
    SIGHUP, SIGINT, SIGPIPE, SIGQUIT, SIGTERM,
#ifdef SIGXCPU
    SIGXCPU,
#endif
#ifdef SIGXFSZ
    SIGXFSZ,
#endif
};

static void on_signal(int sig)
{
    char *path = pending_output;
    if (path) {
        unlink(path);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

// Block cleanup signals while pending_output changes along with the file.
static sigset_t block_signals(void)
{
    sigset_t set, old;
    sigemptyset(&set);
    for (iz i = 0; i < countof(cleanup_signals); i++) {
        sigaddset(&set, cleanup_signals[i]);
    }
    sigprocmask(SIG_BLOCK, &set, &old);
    return old;
}

static void restore_signals(sigset_t old)
{
    sigprocmask(SIG_SETMASK, &old, 0);
}

// Forget the pending output file, first deleting it unless kept.
static void release_output(b32 keep)
{
    sigset_t old = block_signals();
    char *path = pending_output;
    if (path && !keep) {
        unlink(path);
    }
    pending_output = 0;
    restore_signals(old);
    free(path);
}

static i32 open_output(os *ctx, char *cpath, i32 mode)
{
    int kept = 0;  // why a name to replace could not be removed
    if (mode & OS_FORCE) {
        // Replace rather than truncate, so a link is never written through
        kept = unlink(cpath) ? errno : 0;
    }

    // Exactly one output file is open at a time
    char *copy = malloc(strlen(cpath) + 1);
    if (!copy) {
        return OS_ERR;
    }
    strcpy(copy, cpath);

    sigset_t old = block_signals();
    mode_t perm = mode & OS_DEFPERMS ? 0666 : 0600;
    int fd = open(cpath, O_WRONLY|O_CREAT|O_EXCL, perm);
    int err = errno;
    if (fd >= 0) {
        ctx->outfd = fd;
        ctx->defperms = !!(mode & OS_DEFPERMS);
        pending_output = copy;
    }
    restore_signals(old);

    if (fd < 0) {
        // Forced, a name not replaced (a directory) is an error. Leave
        // the reason in errno for the caller: for that, why it stayed.
        free(copy);
        b32 force = !!(mode & OS_FORCE);
        errno = err==EEXIST && force && kept ? kept : err;
        return err==EEXIST && !force ? OS_EEXIST : OS_ERR;
    }
    return fd;
}

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    char *cpath = tocstr(&scratch, path);
    if (mode & (OS_CREATE|OS_FORCE)) {
        return open_output(ctx, cpath, mode);
    }

    // Opening a FIFO would block until it has a writer, so check the type
    // without blocking when only regular files are wanted.
    int flags = O_RDONLY;
    flags |= mode & OS_NOFOLLOW ? O_NOFOLLOW : 0;
    flags |= mode & OS_REGULAR  ? O_NONBLOCK : 0;
    int fd = open(cpath, flags);
    if (fd < 0) {
        // Refused symbolic links are ELOOP on Linux, EMLINK on FreeBSD,
        // and EFTYPE on NetBSD
        b32 link = errno==ELOOP || errno==EMLINK;
#ifdef EFTYPE
        link |= errno==EFTYPE;
#endif
        if ((mode & OS_NOFOLLOW) && link) {
            return OS_ESYMLINK;
        }
        return errno==EISDIR ? OS_EISDIR : OS_ERR;
    }

    struct stat st;
    i32 err = 0;
    if (fstat(fd, &st)) {
        err = OS_ERR;
    } else if (S_ISDIR(st.st_mode)) {
        err = OS_EISDIR;
    } else if ((mode & OS_REGULAR) && !S_ISREG(st.st_mode)) {
        err = OS_ENOTREG;
    } else if ((mode & OS_ONELINK) && st.st_nlink>1) {
        err = OS_ELINKS;
    } else if (flags & O_NONBLOCK) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    }
    if (err) {
        close(fd);
        return err;
    }
    return fd;
}

static b32 os_close(os *ctx, i32 fd)
{
    b32 ok  = !close(fd) || errno==EINTR;
    int err = errno;  // why, for the caller, past the cleanup
    if (fd == ctx->outfd) {
        ctx->outfd = -1;
        release_output(0);
    }
    errno = err;
    return ok;
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    (void)ctx;
    for (;;) {
        iz r = read(fd, buf, (uz)MIN(cap, 1<<30));
        if (r >= 0) {
            return r;
        } else if (errno == EAGAIN) {
            // Inherited non-blocking, as a pipe or terminal another program
            // left so: wait for input from now on, as GNU gzip does
            int flags = fcntl(fd, F_GETFL);
            if (flags<0 || !(flags & O_NONBLOCK) ||
                fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)) {
                errno = EAGAIN;
                return -1;
            }
        } else if (errno != EINTR) {
            return -1;
        }
    }
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    (void)ctx;
    while (len) {
        iz r = write(fd, buf, (uz)MIN(len, 1<<30));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        buf += r;
        len -= r;
    }
    return 1;
}

[[maybe_unused]] static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    return !unlink(tocstr(&scratch, path));
}

[[maybe_unused]] static b32 os_keep(os *ctx, i32 fd)
{
    if (fd == ctx->outfd) {
        ctx->outfd = -1;  // so that closing it has nothing to forget
        release_output(1);
    }
    return 1;
}

static void os_exit(os *ctx, i32 status)
{
    (void)ctx;
    char *path = pending_output;
    if (path) {
        unlink(path);
    }
    _exit(status);
}

static b32 os_isatty(os *ctx, i32 fd)
{
    (void)ctx;
    return isatty(fd);
}

static s8 os_error(os *ctx)
{
    (void)ctx;
    return errno ? cstr(strerror(errno)) : S("");
}

// Occupy any closed standard descriptor, so that a file opened later
// cannot take its place and receive messages. The opposite access mode
// makes using it fail as though it were still closed. Without /dev/null
// (some chroots and sandboxes), exit with msg and status instead: with
// nothing opened yet, msg reaches only the caller's standard error.
static void reserve_stdfds(os *ctx, s8 msg, i32 status)
{
    for (int fd = 0; fd <= 2; fd++) {
        if (fcntl(fd, F_GETFD) < 0) {
            int devnull = open("/dev/null", fd ? O_RDONLY : O_WRONLY);
            if (devnull != fd) {  // the lowest free, so fd unless it failed
                os_write(ctx, 2, msg.s, msg.len);
                os_exit(ctx, status);
            }
        }
    }
}

// Install handlers that remove the pending output file on interruption.
static void install_signals(void)
{
    for (iz i = 0; i < countof(cleanup_signals); i++) {
        struct sigaction sa = {0};
        sigaction(cleanup_signals[i], 0, &sa);
        if (sa.sa_handler != SIG_IGN) {  // e.g. under nohup
            sa.sa_handler = on_signal;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            sigaction(cleanup_signals[i], &sa, 0);
        }
    }
}
