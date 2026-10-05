// POSIX platform layer for tugz zip
// $ cc -O2 -o zip platform/zip_posix.c
#define _POSIX_C_SOURCE 200809L  // sigaction, pread, O_NOFOLLOW, ...
#define _DARWIN_C_SOURCE         // macOS hides O_NOFOLLOW otherwise
#define _FILE_OFFSET_BITS 64     // large files on 32-bit hosts
#define _TIME_BITS 64            // and times past 2038 (glibc)
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/deflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/zipcli.c"
#include "posix.c"

#include <dirent.h>
#include <stdio.h>  // rename
#include <time.h>

static b32 os_stat(os *ctx, s8 path, b32 follow, os_info *info,
                   arena scratch)
{
    (void)ctx;
    char *cpath = tocstr(&scratch, path);
    struct stat st;
    if (follow ? stat(cpath, &st) : lstat(cpath, &st)) {
        return 0;
    }
    info->type  = S_ISREG(st.st_mode) ? FT_FILE :
                  S_ISDIR(st.st_mode) ? FT_DIR  :
                  S_ISLNK(st.st_mode) ? FT_LINK : FT_OTHER;
    info->size  = st.st_size;
    info->mtime = st.st_mtime;
    info->atime = st.st_atime;
    info->mode  = (u32)st.st_mode;
    info->attr  = 0;
    info->uid   = (u32)st.st_uid;
    info->gid   = (u32)st.st_gid;
    info->dev   = (u64)st.st_dev;
    info->ino[0] = (u64)st.st_ino;
    info->ino[1] = 0;
    return 1;
}

static s8 *os_listdir(os *ctx, s8 path, b32 all, iz *count, arena *perm,
                      arena scratch)
{
    (void)ctx;
    (void)all;  // no hidden or system attributes
    DIR *d = opendir(tocstr(&scratch, path));
    if (!d) {
        return 0;
    }
    s8s names = {0};
    for (;;) {
        errno = 0;  // distinguishes an error from the end
        struct dirent *e = readdir(d);
        if (!e) {
            break;
        }
        s8 name = cstr(e->d_name);
        if (zequals(name, S(".")) || zequals(name, S(".."))) {
            continue;
        }
        s8 copy = {newbytes(perm, name.len), name.len};
        bytecopy(copy.s, name.s, name.len);
        *push(perm, &names) = copy;
    }
    int err = errno;
    closedir(d);
    if (err) {
        return 0;
    }
    *count = names.len;
    return names.data ? names.data : new(perm, 1, s8);
}

static s8 os_readlink(os *ctx, s8 path, arena *perm, arena scratch)
{
    (void)ctx;
    char *cpath = tocstr(&scratch, path);
    for (iz cap = 256;; cap *= 2) {
        u8 *buf = newbytes(&scratch, cap);
        iz  n   = readlink(cpath, (char *)buf, (uz)cap);
        if (n < 0) {
            return (s8){0};
        } else if (n < cap) {
            s8 r = {newbytes(perm, n), n};
            bytecopy(r.s, buf, n);
            return r;
        }
    }
}

static b32 os_readat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    (void)ctx;
    while (len) {
        iz r = pread(fd, buf, (uz)MIN(len, 1<<30), (off_t)off);
        if (r < 0 && errno == EINTR) {
            continue;
        } else if (r <= 0) {
            return 0;
        }
        buf += r;
        len -= r;
        off += r;
    }
    return 1;
}

static b32 os_writeat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    (void)ctx;
    while (len) {
        iz r = pwrite(fd, buf, (uz)MIN(len, 1<<30), (off_t)off);
        if (r < 0 && errno == EINTR) {
            continue;
        } else if (r <= 0) {
            return 0;
        }
        buf += r;
        len -= r;
        off += r;
    }
    return 1;
}

static b32 os_truncate(os *ctx, i32 fd, i64 len)
{
    (void)ctx;
    return !ftruncate(fd, (off_t)len);
}

static b32 os_commit(os *ctx, i32 fd, s8 path, arena scratch)
{
    char *dst = tocstr(&scratch, path);
    struct stat st;
    if (!stat(dst, &st)) {
        fchmod(fd, st.st_mode & 0777);
    } else if (!ctx->defperms) {
        // The archive existed at startup, so the temp file is owner-only,
        // but it has since gone and this becomes a new file. Give it the
        // usual 0666 less the umask. Unlike OS_DEFPERMS, this ignores a
        // default ACL, and the target may still change before the rename.
        mode_t mask = umask(0);
        umask(mask);
        fchmod(fd, 0666 & ~mask);
    }

    // Flush and close before replacing anything, since deferred write
    // errors (NFS, quotas) may surface only now. Until the rename, the
    // file is still discarded on failure or interruption.
    b32 ok = !fsync(fd) || errno==EINVAL || errno==ENOTSUP;
    ok &= !close(fd) || errno==EINTR;
    ctx->outfd = -1;

    sigset_t old = block_signals();
    ok = ok && !rename(pending_output, dst);
    release_output(ok);
    restore_signals(old);
    return ok;
}

static void os_localtime(os *ctx, i64 t, i32 tm[6])
{
    (void)ctx;
    time_t    tt = (time_t)t;
    struct tm r;
    if (!localtime_r(&tt, &r)) {
        zip_gmtime(t, tm);
        return;
    }
    tm[0] = r.tm_year + 1900;
    tm[1] = r.tm_mon + 1;
    tm[2] = r.tm_mday;
    tm[3] = r.tm_hour;
    tm[4] = r.tm_min;
    tm[5] = r.tm_sec;
}

int main(int argc, char **argv)
{
    os ctx = {0};
    ctx.outfd = -1;
    reserve_stdfds();
    install_signals();

    iz cap = (iz)1 << 28;
    byte *mem = malloc((uz)cap);
    if (!mem) {
        os_oom(&ctx);
    }
    zipconfig conf = {0};
    conf.perm.beg = mem;
    conf.perm.end = mem + cap;
    conf.perm.ctx = &ctx;

    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = new(&conf.perm, conf.nargs, s8);
    for (i32 i = 0; i < conf.nargs; i++) {
        conf.args[i] = cstr(argv[i+1]);
    }
    char *epoch = getenv("SOURCE_DATE_EPOCH");
    conf.epoch = epoch ? cstr(epoch) : (s8){0};
    i32 status = zip_main(&conf);
    free(mem);  // for leak checkers
    return status;
}
