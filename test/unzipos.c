// In-memory platform layer for the unzip program, shared by its unit
// tests (test/unzipclitests.c) and its libFuzzer harness
// (test/fuzz_unzip.c)
//
// Runs unzip_main (src/unzipcli.c) over a file system in memory: files,
// directories, and symbolic links, each path resolved as a kernel
// resolves it, every component before the last followed through links,
// and the last too where the call follows, so that a write through a
// link lands where the link leads, and is seen there. Components longer
// than 255 bytes, and paths of 4096, are refused, as on Linux. Standard
// input is scripted, as the prompts' answers or as an archive (unzip -),
// and standard output and error are captured, apart and interleaved.
// The umask, which descriptors are terminals, and Windows conventions
// (names that differ only in ASCII case are one, links are made as files
// holding their targets, no umask) are set per run; local time is UTC.
//
// Faults to inject: the archive shrinking to a length, or its reads
// failing past an offset, from when it is opened or from the first
// change to the file system; writes to extracted files failing past a
// total; the nth directory, file, or keep failing; attributes failing;
// links failing.
//
// Memory is one reservation committed exactly as asked, and once a run
// has changed the file system (made a directory, created, removed, or
// linked a file, or set attributes) since it opened the archive it is
// processing, committing more traps, faults or not: unzip claims all an
// archive needs before its output, so that running out of memory cannot
// strike once it has begun to write. One exception is allowed:
// once a new name has been asked for (the prompt's [r]ename), a link or
// directory renamed so is kept in perm to the end, as src/unzipcli.c
// notes where it claims the room for each entry. (The POSIX layer's
// os_open also allocates its pending output's path, with malloc, outside
// the arenas: a failure there is an ordinary failure to create a file.)
// What is committed is first filled with what a use of it before it is
// set would likely trip over (see os_extend), and under AddressSanitizer
// the uncommitted part is poisoned.
//
// Every call that changes the file system is checked to be to the -d
// directory (unzipos's dest), or below it reached through no link and
// no "." or ".." component, as unzip must never write elsewhere.
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

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define UNZIPOS_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(UNZIPOS_ASAN)
#  include <sanitizer/asan_interface.h>
#  define POISON(p, n)    ASAN_POISON_MEMORY_REGION(p, (uz)(n))
#  define UNPOISON(p, n)  ASAN_UNPOISON_MEMORY_REGION(p, (uz)(n))
#else
#  define POISON(p, n)    (void)0
#  define UNPOISON(p, n)  (void)0
#endif

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
            fflush(stderr); \
            __builtin_trap(); \
        } \
    } while (0)

enum {
    MFS_FILES   = 4096,
    MFS_FDS     = 16,
    MFS_NAMEMAX = 255,      // NAME_MAX, a component's longest
    MFS_PATHMAX = 4096,     // PATH_MAX, with its terminator
    MFS_LINKS   = 40,       // links followed in a path, as Linux's
    MFS_OUTMAX  = 1 << 26,  // a captured stream's, past which writes fail
};

typedef struct {
    s8  name;     // canonical: relative, without "." or "..", or links
    u8 *data;     // contents, or a link's target
    iz  len;
    iz  cap;
    i32 type;     // FT_FILE, FT_DIR, FT_LINK, or FT_OTHER
    u32 perm;     // 07777
    u32 dosattr;  // as set by unzip (Windows conventions)
    u32 uid;
    u32 gid;
    i64 mtime;
    i64 atime;
    u64 ino;
    i32 opens;    // open descriptors, which keep a removed file's slot
    b32 live;
} mfile;

typedef struct {
    u8 *s;
    iz  len;
    iz  cap;
} mbuf;

// When faults in the archive begin
enum {
    FAULT_OPEN = 1,  // as unzip opens it
    FAULT_CHANGE,    // as unzip first changes the file system
};

struct os {
    jmp_buf jmp;
    i32     status;  // given to os_exit
    b32     exited;  // the run ended by os_exit

    mfile   files[MFS_FILES];
    i32     nfiles;  // slots ever used
    mfile   top;     // the root directory, ""
    u64     nextino;
    i64     now;     // the time given new files

    struct {
        i32 file;
        iz  off;
        b32 open;
        b32 created;  // discarded on close unless kept
        b32 kept;
    } fds[MFS_FDS];

    // Standard input, read a few bytes at a time, or with stdinfile, a
    // regular file, read in place
    u8     *in;
    iz      inlen;
    iz      inoff;
    b32     stdinfile;
    mbuf    out[3];   // standard output, error, and both interleaved
    b32     tty[3];   // which standard descriptors are terminals

    b32     windows;  // Windows conventions
    u32     umask;
    s8      dest;     // unzip's root: the -d directory and a '/', or ""
    char   *error;    // why the last failing call failed
    b32     missing;  // the last failure found nothing there

    byte   *mem;      // the reservation
    iz      cap;
    byte   *lo;       // perm committed below, scratch from hi up
    byte   *hi;
    b32     changed;  // the file system changed since the archive opened
    b32     faulted;  // a fault was injected
    b32     renamed;  // a new name was asked for

    // Faults: from when, the archive (the file so named, or standard
    // input) shrinks to a length, and reads of it past an offset fail
    // (-1 for neither); writes to created files past a total fail (-1
    // for none); the nth mkdir, create, or keep fails (0 for none);
    // os_setattrs and os_setdirattrs fail for the OS_A* flags given;
    // links fail
    char   *archive;
    i32     when;
    i64     shrinkto;
    i64     failreadat;
    i64     failwriteat;
    i32     failmkdir;
    i32     failcreate;
    i32     failkeep;
    i32     failattrs;
    b32     failsymlink;
    b32     armed;  // the archive's faults have begun

    // Counted per run
    i64     written;
    i32     nmkdir;
    i32     ncreate;
    i32     nkeep;
    i32     nchanges;  // calls that changed the file system
};

static s8 cstrs8(char const *z)
{
    return (s8){(u8 *)z, (iz)strlen(z)};
}

static u8 mfs_fold(os *ctx, u8 c)
{
    return ctx->windows && c>='A' && c<='Z' ? c|0x20 : c;
}

// Whether two names are one, as the file system compares them.
static b32 mfs_eq(os *ctx, s8 a, s8 b)
{
    if (a.len != b.len) {
        return 0;
    }
    for (iz i = 0; i < a.len; i++) {
        if (mfs_fold(ctx, a.s[i]) != mfs_fold(ctx, b.s[i])) {
            return 0;
        }
    }
    return 1;
}

static mfile *mfs_lookup(os *ctx, s8 name)
{
    if (!name.len) {
        return &ctx->top;
    }
    for (i32 i = 0; i < ctx->nfiles; i++) {
        mfile *f = ctx->files + i;
        if (f->live && mfs_eq(ctx, f->name, name)) {
            return f;
        }
    }
    return 0;
}

static void mfs_setlen(mfile *f, iz len)
{
    if (len > f->cap) {
        f->cap  = MAX(2*f->cap, len);
        f->data = realloc(f->data, (uz)f->cap);
        CHECK(f->data);
    }
    if (len > f->len) {
        memset(f->data+f->len, 0, (uz)(len-f->len));
    }
    f->len = len;
}

// A new file by its canonical name, which must not be taken.
static mfile *mfs_new(os *ctx, s8 name, i32 type)
{
    CHECK(name.len && !mfs_lookup(ctx, name));
    i32 i = 0;
    for (; i<ctx->nfiles && (ctx->files[i].live || ctx->files[i].opens);
         i++) {}
    CHECK(i < MFS_FILES);
    ctx->nfiles = MAX(ctx->nfiles, i+1);
    mfile *f = ctx->files + i;
    free(f->name.s);
    f->name.s   = malloc((uz)name.len + 1);
    f->name.len = name.len;
    CHECK(f->name.s);
    bytecopy(f->name.s, name.s, name.len);
    f->len     = 0;
    f->type    = type;
    f->perm    = type==FT_DIR ? 0755 : type==FT_LINK ? 0777 : 0644;
    f->dosattr = 0;
    f->uid     = f->gid = 1000;
    f->mtime   = f->atime = ctx->now;
    f->ino     = ++ctx->nextino;
    f->live    = 1;
    return f;
}

// Results of resolving a path
enum {
    MFS_OK,
    MFS_ENOENT,
    MFS_ENOTDIR,
    MFS_ELOOP,
    MFS_ENAMETOOLONG,
};

typedef struct {
    i32    err;
    mfile *f;     // what the path names, or null if nothing (yet)
    u8     name[MFS_PATHMAX];  // its canonical name
    iz     len;
} mpath;

// Resolve a path as a kernel does: from the top, each component looked
// up in the directory reached, a link followed to its target, relative
// to the link's directory or else from the top, for each component but
// the last, which is followed only given follow (or a '/' after it).
// Without error, f is null where only the last component is missing.
static void mfs_resolve(os *ctx, s8 path, b32 follow, mpath *r)
{
    u8 rem[2*MFS_PATHMAX];
    iz rlen = path.len;
    r->err = MFS_OK;
    r->f   = 0;
    r->len = 0;
    if (!path.len) {
        r->err = MFS_ENOENT;
        return;
    } else if (path.len >= MFS_PATHMAX) {
        r->err = MFS_ENAMETOOLONG;
        return;
    }
    bytecopy(rem, path.s, rlen);
    i32 links = 0;
    for (iz i = 0;;) {
        for (; i<rlen && rem[i]=='/'; i++) {}
        if (i == rlen) {
            r->f = mfs_lookup(ctx, (s8){r->name, r->len});  // a directory
            return;
        }
        iz j = i;
        for (; j<rlen && rem[j]!='/'; j++) {}
        s8  comp  = {rem+i, j-i};
        iz  k     = j;
        for (; k<rlen && rem[k]=='/'; k++) {}
        b32 last  = k == rlen;
        b32 slash = j < rlen;  // so it must be a directory
        i = j;
        if (comp.len==1 && comp.s[0]=='.') {
            continue;
        } else if (comp.len==2 && comp.s[0]=='.' && comp.s[1]=='.') {
            for (; r->len>0 && r->name[r->len-1]!='/'; r->len--) {}
            r->len -= r->len > 0;
            continue;
        } else if (comp.len>MFS_NAMEMAX || r->len+1+comp.len>=MFS_PATHMAX) {
            r->err = MFS_ENAMETOOLONG;
            return;
        }

        iz save = r->len;
        if (r->len) {
            r->name[r->len++] = '/';
        }
        bytecopy(r->name+r->len, comp.s, comp.len);
        r->len += comp.len;
        mfile *f = mfs_lookup(ctx, (s8){r->name, r->len});
        if (!f) {
            r->err = last ? MFS_OK : MFS_ENOENT;
            return;
        }

        if (f->type==FT_LINK && (!last || follow || slash)) {
            u8 tmp[2*MFS_PATHMAX];
            s8 rest = {rem+j, rlen-j};
            if (++links > MFS_LINKS) {
                r->err = MFS_ELOOP;
                return;
            } else if (!f->len) {
                r->err = MFS_ENOENT;
                return;
            } else if (f->len+rest.len >= countof(tmp)) {
                r->err = MFS_ENAMETOOLONG;
                return;
            }
            bytecopy(tmp, f->data, f->len);
            bytecopy(tmp+f->len, rest.s, rest.len);
            rlen = f->len + rest.len;
            bytecopy(rem, tmp, rlen);
            r->len = f->data[0]=='/' ? 0 : save;
            i = 0;
            continue;
        }
        if ((!last || slash) && f->type!=FT_DIR) {
            r->err = MFS_ENOTDIR;
            return;
        } else if (last) {
            r->f = f;
            return;
        }
    }
}

static void mfs_fail(os *ctx, char *error, b32 missing)
{
    ctx->error   = error;
    ctx->missing = missing;
}

static void mfs_fault(os *ctx, char *error)
{
    ctx->faulted = 1;
    mfs_fail(ctx, error, 0);
}

// Report a failed resolution as errno would. Returns false.
static b32 mfs_failed(os *ctx, i32 err)
{
    switch (err) {
    case MFS_ENOENT:
        mfs_fail(ctx, "No such file or directory", 1);
        break;
    case MFS_ENOTDIR:
        mfs_fail(ctx, "Not a directory", 1);
        break;
    case MFS_ELOOP:
        mfs_fail(ctx, "Too many levels of symbolic links", 0);
        break;
    case MFS_ENAMETOOLONG:
        mfs_fail(ctx, "File name too long", 0);
        break;
    }
    return 0;
}

// Begin the archive's faults, if they begin now.
static void mfs_arm(os *ctx, i32 when)
{
    if (ctx->when==when && !ctx->armed) {
        ctx->armed = 1;
        if (ctx->shrinkto >= 0) {
            mfile *f = mfs_lookup(ctx, cstrs8(ctx->archive));
            if (f && f->type==FT_FILE) {
                f->len = MIN(f->len, (iz)ctx->shrinkto);
            }
            ctx->inlen = ctx->stdinfile ? MIN(ctx->inlen, (iz)ctx->shrinkto)
                                        : ctx->inlen;
            ctx->faulted = 1;
        }
    }
}

// Note a change to the file system, before making it.
static void mfs_changed(os *ctx)
{
    ctx->nchanges++;
    ctx->now++;
    if (!ctx->changed) {
        ctx->changed = 1;
        mfs_arm(ctx, FAULT_CHANGE);
    }
}

// Check a change to the file system, at path, before making it: to the
// -d directory itself, or below it, through no link, nor "." or "..".
static void mfs_change(os *ctx, s8 path)
{
    s8  d    = ctx->dest;
    b32 self = d.len && path.len==d.len-1 &&
               !memcmp(path.s, d.s, (uz)path.len);
    if (!self) {
        CHECK(path.len>d.len && !memcmp(path.s, d.s, (uz)d.len));
        for (iz k = d.len; k < path.len;) {
            iz j = k;
            for (; j<path.len && path.s[j]!='/'; j++) {}
            s8 comp = {path.s+k, j-k};
            CHECK(comp.len);
            CHECK(!s8equals(comp, S(".")) && !s8equals(comp, S("..")));
            if (j < path.len) {
                mpath *r = malloc(sizeof(*r));
                CHECK(r);
                mfs_resolve(ctx, (s8){path.s, j}, 0, r);
                CHECK(!r->err && r->f && r->f->type==FT_DIR);
                free(r);
            }
            k = j + 1;
        }
    }
    mfs_changed(ctx);
}

static mfile *mfs_fd(os *ctx, i32 fd)
{
    CHECK(fd>2 && fd<MFS_FDS && ctx->fds[fd].open);
    return ctx->files + ctx->fds[fd].file;
}

static i32 mfs_newfd(os *ctx, mfile *f, b32 created)
{
    i32 fd = 3;
    for (; fd<MFS_FDS && ctx->fds[fd].open; fd++) {}
    CHECK(fd < MFS_FDS);
    ctx->fds[fd].file    = (i32)(f - ctx->files);
    ctx->fds[fd].off     = 0;
    ctx->fds[fd].open    = 1;
    ctx->fds[fd].created = created;
    ctx->fds[fd].kept    = 0;
    f->opens++;
    return fd;
}

static b32 mfs_isarchive(os *ctx, mfile *f)
{
    return s8equals(f->name, cstrs8(ctx->archive));
}

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    (void)scratch;
    CHECK(!(mode & OS_FORCE));  // unzip removes, then creates
    mpath *r = malloc(sizeof(*r));
    CHECK(r);
    i32 fd = OS_ERR;

    if (mode & OS_CREATE) {
        mfs_change(ctx, path);
        mfs_resolve(ctx, path, 0, r);
        if (r->err) {
            mfs_failed(ctx, r->err);
        } else if (r->f) {
            mfs_fail(ctx, "File exists", 0);  // even a dangling link
            fd = OS_EEXIST;
        } else if (++ctx->ncreate == ctx->failcreate) {
            mfs_fault(ctx, "Permission denied");
        } else {
            mfile *f = mfs_new(ctx, (s8){r->name, r->len}, FT_FILE);
            f->perm = 0600;  // owner-only until unzip sets its mode
            fd = mfs_newfd(ctx, f, 1);
        }
        free(r);
        return fd;
    }

    // Changes before an archive is opened are earlier archives'
    ctx->changed = 0;
    mfs_resolve(ctx, path, !(mode & OS_NOFOLLOW), r);
    mfile *f = r->f;
    if (r->err) {
        mfs_failed(ctx, r->err);
    } else if (!f) {
        mfs_failed(ctx, MFS_ENOENT);
    } else if (f->type == FT_LINK) {
        fd = OS_ESYMLINK;
    } else if (f->type == FT_DIR) {
        fd = OS_EISDIR;
    } else if (f->type==FT_OTHER && (mode & OS_REGULAR)) {
        fd = OS_ENOTREG;
    } else {
        if (mfs_isarchive(ctx, f)) {
            mfs_arm(ctx, FAULT_OPEN);
        }
        fd = mfs_newfd(ctx, f, 0);
    }
    free(r);
    return fd;
}

static void mfs_unlink(mfile *f)
{
    f->live = 0;
}

static b32 os_close(os *ctx, i32 fd)
{
    mfile *f = mfs_fd(ctx, fd);
    ctx->fds[fd].open = 0;
    f->opens--;
    if (ctx->fds[fd].created && !ctx->fds[fd].kept && f->live) {
        mfs_unlink(f);  // discarded, as never kept
    }
    return 1;
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    CHECK(fd==0 && cap>0);  // unzip reads only its answers, or an archive
    iz n = MIN(MIN(cap, ctx->inlen-ctx->inoff), 7);  // short reads
    if (ctx->armed && ctx->failreadat>=0 && ctx->inoff+n>ctx->failreadat &&
        s8equals(cstrs8(ctx->archive), S("-"))) {
        mfs_fault(ctx, "Input/output error");
        return -1;
    }
    if (n > 0) {
        bytecopy(buf, ctx->in+ctx->inoff, n);
        ctx->inoff += n;
    }
    return MAX(n, 0);
}

static void mbuf_put(mbuf *b, u8 const *p, iz len)
{
    if (b->len+len > b->cap) {
        b->cap = MAX(2*b->cap, b->len+len);
        b->s   = realloc(b->s, (uz)b->cap);
        CHECK(b->s);
    }
    if (len) {
        bytecopy(b->s+b->len, p, len);
    }
    b->len += len;
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    CHECK(len >= 0);
    if (fd==1 || fd==2) {
        mbuf *b = ctx->out + fd - 1;
        if (b->len+len > MFS_OUTMAX) {
            mfs_fault(ctx, "File too large");
            return 0;
        }
        mbuf_put(b, buf, len);
        mbuf_put(ctx->out+2, buf, len);
        if (fd==2 && s8equals((s8){buf, len}, S("new name: "))) {
            ctx->renamed = 1;
        }
        return 1;
    }
    mfile *f = mfs_fd(ctx, fd);
    CHECK(ctx->fds[fd].created && !ctx->fds[fd].kept);
    if (ctx->failwriteat>=0 && ctx->written+len>ctx->failwriteat) {
        mfs_fault(ctx, "No space left on device");
        return 0;
    } else if (f->len+len > MFS_OUTMAX) {
        mfs_fault(ctx, "File too large");
        return 0;
    }
    ctx->written += len;
    iz off = f->len;
    mfs_setlen(f, off+len);
    bytecopy(f->data+off, buf, len);
    return 1;
}

static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)scratch;
    mfs_change(ctx, path);
    mpath *r = malloc(sizeof(*r));
    CHECK(r);
    mfs_resolve(ctx, path, 0, r);
    b32 ok = 0;
    if (r->err) {
        mfs_failed(ctx, r->err);
    } else if (!r->f) {
        mfs_failed(ctx, MFS_ENOENT);
    } else if (r->f->type == FT_DIR) {
        mfs_fail(ctx, "Is a directory", 0);  // unlink(2), as Linux
    } else {
        mfs_unlink(r->f);
        ok = 1;
    }
    free(r);
    return ok;
}

static b32 os_keep(os *ctx, i32 fd)
{
    mfs_fd(ctx, fd);
    CHECK(ctx->fds[fd].created && !ctx->fds[fd].kept);
    if (++ctx->nkeep == ctx->failkeep) {
        mfs_fault(ctx, "Access is denied");  // as Windows may refuse
        return 0;
    }
    ctx->fds[fd].kept = 1;
    return 1;
}

static void os_exit(os *ctx, i32 status)
{
    ctx->status = status;
    ctx->exited = 1;
    longjmp(ctx->jmp, 1);
}

static void mfs_info(mfile *f, os_info *info)
{
    *info = (os_info){0};
    info->type   = f->type;
    info->size   = f->type==FT_DIR ? 0 : f->len;
    info->mtime  = f->mtime;
    info->atime  = f->atime;
    info->mode   = f->perm | (f->type==FT_DIR  ? 0040000 :
                              f->type==FT_LINK ? 0120000 :
                              f->type==FT_FILE ? 0100000 : 0010000);
    info->attr   = f->type==FT_DIR ? 0x10 : f->dosattr;
    info->uid    = f->uid;
    info->gid    = f->gid;
    info->dev    = 1;
    info->ino[0] = f->ino;
}

static b32 os_stat(os *ctx, s8 path, b32 follow, os_info *info,
                   arena scratch)
{
    (void)scratch;
    mpath *r = malloc(sizeof(*r));
    CHECK(r);
    mfs_resolve(ctx, path, follow, r);
    b32 ok = 0;
    if (r->err) {
        mfs_failed(ctx, r->err);
    } else if (!r->f) {
        mfs_failed(ctx, MFS_ENOENT);
    } else {
        mfs_info(r->f, info);
        ok = 1;
    }
    free(r);
    return ok;
}

static b32 os_missing(os *ctx)
{
    return ctx->missing;
}

static b32 os_fstat(os *ctx, i32 fd, os_info *info)
{
    if (fd == 0) {
        if (s8equals(cstrs8(ctx->archive), S("-"))) {
            ctx->changed = 0;  // as os_open, for the archive
            mfs_arm(ctx, FAULT_OPEN);
        }
        *info = (os_info){0};
        info->type = ctx->stdinfile ? FT_FILE : FT_OTHER;  // or a pipe
        info->size = ctx->stdinfile ? ctx->inlen : 0;
        info->mode = ctx->stdinfile ? 0100644 : 0010600;
        return 1;
    }
    mfs_info(mfs_fd(ctx, fd), info);
    return 1;
}

static i32 os_readat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    CHECK(len>=0 && off>=0);
    u8 *data = ctx->in;
    iz  size = ctx->inlen;
    if (fd) {
        mfile *f = mfs_fd(ctx, fd);
        CHECK(!ctx->fds[fd].created);
        data = f->data;
        size = f->len;
    } else {
        CHECK(ctx->stdinfile);
    }
    if (ctx->armed && ctx->failreadat>=0 && off+len>ctx->failreadat) {
        mfs_fault(ctx, "Input/output error");
        return -1;
    }
    iz n = (iz)MIN(len, MAX(size-off, 0));
    if (n) {
        bytecopy(buf, data+off, n);
    }
    return n==len ? 1 : 0;
}

static void os_localtime(os *ctx, i64 t, i32 tm[6])
{
    (void)ctx;
    zip_gmtime(t, tm);
}

static i64 os_mktime(os *ctx, i32 const tm[6])
{
    (void)ctx;
    return uz_timegm(tm);
}

static u32 os_umask(os *ctx)
{
    return ctx->windows ? 0 : ctx->umask;
}

static b32 os_isatty(os *ctx, i32 fd)
{
    return fd>=0 && fd<=2 && ctx->tty[fd];
}

static s8 os_error(os *ctx)
{
    return ctx->error ? cstrs8(ctx->error) : S("");
}

// Decoded as Latin-1, unlike any real code page, which is enough to
// give names in one a second, Unicode, name (Windows conventions).
static s8 os_fromcp(os *ctx, s8 name, b32 oem, arena *perm, arena scratch)
{
    (void)ctx; (void)oem; (void)scratch;
    s8 r = {newstr(perm, 2*name.len), 0};
    for (iz i = 0; i < name.len; i++) {
        u8 c = name.s[i];
        if (c < 0x80) {
            r.s[r.len++] = c;
        } else {
            r.s[r.len++] = (u8)(0xc0 | c>>6);
            r.s[r.len++] = (u8)(0x80 | (c & 0x3f));
        }
    }
    return r;
}

static s8 os_upcase(os *ctx, s8 name, arena *a)
{
    (void)ctx; (void)a;
    return name;  // ASCII case is folded by the matching itself
}

// The entries directly within a directory, "." for the top, left for
// os_stat to examine.
static os_dirent *os_listdir(os *ctx, s8 path, b32 all, iz *count,
                             arena *a)
{
    (void)all;
    mpath *r = malloc(sizeof(*r));
    CHECK(r);
    mfs_resolve(ctx, path, 1, r);
    if (r->err || !r->f || r->f->type!=FT_DIR) {
        mfs_failed(ctx, r->err ? r->err : MFS_ENOTDIR);
        free(r);
        return 0;
    }
    s8 dir = r->f->name;
    iz pre = dir.len ? dir.len+1 : 0;
    iz n   = 0;
    for (i32 pass = 0; pass < 2; pass++) {
        os_dirent *list = pass ? new(a, n, os_dirent) : 0;
        iz         m    = 0;
        for (i32 i = 0; i < ctx->nfiles; i++) {
            mfile *f  = ctx->files + i;
            s8     s  = f->name;
            b32    in = f->live && s.len>pre &&
                        (!pre || (mfs_eq(ctx, (s8){s.s, dir.len}, dir) &&
                                  s.s[dir.len]=='/'));
            for (iz j = pre; in && j < s.len; j++) {
                in = s.s[j] != '/';
            }
            if (in && pass) {
                list[m].name.s   = newstr(a, s.len-pre);
                list[m].name.len = s.len - pre;
                bytecopy(list[m].name.s, s.s+pre, s.len-pre);
            }
            m += in;
        }
        n = m;
        if (pass) {
            free(r);
            *count = n;
            return list;
        }
    }
    return 0;
}

static b32 os_mkdir(os *ctx, s8 path, arena scratch)
{
    (void)scratch;
    mfs_change(ctx, path);
    mpath *r = malloc(sizeof(*r));
    CHECK(r);
    mfs_resolve(ctx, path, 0, r);
    b32 ok = 0;
    if (r->err) {
        mfs_failed(ctx, r->err);
    } else if (r->f) {
        mfs_fail(ctx, "File exists", 0);
    } else if (++ctx->nmkdir == ctx->failmkdir) {
        mfs_fault(ctx, "Permission denied");
    } else {
        mfile *f = mfs_new(ctx, (s8){r->name, r->len}, FT_DIR);
        f->perm = 0777 & ~os_umask(ctx);
        ok = 1;
    }
    free(r);
    return ok;
}

// Give a file its attributes, but those failing by injection.
static i32 mfs_setattrs(os *ctx, mfile *f, osattrs *a, s8 *why)
{
    i32 failed = a->flags & ctx->failattrs & (OS_AOWNER|OS_AMODE|OS_ATIMES);
    for (i32 b = 0; b < 3; b++) {
        if (failed & 1<<b) {
            why[b] = S("Operation not permitted");
            ctx->faulted = 1;
        }
    }
    i32 ok = a->flags & ~failed;
    if (ok & OS_AOWNER) {
        f->uid = a->uid;
        f->gid = a->gid;
    }
    if (ok & OS_AMODE) {
        u32 sgid = a->flags&OS_ASGID ? f->perm&02000 : 0;
        f->perm    = (a->mode & 07777) | sgid;
        f->dosattr = a->dosattr;
    }
    if (ok & OS_ATIMES) {
        f->mtime = a->mtime;
        f->atime = a->atime;
    }
    return failed;
}

static i32 os_setattrs(os *ctx, i32 fd, osattrs *attrs, s8 *why, arena *a)
{
    (void)a;
    mfile *f = mfs_fd(ctx, fd);
    CHECK(ctx->fds[fd].created && !ctx->fds[fd].kept);
    mfs_changed(ctx);
    return mfs_setattrs(ctx, f, attrs, why);
}

static i32 os_setdirattrs(os *ctx, s8 path, osattrs *attrs, s8 *why,
                          arena *a)
{
    (void)a;
    mfs_change(ctx, path);
    mpath *r = malloc(sizeof(*r));
    CHECK(r);
    mfs_resolve(ctx, path, 0, r);  // O_NOFOLLOW|O_DIRECTORY
    i32 failed = 0;
    if (r->err || !r->f || r->f->type!=FT_DIR) {
        mfs_failed(ctx, r->err ? r->err : !r->f ? MFS_ENOENT :
                        r->f->type==FT_LINK ? MFS_ELOOP : MFS_ENOTDIR);
        why[0] = why[1] = why[2] = os_error(ctx);
        failed = attrs->flags & (OS_AOWNER|OS_AMODE|OS_ATIMES);
    } else {
        failed = mfs_setattrs(ctx, r->f, attrs, why);
    }
    free(r);
    return failed;
}

static i32 os_symlink(os *ctx, s8 target, s8 path, osattrs *attrs, s8 *why,
                      arena *a)
{
    (void)a;
    mfs_change(ctx, path);
    // Up to a NUL, as symlink(2) takes a C string, but on Windows all of
    // it, written to a file
    iz tlen = 0;
    for (; tlen<target.len && (target.s[tlen] || ctx->windows); tlen++) {}
    mpath *r = malloc(sizeof(*r));
    CHECK(r);
    mfs_resolve(ctx, path, 0, r);
    i32 failed = 0;
    if (r->err) {
        mfs_failed(ctx, r->err);
        failed = OS_ALINK;
    } else if (r->f) {
        mfs_fail(ctx, "File exists", 0);
        failed = OS_ALINK;
    } else if (ctx->failsymlink) {
        mfs_fault(ctx, "Operation not permitted");
        failed = OS_ALINK;
    } else if (!tlen && !ctx->windows) {
        mfs_failed(ctx, MFS_ENOENT);
        failed = OS_ALINK;
    } else {
        // On Windows, a file holding its target
        mfile *f = mfs_new(ctx, (s8){r->name, r->len},
                           ctx->windows ? FT_FILE : FT_LINK);
        mfs_setlen(f, tlen);
        if (tlen) {
            bytecopy(f->data, target.s, tlen);
        }
        if (attrs->flags & OS_AOWNER) {
            osattrs own = {0};
            own.uid   = attrs->uid;
            own.gid   = attrs->gid;
            own.flags = OS_AOWNER;
            failed = mfs_setattrs(ctx, f, &own, why);
        }
    }
    if (failed & OS_ALINK) {
        why[3] = os_error(ctx);
    }
    free(r);
    return failed;
}

// As the platform layers commit memory, but exactly what is wanted, and
// nothing once the file system has changed (but for a renamed entry kept
// in perm). Committed memory is filled as though with garbage.
static void os_extend(os *ctx, arena *a, iz need)
{
    if (a->down) {
        a->beg = ctx->hi;
    } else if (a->end != ctx->lo) {
        os_oom(ctx);  // a fixed arena
    }
    iz want = need - (a->end - a->beg);
    if (want > ctx->hi-ctx->lo) {
        os_oom(ctx);
    } else if (want > 0) {
        CHECK(!ctx->changed || (ctx->renamed && !a->down));
        byte *at = a->down ? ctx->hi-want : ctx->lo;
        UNPOISON(at, want);
        // As garbage, whatever was there: here a large negative 64-bit
        // number in each aligned word (INT64_MIN), which arithmetic on
        // what was never set is likely to overflow, at both ends (where
        // the allocations that asked lie) of a large commit
        iz ends = MIN(want, 1<<14);
        for (i32 end = 0; end < 2; end++) {
            byte *p = end ? at+want-ends : at;
            memset(p, 0, (uz)ends);
            for (iz i = 7 - (iz)((uptr)p & 7); i < ends; i += 8) {
                p[i] = 0x80;
            }
        }
        if (a->down) {
            ctx->hi = at;
            a->beg  = at;
        } else {
            ctx->lo = at + want;
            a->end  = ctx->lo;
        }
    }
}

// Building the file system

// Add a file, a directory (FT_DIR, without data), a link (FT_LINK, its
// target as its data), or a special file (FT_OTHER), by its canonical
// name, its directories already there.
static mfile *mfs_put(os *ctx, char const *name, i32 type, void const *p,
                      iz len, i64 mtime)
{
    s8 n = cstrs8(name);
    for (iz k = 0; k < n.len; k++) {
        if (n.s[k] == '/') {
            mfile *d = mfs_lookup(ctx, (s8){n.s, k});
            CHECK(d && d->type==FT_DIR);
        }
    }
    mfile *f = mfs_new(ctx, n, type);
    mfs_setlen(f, len);
    if (len) {
        bytecopy(f->data, p, len);
    }
    f->mtime = f->atime = mtime;
    return f;
}

// A file by its canonical name, or null.
static mfile *mfs_get(os *ctx, char const *name)
{
    return mfs_lookup(ctx, cstrs8(name));
}

// A file's contents, or a link's target, or a null string if none.
[[maybe_unused]] static s8 mfs_data(os *ctx, char const *name)
{
    mfile *f = mfs_get(ctx, name);
    return f ? (s8){f->data ? f->data : (u8 *)"", f->len} : (s8){0};
}

// Forget all files, input, and output, and inject no faults.
static void mfs_reset(os *ctx)
{
    for (i32 i = 0; i < MFS_FILES; i++) {
        ctx->files[i].live  = 0;
        ctx->files[i].opens = 0;
    }
    for (i32 i = 0; i < MFS_FDS; i++) {
        ctx->fds[i].open = 0;
    }
    ctx->nfiles      = 0;
    ctx->top         = (mfile){0};
    ctx->top.type    = FT_DIR;
    ctx->top.perm    = 0755;
    ctx->top.live    = 1;
    ctx->now         = 1900000000;
    ctx->in          = 0;
    ctx->inlen       = ctx->inoff = 0;
    ctx->stdinfile   = 0;
    ctx->tty[0] = ctx->tty[1] = ctx->tty[2] = 0;
    ctx->windows     = 0;
    ctx->umask       = 022;
    ctx->dest        = S("");
    ctx->archive     = "a.zip";
    ctx->when        = 0;
    ctx->shrinkto    = -1;
    ctx->failreadat  = -1;
    ctx->failwriteat = -1;
    ctx->failmkdir   = 0;
    ctx->failcreate  = 0;
    ctx->failkeep    = 0;
    ctx->failattrs   = 0;
    ctx->failsymlink = 0;
}

static os *unzipos_new(iz cap)
{
    os *ctx = calloc(1, sizeof(*ctx));
    CHECK(ctx);
    ctx->mem = malloc((uz)cap);
    CHECK(ctx->mem);
    ctx->cap = cap;
    ctx->lo  = ctx->mem;
    ctx->hi  = ctx->mem + cap;
    POISON(ctx->mem, cap);
    mfs_reset(ctx);
    return ctx;
}

// Run unzip with arguments, its environment's UNZIP (or null), in fresh
// memory, returning its exit status. As at a real exit, files it
// created and did not keep are then gone; *open, if given, tells how
// many descriptors it left open had it returned (not exited).
static i32 unzipos_run(os *ctx, char **argv, i32 argc, char *env, i32 *open)
{
    POISON(ctx->mem, ctx->lo-ctx->mem);
    POISON(ctx->hi, ctx->mem+ctx->cap-ctx->hi);
    ctx->lo = ctx->mem;
    ctx->hi = ctx->mem + ctx->cap;
    ctx->changed = ctx->faulted = ctx->renamed = ctx->armed = 0;
    ctx->exited  = 0;
    ctx->error   = 0;
    ctx->inoff   = 0;
    ctx->written = 0;
    ctx->nmkdir  = ctx->ncreate = ctx->nkeep = ctx->nchanges = 0;
    for (i32 i = 0; i < 3; i++) {
        ctx->out[i].len = 0;
    }

    s8 args[32];
    CHECK(argc <= countof(args));
    for (i32 i = 0; i < argc; i++) {
        args[i] = cstrs8(argv[i]);
    }
    unzipconfig conf = {0};
    conf.perm     = (arena){ctx->lo, ctx->lo, ctx, 0};
    conf.scratch  = (arena){ctx->hi, ctx->hi, ctx, 1};
    conf.args     = args;
    conf.nargs    = argc;
    conf.windows  = ctx->windows;
    conf.unzipenv = env ? cstrs8(env) : (s8){0};

    i32 status = 0;
    if (setjmp(ctx->jmp)) {
        status = ctx->status;
    } else {
        status = unzip_main(&conf);
    }
    i32 left = 0;
    for (i32 fd = 3; fd < MFS_FDS; fd++) {
        if (ctx->fds[fd].open) {
            left++;
            os_close(ctx, fd);
        }
    }
    if (open) {
        *open = ctx->exited ? 0 : left;
    }
    return status;
}

// What unzip wrote to standard output (1), error (2), or both (3).
static s8 unzipos_output(os *ctx, i32 fd)
{
    mbuf *b = ctx->out + fd - 1;
    return (s8){b->s ? b->s : (u8 *)"", b->len};
}

// A copy of the file system, to compare with later.
typedef struct {
    mfile *files;
    i32    n;
} msnap;

[[maybe_unused]] static msnap mfs_snapshot(os *ctx)
{
    msnap r = {calloc(MFS_FILES, sizeof(mfile)), 0};
    CHECK(r.files);
    for (i32 i = 0; i < ctx->nfiles; i++) {
        mfile *f = ctx->files + i;
        if (f->live) {
            mfile *c = r.files + r.n++;
            *c = *f;
            c->name.s = malloc((uz)f->name.len + 1);
            c->data   = malloc((uz)f->len + 1);
            CHECK(c->name.s && c->data);
            bytecopy(c->name.s, f->name.s, f->name.len);
            if (f->len) {
                bytecopy(c->data, f->data, f->len);
            }
        }
    }
    return r;
}

[[maybe_unused]] static void mfs_snapfree(msnap s)
{
    for (i32 i = 0; i < s.n; i++) {
        free(s.files[i].name.s);
        free(s.files[i].data);
    }
    free(s.files);
}

// Whether a canonical name is the directory dir (without a '/') or
// within it: the top ("") holds all, and a null string none.
static b32 mfs_within(os *ctx, s8 name, s8 dir)
{
    if (!dir.s) {
        return 0;
    } else if (!dir.len) {
        return 1;
    } else if (name.len < dir.len) {
        return 0;
    }
    return mfs_eq(ctx, (s8){name.s, dir.len}, dir) &&
           (name.len==dir.len || name.s[dir.len]=='/');
}

// Whether every file but those within dir (a canonical name) is as it
// was, by name, type, contents, attributes, and identity, and none has
// been added. (The top, which is no file, cannot change.)
[[maybe_unused]] static b32 mfs_unchanged(os *ctx, msnap s, s8 dir)
{
    for (i32 i = 0; i < s.n; i++) {
        mfile *o = s.files + i;
        if (mfs_within(ctx, o->name, dir)) {
            continue;
        }
        mfile *f = mfs_lookup(ctx, o->name);
        if (!f || f->type!=o->type || f->ino!=o->ino || f->len!=o->len ||
            (f->len && memcmp(f->data, o->data, (uz)f->len)) ||
            f->perm!=o->perm || f->dosattr!=o->dosattr ||
            f->uid!=o->uid || f->gid!=o->gid || f->mtime!=o->mtime ||
            f->atime!=o->atime) {
            fprintf(stderr, "changed: %.*s\n", (int)o->name.len, o->name.s);
            return 0;
        }
    }
    for (i32 i = 0; i < ctx->nfiles; i++) {
        mfile *f = ctx->files + i;
        if (f->live && !mfs_within(ctx, f->name, dir) && f->ino>0) {
            b32 found = 0;
            for (i32 k = 0; k<s.n && !found; k++) {
                found = s.files[k].ino == f->ino;
            }
            if (!found) {
                fprintf(stderr, "added: %.*s\n", (int)f->name.len, f->name.s);
                return 0;
            }
        }
    }
    return 1;
}

static void unzipos_free(os *ctx)
{
    for (i32 i = 0; i < MFS_FILES; i++) {
        free(ctx->files[i].name.s);
        free(ctx->files[i].data);
    }
    for (i32 i = 0; i < 3; i++) {
        free(ctx->out[i].s);
    }
    UNPOISON(ctx->mem, ctx->cap);
    free(ctx->mem);
    free(ctx);
}
