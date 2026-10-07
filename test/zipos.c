// In-memory platform layer for the zip program, shared by its unit tests
// (test/zipclitests.c) and its libFuzzer harness (test/fuzz_zip.c)
// Runs zip_main (src/zipcli.c) over a table of files, with faults to
// inject: an archive that shrinks, or whose reads fail, and writes that
// fail. Memory is one reservation committed as the platform layers
// commit it, but exactly what is asked rather than a megabyte at a time,
// and once the temporary file exists, committing more traps: zip claims
// whatever grows with its work before any output, so that running out of
// memory cannot strike once output has started (except, here, for an
// error's reason after an injected fault). Under AddressSanitizer, the
// uncommitted part of the reservation is poisoned.
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/deflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/wild.c"
#include "../src/dir.c"
#include "../src/zipcli.c"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define ZIPOS_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(ZIPOS_ASAN)
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

enum { MAX_FILES = 32, MAX_FDS = 16, MAX_OUTPUT = 1<<16, MAX_SIZE = 1<<26 };

typedef struct {
    s8  name;
    u8 *data;    // contents, or a link's target
    iz  len;
    iz  cap;
    i32 type;    // FT_FILE, FT_DIR, FT_LINK, or FT_OTHER
    i64 mtime;
    b32 live;
} mfile;

// When faults in the archive begin
enum {
    FAULT_OPEN = 1,  // as zip opens it, before reading it
    FAULT_TEMP,      // as zip creates its temporary file, before copying
};

struct os {
    jmp_buf jmp;
    i32     status;  // given to os_exit
    mfile   files[MAX_FILES];
    struct {
        i32 file;
        iz  off;
        b32 open;
        b32 created;  // discarded on close unless committed
    } fds[MAX_FDS];
    u8      out[2][MAX_OUTPUT];  // standard output and error, as written
    iz      outlen[2];
    char   *error;    // why the last failing call failed
    b32     missing;  // the last failure found nothing there

    byte   *mem;      // the reservation
    iz      cap;
    byte   *lo;       // perm committed below, scratch from hi up
    byte   *hi;
    b32     writing;  // the temporary file exists
    b32     faulted;  // a fault was injected

    // Faults: from when, the archive (the file so named) shrinks to a
    // length, and reads of it past an offset fail; writes of the
    // temporary file past an offset fail (-1 for none of each)
    char   *archive;
    i32     when;
    i64     shrinkto;
    i64     failreadat;
    i64     failwriteat;
    b32     armed;  // the archive's faults have begun
};

static s8 cstrs8(char const *z)
{
    return (s8){(u8 *)z, (iz)strlen(z)};
}

static mfile *mfs_find(os *ctx, s8 name)
{
    for (i32 i = 0; i < MAX_FILES; i++) {
        mfile *f = ctx->files + i;
        if (f->live && s8equals(f->name, name)) {
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

static mfile *mfs_create(os *ctx, s8 name, i32 type)
{
    CHECK(!mfs_find(ctx, name));
    for (i32 i = 0; i < MAX_FILES; i++) {
        mfile *f = ctx->files + i;
        if (!f->live) {
            free(f->name.s);
            f->name.s   = malloc((uz)name.len + 1);
            f->name.len = name.len;
            CHECK(f->name.s);
            bytecopy(f->name.s, name.s, name.len);
            f->len   = 0;
            f->type  = type;
            f->mtime = 0;
            f->live  = 1;
            return f;
        }
    }
    CHECK(0);
    return 0;
}

// Add a file, a directory (type FT_DIR, without data), a link (FT_LINK,
// its target as its data), or a special file (FT_OTHER).
static void mfs_put(os *ctx, char const *name, i32 type, void const *p,
                    iz len, i64 mtime)
{
    mfile *f = mfs_create(ctx, cstrs8(name), type);
    mfs_setlen(f, len);
    if (len) {
        bytecopy(f->data, p, len);
    }
    f->mtime = mtime;
}

// A file's contents, or a null string if there is none.
static s8 mfs_get(os *ctx, char const *name)
{
    mfile *f = mfs_find(ctx, cstrs8(name));
    return f ? (s8){f->data ? f->data : (u8 *)"", f->len} : (s8){0};
}

// Forget all files and output, and inject no faults.
static void mfs_reset(os *ctx)
{
    for (i32 i = 0; i < MAX_FILES; i++) {
        ctx->files[i].live = 0;
    }
    for (i32 i = 0; i < MAX_FDS; i++) {
        ctx->fds[i].open = 0;
    }
    ctx->outlen[0] = ctx->outlen[1] = 0;
    ctx->archive     = "a.zip";
    ctx->when        = 0;
    ctx->shrinkto    = -1;
    ctx->failreadat  = -1;
    ctx->failwriteat = -1;
}

static os *zipos_new(iz cap)
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

// Follow links, each target relative to its link's directory, to a file,
// or null if there is none (dangling, or a loop). Allocates nothing, as
// zip may not, once writing, but for what it asks.
static mfile *mfs_follow(os *ctx, mfile *f)
{
    for (i32 i = 0; f && f->type==FT_LINK; i++) {
        u8 buf[256];
        iz cut = f->name.len;
        for (; cut>0 && f->name.s[cut-1]!='/'; cut--) {}
        if (i==8 || cut+f->len>countof(buf)) {
            return 0;
        }
        bytecopy(buf, f->name.s, cut);
        bytecopy(buf+cut, f->data, f->len);
        f = mfs_find(ctx, (s8){buf, cut+f->len});
    }
    return f;
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

static b32 mfs_isarchive(os *ctx, mfile *f)
{
    return s8equals(f->name, cstrs8(ctx->archive));
}

static void mfs_arm(os *ctx, i32 when)
{
    mfile *f = mfs_find(ctx, cstrs8(ctx->archive));
    if (f && ctx->when==when && !ctx->armed) {
        ctx->armed = 1;
        if (ctx->shrinkto >= 0) {
            f->len = MIN(f->len, (iz)ctx->shrinkto);
            ctx->faulted = 1;
        }
    }
}

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    (void)scratch;
    i32 fd = 3;
    for (; fd<MAX_FDS && ctx->fds[fd].open; fd++) {}
    CHECK(fd < MAX_FDS);
    CHECK(!(mode & OS_FORCE));  // zip never replaces a file by opening it

    mfile *f       = mfs_find(ctx, path);
    b32    created = 0;
    if (mode & OS_CREATE) {
        if (f) {
            mfs_fail(ctx, "File exists", 0);
            return OS_EEXIST;
        }
        f = mfs_create(ctx, path, FT_FILE);
        f->mtime = 1700000000;
        created  = 1;
        ctx->writing = 1;  // zip creates only its temporary file
        mfs_arm(ctx, FAULT_TEMP);
    } else {
        if (f && f->type==FT_LINK && (mode & OS_NOFOLLOW)) {
            return OS_ESYMLINK;
        }
        f = mfs_follow(ctx, f);
        if (!f) {
            mfs_fail(ctx, "No such file or directory", 1);
            return OS_ERR;
        } else if (f->type == FT_DIR) {
            return OS_EISDIR;
        } else if (f->type==FT_OTHER && (mode & OS_REGULAR)) {
            return OS_ENOTREG;
        }
        if (mfs_isarchive(ctx, f)) {
            mfs_arm(ctx, FAULT_OPEN);
        }
    }
    ctx->fds[fd].file    = (i32)(f - ctx->files);
    ctx->fds[fd].off     = 0;
    ctx->fds[fd].open    = 1;
    ctx->fds[fd].created = created;
    return fd;
}

static mfile *mfs_fd(os *ctx, i32 fd)
{
    CHECK(fd>2 && fd<MAX_FDS && ctx->fds[fd].open);
    return ctx->files + ctx->fds[fd].file;
}

static b32 os_close(os *ctx, i32 fd)
{
    mfile *f = mfs_fd(ctx, fd);
    ctx->fds[fd].open = 0;
    if (ctx->fds[fd].created) {
        f->live = 0;  // discarded, as never committed
    }
    return 1;
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    CHECK(cap > 0);
    if (fd == 0) {
        return 0;  // empty standard input
    }
    mfile *f = mfs_fd(ctx, fd);
    iz     n = MAX(MIN(cap, f->len - ctx->fds[fd].off), 0);
    n = MIN(n, 1000);  // short reads
    if (n) {
        bytecopy(buf, f->data+ctx->fds[fd].off, n);
    }
    ctx->fds[fd].off += n;
    return n;
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    CHECK(fd==1 || fd==2);
    iz *n    = ctx->outlen + fd - 1;
    iz  take = MIN(len, MAX_OUTPUT - *n);
    bytecopy(ctx->out[fd-1] + *n, buf, take);
    *n += take;
    return 1;
}

[[maybe_unused]] static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)ctx; (void)path; (void)scratch;
    CHECK(0);  // unused by zip
    return 0;
}

[[maybe_unused]] static b32 os_keep(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    CHECK(0);  // unused by zip
    return 0;
}

static void os_exit(os *ctx, i32 status)
{
    ctx->status = status;
    longjmp(ctx->jmp, 1);
}

static void mfs_info(os *ctx, mfile *f, os_info *info)
{
    *info = (os_info){0};
    info->type   = f->type;
    info->size   = f->type==FT_LINK ? f->len : f->type==FT_DIR ? 0 : f->len;
    info->mtime  = f->mtime;
    info->atime  = f->mtime;
    info->mode   = f->type==FT_DIR  ? 0040755 :
                   f->type==FT_LINK ? 0120777 :
                   f->type==FT_FILE ? 0100644 : 0010644;
    info->attr   = f->type==FT_DIR ? 0x10 : 0x20;
    info->uid    = 1000;
    info->gid    = 1000;
    info->dev    = 1;
    info->ino[0] = (u64)(f - ctx->files) + 1;
}

static b32 os_stat(os *ctx, s8 path, b32 follow, os_info *info,
                   arena scratch)
{
    (void)scratch;
    mfile *f = mfs_find(ctx, path);
    f = follow ? mfs_follow(ctx, f) : f;
    if (!f) {
        mfs_fail(ctx, "No such file or directory", 1);
        return 0;
    }
    mfs_info(ctx, f, info);
    return 1;
}

static b32 os_missing(os *ctx)
{
    return ctx->missing;
}

static b32 os_fstat(os *ctx, i32 fd, os_info *info)
{
    if (fd == 0) {
        *info = (os_info){0};
        info->type = FT_OTHER;  // a pipe
        return 1;
    }
    mfs_info(ctx, mfs_fd(ctx, fd), info);
    return 1;
}

static s8 os_readlink(os *ctx, s8 path, arena *a)
{
    mfile *f = mfs_find(ctx, path);
    if (!f || f->type!=FT_LINK) {
        mfs_fail(ctx, f ? "Invalid argument" : "No such file or directory",
                  !f);
        return (s8){0};
    }
    s8 r = {newstr(a, f->len), f->len};
    if (r.len) {
        bytecopy(r.s, f->data, f->len);
    }
    return r;
}

static i32 os_readat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    mfile *f = mfs_fd(ctx, fd);
    CHECK(len>=0 && off>=0);
    if (ctx->armed && ctx->failreadat>=0 && off+len>ctx->failreadat) {
        mfs_fault(ctx, "Input/output error");
        return -1;
    }
    iz n = (iz)MIN(len, MAX(f->len-off, 0));
    if (n) {
        bytecopy(buf, f->data+off, n);
    }
    return n==len ? 1 : 0;
}

static b32 os_writeat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    mfile *f = mfs_fd(ctx, fd);
    CHECK(ctx->fds[fd].created && len>=0 && off>=0);
    if (ctx->failwriteat>=0 && off+len>ctx->failwriteat) {
        mfs_fault(ctx, "No space left on device");
        return 0;
    } else if (off+len > MAX_SIZE) {
        mfs_fault(ctx, "File too large");  // as entries may share data
        return 0;
    }
    if (off+len > f->len) {
        mfs_setlen(f, (iz)(off+len));
    }
    bytecopy(f->data+off, buf, len);
    return 1;
}

static b32 os_truncate(os *ctx, i32 fd, i64 len)
{
    mfile *f = mfs_fd(ctx, fd);
    CHECK(ctx->fds[fd].created);
    mfs_setlen(f, (iz)len);
    return 1;
}

// The archive is never a link here.
static s8 os_resolve(os *ctx, s8 path, arena *perm, arena scratch)
{
    (void)perm; (void)scratch;
    mfile *f = mfs_find(ctx, path);
    CHECK(!f || f->type!=FT_LINK);
    return path;
}

static b32 os_writable(os *ctx, s8 path, arena scratch)
{
    (void)ctx; (void)path; (void)scratch;
    return 1;
}

static i32 os_commit(os *ctx, i32 fd, s8 temp, s8 path, b32 replace,
                     arena scratch)
{
    (void)scratch;
    mfile *f = mfs_fd(ctx, fd);
    CHECK(ctx->fds[fd].created && s8equals(f->name, temp));
    mfile *old = mfs_find(ctx, path);
    ctx->fds[fd].open = 0;
    if (old && !replace) {
        f->live = 0;
        mfs_fail(ctx, "File exists", 0);
        return COMMIT_EREPLACE;
    } else if (old) {
        old->live = 0;
    }
    free(f->name.s);
    f->name.s   = malloc((uz)path.len + 1);
    f->name.len = path.len;
    CHECK(f->name.s);
    bytecopy(f->name.s, path.s, path.len);
    return COMMIT_OK;
}

static void os_localtime(os *ctx, i64 t, i32 tm[6])
{
    (void)ctx;
    zip_gmtime(t, tm);
}

static b32 os_isatty(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    return 0;
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

static s8 os_fullpath(os *ctx, s8 path, arena *perm, arena scratch)
{
    (void)ctx; (void)path; (void)perm; (void)scratch;
    return (s8){0};  // every file has an ID
}

static s8 os_upcase(os *ctx, s8 name, arena *a)
{
    (void)ctx; (void)a;
    return name;
}

// The entries directly within a directory, "." for the top, left for
// os_stat to examine.
static os_dirent *os_listdir(os *ctx, s8 path, b32 all, iz *count,
                             arena *a)
{
    (void)all;
    b32    top = s8equals(path, S("."));
    mfile *dir = top ? 0 : mfs_follow(ctx, mfs_find(ctx, path));
    if (!top && (!dir || dir->type!=FT_DIR)) {
        mfs_fail(ctx, "Not a directory", 0);
        return 0;
    }
    iz pre = top ? 0 : path.len + 1;
    s8 names[MAX_FILES];
    iz n = 0;
    for (i32 i = 0; i < MAX_FILES; i++) {
        mfile *f = ctx->files + i;
        s8     s = f->name;
        b32    in = f->live && s.len>pre &&
                    (top || (!memcmp(s.s, path.s, (uz)path.len) &&
                             s.s[path.len]=='/'));
        for (iz j = pre; in && j < s.len; j++) {
            in = s.s[j] != '/';
        }
        if (in) {
            names[n++] = (s8){s.s+pre, s.len-pre};
        }
    }
    os_dirent *list = new(a, n, os_dirent);
    for (iz i = 0; i < n; i++) {
        list[i].name.s   = newstr(a, names[i].len);
        list[i].name.len = names[i].len;
        bytecopy(list[i].name.s, names[i].s, names[i].len);
    }
    *count = n;
    return list;
}

// As the platform layers commit memory, but exactly what is wanted, and
// nothing once the temporary file exists (unless after a fault). Scratch
// arenas, in which committing for one makes room for all, may still ask.
static void os_extend(os *ctx, arena *a, iz need)
{
    if (a->down) {
        a->beg = ctx->hi;
    } else if (a->end != ctx->lo) {
        os_oom(ctx);  // a fixed arena, such as a codec's
    }
    iz want = need - (a->end - a->beg);
    if (want > ctx->hi-ctx->lo) {
        os_oom(ctx);
    } else if (want > 0) {
        CHECK(!ctx->writing || ctx->faulted);
        byte *at = a->down ? ctx->hi-want : ctx->lo;
        UNPOISON(at, want);
        if (a->down) {
            ctx->hi = at;
            a->beg  = at;
        } else {
            ctx->lo = at + want;
            a->end  = ctx->lo;
        }
    }
}

// Run zip with arguments, as on POSIX or with Windows conventions, in
// fresh memory, returning its exit status. As at a real exit, files it
// created and did not commit are then gone.
static i32 zipos_run(os *ctx, char **argv, i32 argc, b32 windows)
{
    POISON(ctx->mem, ctx->lo-ctx->mem);
    POISON(ctx->hi, ctx->mem+ctx->cap-ctx->hi);
    ctx->lo = ctx->mem;
    ctx->hi = ctx->mem + ctx->cap;
    ctx->writing = ctx->faulted = ctx->armed = 0;
    ctx->error   = 0;
    ctx->outlen[0] = ctx->outlen[1] = 0;

    s8 args[16];
    CHECK(argc <= countof(args));
    for (i32 i = 0; i < argc; i++) {
        args[i] = cstrs8(argv[i]);
    }
    zipconfig conf = {0};
    conf.perm    = (arena){ctx->lo, ctx->lo, ctx, 0};
    conf.scratch = (arena){ctx->hi, ctx->hi, ctx, 1};
    conf.args    = args;
    conf.nargs   = argc;
    conf.windows = windows;

    i32 status = 0;
    if (setjmp(ctx->jmp)) {
        status = ctx->status;
    } else {
        status = zip_main(&conf);
    }
    for (i32 fd = 3; fd < MAX_FDS; fd++) {
        if (ctx->fds[fd].open) {
            os_close(ctx, fd);
        }
    }
    return status;
}

// What zip wrote to standard output (1) or error (2).
static s8 zipos_output(os *ctx, i32 fd)
{
    return (s8){ctx->out[fd-1], ctx->outlen[fd-1]};
}

static void zipos_free(os *ctx)
{
    for (i32 i = 0; i < MAX_FILES; i++) {
        free(ctx->files[i].name.s);
        free(ctx->files[i].data);
    }
    UNPOISON(ctx->mem, ctx->cap);
    free(ctx->mem);
    free(ctx);
}
