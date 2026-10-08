// POSIX platform layer for tugz unzip
// $ cc -O2 -o unzip platform/unzip_posix.c
#define _POSIX_C_SOURCE 200809L  // sigaction, pread, O_NOFOLLOW, ...
#define _DARWIN_C_SOURCE         // macOS hides O_NOFOLLOW otherwise
#define _FILE_OFFSET_BITS 64     // large files on 32-bit hosts
#define _TIME_BITS 64            // and times past 2038 (glibc)
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/inflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/wild.c"
#include "../src/dir.c"
#include "../src/zipin.c"
#include "../src/unzip.c"
#include "../src/unzipcli.c"
#include "posix.c"
#include "zipfs_posix.c"

// Extraction: directories, attributes, links, times

// The reason for the last failure, copied: strerror's buffer is reused.
static s8 failure(arena *a)
{
    return JOIN(a, cstr(strerror(errno)));
}

static u32 os_umask(os *ctx)
{
    (void)ctx;
    mode_t m = umask(0);  // read only by changing it, before any output
    umask(m);
    return (u32)m & 0777;
}

static i64 os_mktime(os *ctx, i32 const tm[6])
{
    (void)ctx;
    struct tm t = {0};
    t.tm_year  = tm[0] - 1900;
    t.tm_mon   = tm[1] - 1;
    t.tm_mday  = tm[2];
    t.tm_hour  = tm[3];
    t.tm_min   = tm[4];
    t.tm_sec   = tm[5];
    t.tm_isdst = -1;  // as the zone had it then
    time_t r = mktime(&t);
    return r==(time_t)-1 ? uz_timegm(tm) : (i64)r;
}

static b32 os_mkdir(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    return !mkdir(tocstr(&scratch, path), 0777);
}

// Apply attributes to an open file or directory: the owner first, as
// changing it may clear set-ID bits, then for a file its mode and times,
// for a directory its times and mode. There is no creation time to set
// (OS_ACTIME), as UnZip's Unix port sets none.
static i32 setattrs(int fd, osattrs *attrs, b32 dir, s8 *why, arena *a)
{
    i32 failed = 0;
    i32 flags  = attrs->flags;
    if ((flags & OS_AOWNER) &&
        fchown(fd, (uid_t)attrs->uid, (gid_t)attrs->gid)) {
        failed |= OS_AOWNER;
        why[0] = failure(a);
    }
    mode_t mode = (mode_t)(attrs->mode & 07777);
    if (flags & OS_ASGID) {
        struct stat st;
        mode |= fstat(fd, &st) ? 0 : st.st_mode & S_ISGID;
    }
    for (i32 step = dir; step < dir+2; step++) {
        if ((step & 1) && (flags & OS_ATIMES)) {
            struct timespec ts[2] = {
                {(time_t)attrs->atime, 0}, {(time_t)attrs->mtime, 0},
            };
            if (futimens(fd, ts)) {
                failed |= OS_ATIMES;
                why[2] = failure(a);
            }
        } else if (!(step & 1) && (flags & OS_AMODE) && fchmod(fd, mode)) {
            failed |= OS_AMODE;
            why[1] = failure(a);
        }
    }
    return failed;
}

static i32 os_setattrs(os *ctx, i32 fd, osattrs *attrs, s8 *why, arena *a)
{
    (void)ctx;
    return setattrs(fd, attrs, 0, why, a);
}

static i32 os_setdirattrs(os *ctx, s8 path, osattrs *attrs, s8 *why,
                          arena *a)
{
    (void)ctx;
    arena tmp = *a;
    int   fd  = open(tocstr(&tmp, path),
                     O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_NOCTTY);
    if (fd < 0) {
        s8 r = failure(a);  // each attribute fails for it
        why[0] = why[1] = why[2] = r;
        return attrs->flags & (OS_AOWNER|OS_AMODE|OS_ATIMES);
    }
    i32 failed = setattrs(fd, attrs, 1, why, a);
    close(fd);
    return failed;
}

static i32 os_symlink(os *ctx, s8 target, s8 path, osattrs *attrs, s8 *why,
                      arena *a)
{
    (void)ctx;
    arena tmp   = *a;
    char *cpath = tocstr(&tmp, path);
    if (symlink(tocstr(&tmp, target), cpath)) {  // (up to a NUL, as UnZip)
        why[3] = failure(a);
        return OS_ALINK;
    }
    if ((attrs->flags & OS_AOWNER) &&
        lchown(cpath, (uid_t)attrs->uid, (gid_t)attrs->gid)) {
        why[0] = failure(a);
        return OS_AOWNER;
    }
    return 0;
}

int main(int argc, char **argv)
{
    os ctx = {0};
    ctx.outfd = -1;
    reserve_stdfds(&ctx, S("error:  cannot open /dev/null\n"), PK_PARAM);
    install_signals();

    unzipconfig conf = {0};
    reserve(&ctx, S("error:  not enough memory (opening /dev/zero)\n"),
            PK_MEM);
    conf.perm    = (arena){ctx.lo, ctx.lo, &ctx, 0};
    conf.scratch = (arena){ctx.hi, ctx.hi, &ctx, 1};

    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = new(&conf.perm, conf.nargs, s8);
    for (i32 i = 0; i < conf.nargs; i++) {
        conf.args[i] = cstr(argv[i+1]);
    }
    char *unzipenv = getenv("UNZIP");
    char *unzipopt = getenv("UNZIPOPT");
    conf.unzipenv = unzipenv ? cstr(unzipenv) : (s8){0};
    conf.unzipopt = unzipopt ? cstr(unzipopt) : (s8){0};
    return unzip_main(&conf);
}
