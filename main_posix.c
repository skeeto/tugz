// POSIX platform layer for gzip
// $ cc -O2 -o gzip main_posix.c
#include "src/base.c"
#include "src/crc32.c"
#include "src/inflate.c"
#include "src/deflate.c"
#include "src/gzip.c"
#include "src/cli.c"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

struct os {
    b32 unused;
};

static s8 cstr(char *z)
{
    s8 r = {(u8 *)z, 0};
    for (; z[r.len]; r.len++) {}
    return r;
}

static char *tocstr(arena *a, s8 s)
{
    char *r = (char *)newbytes(a, s.len+1);
    bytecopy(r, s.s, s.len);
    r[s.len] = 0;
    return r;
}

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    (void)ctx;
    char *cpath = tocstr(&scratch, path);
    switch (mode) {
    case OS_READ:;
        int fd = open(cpath, O_RDONLY);
        if (fd < 0) {
            return OS_ERR;
        }
        struct stat st;
        if (!fstat(fd, &st) && S_ISDIR(st.st_mode)) {
            close(fd);
            return OS_EISDIR;
        }
        return fd;
    case OS_CREATE:
        fd = open(cpath, O_WRONLY|O_CREAT|O_EXCL, 0666);
        return fd>=0 ? fd : errno==EEXIST ? OS_EEXIST : OS_ERR;
    case OS_FORCE:
        fd = open(cpath, O_WRONLY|O_CREAT|O_TRUNC, 0666);
        return fd>=0 ? fd : OS_ERR;
    }
    return OS_ERR;
}

static void os_close(os *ctx, i32 fd)
{
    (void)ctx;
    close(fd);
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    (void)ctx;
    for (;;) {
        iz r = read(fd, buf, (uz)MIN(cap, 1<<30));
        if (r>=0 || errno!=EINTR) {
            return r<0 ? -1 : r;
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

static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    return !unlink(tocstr(&scratch, path));
}

static void os_fail(os *ctx)
{
    (void)ctx;
    _exit(EXIT_ERR);
}

int main(int argc, char **argv)
{
    os ctx = {0};
    iz cap = (iz)1 << 25;

    config conf = {0};
    conf.perm.beg = malloc((uz)cap);
    if (!conf.perm.beg) {
        return EXIT_ERR;
    }
    conf.perm.end = conf.perm.beg + cap;
    conf.perm.ctx = &ctx;

    conf.nargs = argc - 1;
    conf.args = new(&conf.perm, conf.nargs, s8);
    for (i32 i = 0; i < conf.nargs; i++) {
        conf.args[i] = cstr(argv[i+1]);
    }
    return gzip_main(&conf);
}
