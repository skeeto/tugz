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
#include <sys/mman.h>
#include <time.h>

#ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON  // its older name
#endif
#ifndef MAP_NORESERVE
#  define MAP_NORESERVE 0
#endif

// zip's memory is one reservation of address space, of which only the
// pages touched get memory: perm grows up from its bottom and scratch
// down from its top, each claiming more of the middle as it needs it.
// MAP_NORESERVE keeps Linux from counting the whole reservation against
// its overcommit heuristic. As much as the system lends, up to 16 GiB
// (1 GiB on 32-bit hosts), halving on refusal.
static void reserve(os *ctx)
{
    iz cap = (iz)1 << (sizeof(void *)==8 ? 34 : 30);
    for (; cap >= (iz)1<<24; cap /= 2) {
        void *p = mmap(0, (uz)cap, PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
        if (p != MAP_FAILED) {
            ctx->lo = p;
            ctx->hi = ctx->lo + cap;
            return;
        }
    }
    os_oom(ctx);
}

// Claim more of the reservation for perm, from below the middle, or for
// a scratch arena, from above it, a megabyte at a time. Scratch below
// the arena asking is free, since the functions that allocated it have
// returned. Any other arena, such as a codec's exact one, is fixed.
static void os_extend(os *ctx, arena *a, iz need)
{
    if (a->down) {
        a->beg = ctx->hi;
    } else if (a->end != ctx->lo) {
        os_oom(ctx);
    }
    iz want = need - (a->end - a->beg);
    iz room = ctx->hi - ctx->lo;
    if (want > room) {
        os_oom(ctx);
    } else if (want > 0) {
        iz chunk = (iz)1 << 20;
        iz take  = MIN((want + chunk - 1) & -chunk, room);
        if (a->down) {
            ctx->hi -= take;
            a->beg   = ctx->hi;
        } else {
            ctx->lo += take;
            a->end   = ctx->lo;
        }
    }
}

static void stat_info(struct stat *st, os_info *info)
{
    info->type   = S_ISREG(st->st_mode) ? FT_FILE :
                   S_ISDIR(st->st_mode) ? FT_DIR  :
                   S_ISLNK(st->st_mode) ? FT_LINK : FT_OTHER;
    info->size   = st->st_size;
    info->mtime  = st->st_mtime;
    info->atime  = st->st_atime;
    info->mode   = (u32)st->st_mode;
    info->attr   = 0;
    info->uid    = (u32)st->st_uid;
    info->gid    = (u32)st->st_gid;
    info->dev    = (u64)st->st_dev;
    info->ino[0] = (u64)st->st_ino;
    info->ino[1] = 0;
}

static b32 os_stat(os *ctx, s8 path, b32 follow, os_info *info,
                   arena scratch)
{
    (void)ctx;
    char *cpath = tocstr(&scratch, path);
    struct stat st;
    if (follow ? stat(cpath, &st) : lstat(cpath, &st)) {
        return 0;
    }
    stat_info(&st, info);
    return 1;
}

static b32 os_fstat(os *ctx, i32 fd, os_info *info)
{
    (void)ctx;
    struct stat st;
    if (fstat(fd, &st)) {
        return 0;
    }
    stat_info(&st, info);
    return 1;
}

// Entries tell only names: each is left for os_stat.
static os_dirent *os_listdir(os *ctx, s8 path, b32 all, iz *count,
                             arena *a)
{
    (void)ctx;
    (void)all;  // no hidden or system attributes
    arena tmp = *a;  // the path, which the listing then overwrites
    DIR  *d   = opendir(tocstr(&tmp, path));
    if (!d) {
        return 0;
    }
    os_dirents names = {0};
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
        s8 copy = {newstr(a, name.len), name.len};
        bytecopy(copy.s, name.s, name.len);
        *push(a, &names) = (os_dirent){copy, {0}};  // type FT_NONE
    }
    int err = errno;
    closedir(d);
    if (err) {
        return 0;
    }
    *count = names.len;
    return names.data ? names.data : new(a, 1, os_dirent);
}

static s8 os_readlink(os *ctx, s8 path, arena *a)
{
    (void)ctx;
    char *cpath = tocstr(a, path);
    for (iz cap = 256;; cap *= 2) {
        u8 *buf = newstr(a, cap);
        iz  n   = readlink(cpath, (char *)buf, (uz)cap);
        if (n < 0) {
            return (s8){0};
        } else if (n < cap) {
            return (s8){buf, n};
        }
    }
}

// Links are read one at a time, so that a dangling one leads where it
// points, a relative target being relative to the link's directory. But
// only links the system itself would follow are: not a loop, nor one
// that Linux's protected_symlinks refuses in a sticky directory.
static s8 os_resolve(os *ctx, s8 path, arena *perm, arena scratch)
{
    struct stat st;
    if (stat(tocstr(&scratch, path), &st) && errno!=ENOENT) {
        return (s8){0};
    }
    for (i32 hops = 0; hops < 40; hops++) {
        s8 target = os_readlink(ctx, path, &scratch);
        if (!target.s) {
            // Not a link (EINVAL), or nothing there
            return errno==EINVAL || errno==ENOENT ? path : (s8){0};
        }
        iz cut = path.len;
        if (target.len && target.s[0]=='/') {
            cut = 0;
        }
        for (; cut>0 && path.s[cut-1]!='/'; cut--) {}
        s8 next = {newstr(perm, cut+target.len), cut+target.len};
        bytecopy(next.s, path.s, cut);
        bytecopy(next.s+cut, target.s, target.len);
        path = next;
    }
    return (s8){0};
}

// Judged by the system, as when Info-ZIP opens the archive to update
// it: root, ACLs, flags, and read-only file systems all count.
static b32 os_writable(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    return !access(tocstr(&scratch, path), W_OK);
}

static i32 os_readat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    (void)ctx;
    while (len) {
        iz r = pread(fd, buf, (uz)MIN(len, 1<<30), (off_t)off);
        if (r < 0 && errno == EINTR) {
            continue;
        } else if (r <= 0) {
            return r<0 ? -1 : 0;
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

    // Close before replacing anything, since deferred write errors (NFS,
    // quotas) may surface only now. Until the rename, the file is still
    // discarded on failure or interruption. As in Info-ZIP there is no
    // fsync: on an SD card, waiting for the device turned a 0.85 s run
    // storing 256 MiB into 20-40 s, adding only durability across a
    // crash and the rare device error that nothing else reports.
    b32 ok = !close(fd) || errno==EINTR;
    ctx->outfd = -1;

    sigset_t old = block_signals();
    ok = ok && !rename(pending_output, dst);
    release_output(ok);
    restore_signals(old);
    return ok;
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

static s8 os_fromoem(os *ctx, s8 name, arena *perm, arena scratch)
{
    (void)ctx;
    (void)name;
    (void)perm;
    (void)scratch;
    return (s8){0};  // never asked: only Windows reads OEM names
}

static s8 os_fullpath(os *ctx, s8 path, arena *perm, arena scratch)
{
    (void)ctx;
    (void)path;
    (void)perm;
    (void)scratch;
    return (s8){0};  // never needed: every file has an inode number
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
    reserve_stdfds(&ctx, S("\nzip error: Could not open /dev/null\n"),
                   ZE_TEMP);
    install_signals();

    zipconfig conf = {0};
    reserve(&ctx);
    conf.perm    = (arena){ctx.lo, ctx.lo, &ctx, 0};
    conf.scratch = (arena){ctx.hi, ctx.hi, &ctx, 1};

    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = new(&conf.perm, conf.nargs, s8);
    for (i32 i = 0; i < conf.nargs; i++) {
        conf.args[i] = cstr(argv[i+1]);
    }
    char *epoch  = getenv("SOURCE_DATE_EPOCH");
    char *zipopt = getenv("ZIPOPT");
    char *zipenv = getenv("ZIP");
    conf.epoch  = epoch  ? cstr(epoch)  : (s8){0};
    conf.zipopt = zipopt ? cstr(zipopt) : (s8){0};
    conf.zipenv = zipenv ? cstr(zipenv) : (s8){0};
    return zip_main(&conf);
}
