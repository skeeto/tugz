// POSIX file system functions for tugz zip and unzip: the arena's
// reservation, examining files, listing directories, positioned reads,
// local time. Included by the programs' platform layers, after posix.c.

#include <dirent.h>
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

// Without anonymous mappings, failing to open /dev/zero writes oom and
// exits with status; otherwise running out is os_oom's.
static void reserve(os *ctx, s8 oom, i32 status)
{
    int flags = MAP_PRIVATE;
    int fd    = -1;
#ifdef MAP_ANONYMOUS
    flags |= MAP_ANONYMOUS;
    (void)oom;
    (void)status;
#else
    fd = open("/dev/zero", O_RDONLY);
    if (fd < 0) {
        os_write(ctx, 2, oom.s, oom.len);
        os_exit(ctx, status);
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
