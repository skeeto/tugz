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
#include "../src/wild.c"
#include "../src/dir.c"
#include "../src/zipin.c"
#include "../src/zipcli.c"
#include "posix.c"

#include <dirent.h>
#include <stdio.h>  // rename
#include <sys/mman.h>
#include <time.h>

// POSIX names anonymous memory only since 2024, so under _POSIX_C_SOURCE
// the BSDs hide it, as glibc did before 2.37, and a private mapping of
// /dev/zero stands in. (macOS cannot map /dev/zero, but _DARWIN_C_SOURCE
// shows MAP_ANON.)
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#  define MAP_ANONYMOUS MAP_ANON  // its older name
#endif

// zip's memory is one reserved range of address space, committed a
// chunk at a time as it is used, so that the commit charge grows with
// use: perm from the bottom up, and scratch from the top down. Mapped
// PROT_NONE, the reservation is charged nothing, where a writable one
// would be charged in full under Linux's strict overcommit, which
// ignores MAP_NORESERVE (in the other modes, that flag would only leave
// the committed chunks uncounted). As much as the system will reserve,
// up to 16 GiB (1 GiB in 32-bit processes), but for room left for the C
// library, whose malloc opendir, localtime, and open_output still use,
// and for the stack: under a limit on address space (ulimit -v), which
// counts the reservation in full, whatever zip took they could not have,
// and their failures were reported as unreadable directories and failed
// temporary files. Once refused, the most that leaves that room is found
// to the megabyte by bisection, a mapping and unmapping each.
static byte *map(iz size, int flags, int fd)
{
    byte *p = mmap(0, (uz)size, PROT_NONE, flags, fd, 0);
    return p==MAP_FAILED ? 0 : p;
}

static void reserve(os *ctx)
{
    int flags = MAP_PRIVATE;
    int fd    = -1;
#ifdef MAP_ANONYMOUS
    flags |= MAP_ANONYMOUS;
#else
    fd = open("/dev/zero", O_RDONLY);
    if (fd < 0) {
        // As Info-ZIP words what it was doing when memory ran out
        s8 msg = S("\nzip error: Out of memory (opening /dev/zero)\n");
        os_write(ctx, 2, msg.s, msg.len);
        os_exit(ctx, ZE_MEM);
    }
#endif
    iz    chunk = (iz)1 << 20;
    iz    room  = 4 * chunk;
    iz    cap   = (iz)1 << (sizeof(void *)==8 ? 34 : 30);
    byte *p     = map(cap+room, flags, fd);
    if (!p) {
        iz lo = 0;    // known to fit with room (or nothing)
        iz hi = cap;  // known not to
        while (hi-lo > chunk) {
            iz    mid = lo + (hi-lo)/chunk/2*chunk;
            byte *t   = map(mid+room, flags, fd);
            if (t) {
                munmap(t, (uz)(mid+room));
                lo = mid;
            } else {
                hi = mid;
            }
        }
        cap = lo;
        p   = lo ? map(cap+room, flags, fd) : 0;
    }
    if (fd >= 0) {
        close(fd);
    }
    if (!p) {
        os_oom(ctx);
    }
    munmap(p+cap, (uz)room);
    ctx->lo = p;
    ctx->hi = p + cap;
}

// Commit more of the reservation to perm, from below the middle, or to
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
        iz    chunk = (iz)1 << 20;
        iz    take  = MIN((want + chunk - 1) & -chunk, room);
        byte *at    = a->down ? ctx->hi-take : ctx->lo;
        if (mprotect(at, (uz)take, PROT_READ|PROT_WRITE)) {
            os_oom(ctx);  // the commit limit, or a data limit (ulimit -d)
        }
        if (a->down) {
            ctx->hi = at;
            a->beg  = at;
        } else {
            ctx->lo = at + take;
            a->end  = ctx->lo;
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

static b32 os_missing(os *ctx)
{
    (void)ctx;
    return errno==ENOENT || errno==ENOTDIR;
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

// Entries tell only names: each is left for os_stat. The names come
// first, packed one after another with their terminators, then entries
// for them all at once: an array doubled as it grew would, in an arena
// that grows down (scratch), leave each smaller copy behind, some 265
// bytes a name in all, where this takes 89 and the name.
static os_dirent *os_listdir(os *ctx, s8 path, b32 all, iz *count,
                             arena *a)
{
    (void)ctx;
    (void)all;  // no hidden or system attributes
    arena tmp = *a;  // the path, which the names then overwrite
    DIR  *d   = opendir(tocstr(&tmp, path));
    if (!d) {
        return 0;
    }
    u8 *first = (u8 *)(a->down ? a->end : a->beg);  // where names begin
    u8 *last  = first;                              // and end
    iz  n     = 0;
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
        u8 *copy = newstr(a, name.len+1);
        assert(a->down ? copy+name.len+1==last : copy==last);  // packed
        bytecopy(copy, name.s, name.len+1);
        last = a->down ? copy : copy+name.len+1;
        n++;
    }
    int err = errno;
    closedir(d);
    if (err) {
        return 0;
    }
    os_dirent *list = new(a, n, os_dirent);  // type FT_NONE
    u8        *p    = MIN(first, last);
    for (iz i = 0; i < n; i++) {
        list[i].name = cstr((char *)p);
        p += list[i].name.len + 1;
    }
    *count = n;
    return list;
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
// that Linux's protected_symlinks refuses in a sticky directory, nor an
// empty one. So a chain may be as long as stat follows, up to Linux's 40
// links (others allow fewer), leaving one more read to find the file at
// its end.
static s8 os_resolve(os *ctx, s8 path, arena *perm, arena scratch)
{
    struct stat st;
    if (stat(tocstr(&scratch, path), &st) && errno!=ENOENT) {
        return (s8){0};
    }
    for (i32 links = 0;; links++) {
        s8 target = os_readlink(ctx, path, &scratch);
        if (!target.s) {
            // Not a link (EINVAL), or nothing there
            return errno==EINVAL || errno==ENOENT ? path : (s8){0};
        } else if (!target.len) {
            errno = ENOENT;  // as stat finds it (BSD, macOS; Linux has none)
            return (s8){0};
        } else if (links == 40) {
            errno = ELOOP;  // a chain grown since stat followed it
            return (s8){0};
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
        if (r<0 && (errno==EINTR || (errno==EAGAIN && wait_for_input(fd)))) {
            continue;  // opened non-blocking (OS_REGULAR), as os_read waits
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

// Move a file to a path where nothing was, failing if anything has
// appeared there since: by linking it there, then unlinking it, or where
// the file system has no hard links (FAT, some network file systems), by
// renaming it, the best that POSIX offers.
static b32 place(char *src, char *dst)
{
    if (!link(src, dst)) {
        unlink(src);
        return 1;
    }
    return errno!=EEXIST && !rename(src, dst);
}

static i32 os_commit(os *ctx, i32 fd, s8 temp, s8 path, b32 replace,
                     arena scratch)
{
    (void)temp;  // pending_output
    char *dst = tocstr(&scratch, path);
    struct stat st;
    if (!stat(dst, &st)) {
        // The old group, where the user may give it (a member, or root),
        // else its permissions would go to another group (the user's, or
        // on BSD the directory's), which then gets only those of others.
        // Set-ID and sticky bits too, as Info-ZIP keeps them, but only
        // while the owner and group are the old ones: otherwise set-ID
        // bits would run the program as someone the old file did not.
        // Should the system refuse them (BSD's sticky files), the rest.
        mode_t      mode  = st.st_mode & 0777;
        struct stat self;
        b32         known = !fstat(fd, &self);
        if (known && self.st_gid!=st.st_gid &&
                !fchown(fd, (uid_t)-1, st.st_gid)) {
            self.st_gid = st.st_gid;
        }
        if (!known || self.st_gid!=st.st_gid) {
            mode = (mode_t)((mode & 0707) | (mode & 07)<<3);
        } else if (self.st_uid == st.st_uid) {
            mode = st.st_mode & 07777;
        }
        if (fchmod(fd, mode)) {
            fchmod(fd, mode & 0777);
        }
    } else if (!ctx->defperms && os_missing(ctx)) {
        // The archive existed at startup, so the temp file is owner-only,
        // but it has since gone and this becomes a new file. Give it the
        // usual 0666 less the umask. Unlike OS_DEFPERMS, this ignores a
        // default ACL, and the target may still change before the rename.
        // Should the archive be there but not examined (an I/O error),
        // its mode is unknown, and owner-only is the safe guess, as in
        // Info-ZIP, which then leaves its temporary file's mode too.
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
    i32 r = ok ? COMMIT_OK : COMMIT_ECLOSE;

    sigset_t old = block_signals();
    if (ok && replace) {
        ok = !rename(pending_output, dst);
    } else if (ok) {
        ok = place(pending_output, dst);
    }
    r = r ? r : ok ? COMMIT_OK : COMMIT_EREPLACE;
    int err = errno;  // why, rather than why the discarding failed
    release_output(ok);
    restore_signals(old);
    errno = err;
    return r;
}

static s8 os_fromcp(os *ctx, s8 name, b32 oem, arena *perm, arena scratch)
{
    (void)ctx;
    (void)name;
    (void)oem;
    (void)perm;
    (void)scratch;
    return (s8){0};  // never asked: only Windows decodes code pages
}

static s8 os_upcase(os *ctx, s8 name, arena *a)
{
    (void)ctx;
    (void)a;
    return name;  // never asked: only Windows expands wildcards
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
