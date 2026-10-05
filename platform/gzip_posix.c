// POSIX platform layer for tugz gzip
// $ cc -O2 -o gzip platform/gzip_posix.c
#define _POSIX_C_SOURCE 200809L  // sigaction, futimens, O_NOFOLLOW, ...
#define _DARWIN_C_SOURCE         // macOS hides O_NOFOLLOW otherwise
#define _FILE_OFFSET_BITS 64     // large files on 32-bit hosts
#define _TIME_BITS 64            // and times past 2038 (glibc)
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/adler32.c"
#include "../src/inflate.c"
#include "../src/deflate.c"
#include "../src/gzip.c"
#include "../src/io.c"
#include "../src/gzipio.c"
#include "../src/cli.c"
#include "posix.c"

static b32 os_isatty(os *ctx, i32 fd)
{
    (void)ctx;
    return isatty(fd);
}

static void os_copymeta(os *ctx, i32 from, i32 to)
{
    (void)ctx;
    struct stat st;
    if (fstat(from, &st)) {
        return;
    }

    // Ownership first, since changing it may clear set-ID bits. Without
    // the original owner, keep no set-ID bits at all.
    mode_t mode = st.st_mode & 07777;
    if (fchown(to, st.st_uid, st.st_gid)) {
        mode &= ~(mode_t)(S_ISUID|S_ISGID);
        if (fchown(to, (uid_t)-1, st.st_gid)) {
            mode &= ~(mode_t)S_ISGID;
        }
    }
    fchmod(to, mode);

    struct timespec times[2] = {st.st_atim, st.st_mtim};
    futimens(to, times);
}

int main(int argc, char **argv)
{
    os ctx = {0};
    ctx.outfd = -1;

    reserve_stdfds();
    install_signals();

    iz cap = (iz)1 << 25;
    byte *mem = malloc((uz)cap);
    if (!mem) {
        os_oom(&ctx);
    }
    config conf = {0};
    conf.perm.beg = mem;
    conf.perm.end = mem + cap;
    conf.perm.ctx = &ctx;

    if (argc > 0) {
        s8 name = cstr(argv[0]);
        for (iz i = name.len; i > 0; i--) {
            if (name.s[i-1] == '/') {
                name.s += i;
                name.len -= i;
                break;
            }
        }
        conf.name = name;
    }
    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args = new(&conf.perm, conf.nargs, s8);
    for (i32 i = 0; i < conf.nargs; i++) {
        conf.args[i] = cstr(argv[i+1]);
    }
    i32 status = gzip_main(&conf);
    free(mem);  // for leak checkers
    return status;
}
