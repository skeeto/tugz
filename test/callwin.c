// Tests of the caller interfaces of zip and unzip (zip_call, unzip_call)
// on Windows itself, run as the GUI runs them (os_call), both programs in
// one translation unit: names in Unicode, a path past MAX_PATH, renames
// and deletions, a read-only archive, and the work stopped by the host's
// meter at each of its calls in turn, which must leave the archive as it
// was, no temporary file, nothing extracted in part, and no handle open.
// On success prints "all call tests pass". A failure traps.
// $ cc -g3 -o tests-callwin test/callwin.c
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/inflate.c"
#include "../src/deflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/wild.c"
#include "../src/dir.c"
#include "../src/zipin.c"
#include "../src/unzip.c"
#include "../src/zipinfo.c"
#define os_oom zip_os_oom
#include "../src/zipcli.c"
#undef os_oom
#define os_oom unzip_os_oom
#include "../src/unzipcli.c"
#undef os_oom

#include "../platform/windows.c"
#include "../platform/zipfs_windows.c"
#include "../platform/zipos_windows.c"
#include "../platform/unzipos_windows.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

W32(iptr) GetCurrentProcess(void);
W32(u32)  GetCurrentProcessId(void);
W32(b32)  GetProcessHandleCount(iptr, u32 *);
W32(u32)  GetTempPathW(u32, c16 *);
W32(b32)  RemoveDirectoryW(c16 *);
W32(b32)  SetFileAttributesW(c16 *, u32);

#define TEST(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
            fflush(stderr); \
            __builtin_trap(); \
        } \
    } while (0)

enum { EXIT_STOP = 100 };  // the status of work the meter stopped

// The host, whose os is first, so that its meter finds it
typedef struct {
    os    ctx;
    byte *lo;      // its memory, reserved once, used again by each run
    byte *hi;
    i32   calls;   // to the meter, in this run
    i32   stopat;  // the call that stops the work, or 0
    b32   zip;     // zip is running, rather than unzip (os_oom)
} host;

static void os_oom(os *ctx)
{
    if (((host *)ctx)->zip) {
        zip_os_oom(ctx);
    } else {
        unzip_os_oom(ctx);
    }
    os_exit(ctx, 4);
}

static void host_meter(os *ctx, s8 path, iz len)
{
    (void)path;
    (void)len;
    host *h = (host *)ctx;
    if (++h->calls == h->stopat) {
        os_exit(ctx, EXIT_STOP);
    }
}

static i32 run_zip(os *ctx, void *arg)
{
    zipconfig conf = {0};
    conf.perm    = (arena){ctx->lo, ctx->lo, ctx, 0};
    conf.scratch = (arena){ctx->hi, ctx->hi, ctx, 1};
    conf.windows = 1;
    return zip_call(&conf, arg);
}

static i32 run_unzip(os *ctx, void *arg)
{
    unzipconfig conf = {0};
    conf.perm    = (arena){ctx->lo, ctx->lo, ctx, 0};
    conf.scratch = (arena){ctx->hi, ctx->hi, ctx, 1};
    conf.windows = 1;
    return unzip_call(&conf, arg);
}

static u32 handles(void)
{
    u32 n = 0;
    TEST(GetProcessHandleCount(GetCurrentProcess(), &n));
    return n;
}

// Run zip (or unzip) as the GUI does, in the host's memory, from its
// start, stopped at the meter's stopat'th call, if not zero, checking
// that it leaves no handle open.
static i32 call(host *h, b32 zip, void *c, i32 stopat)
{
    h->ctx.lo   = h->lo;
    h->ctx.hi   = h->hi;
    h->calls    = 0;
    h->stopat   = stopat;
    h->zip      = zip;
    u32 before  = handles();
    i32 r = os_call(&h->ctx, zip ? run_zip : run_unzip, c);
    h->stopat = 0;  // not the tests' own calls
    oom_out   = 0;
    TEST(!h->ctx.escape && !h->ctx.guard);
    for (i32 fd = 3; fd < MAX_HANDLES; fd++) {
        TEST(!h->ctx.handles[fd]);
    }
    TEST(handles() == before);
    return r;
}

static s8 join(s8 a, char const *b)
{
    iz n = (iz)strlen(b);
    s8 r = {malloc((uz)(a.len + n)), a.len + n};
    TEST(r.s);
    memcpy(r.s, a.s, (uz)a.len);
    memcpy(r.s+a.len, b, (uz)n);
    return r;
}

// Scratch for the tests' own calls, committed as they need it.
static arena scratch(host *h)
{
    return (arena){h->ctx.hi, h->ctx.hi, &h->ctx, 1};
}

// A directory's entry's path.
static s8 kid(s8 dir, s8 name)
{
    s8 r = {malloc((uz)(dir.len + 1 + name.len)), dir.len + 1 + name.len};
    TEST(r.s);
    memcpy(r.s, dir.s, (uz)dir.len);
    r.s[dir.len] = '/';
    memcpy(r.s+dir.len+1, name.s, (uz)name.len);
    return r;
}

static void put(host *h, s8 path, s8 data)
{
    i32 fd = os_open(&h->ctx, path, OS_CREATE, scratch(h));
    TEST(fd >= 0);
    TEST(os_write(&h->ctx, fd, data.s, data.len));
    TEST(os_keep(&h->ctx, fd));
    TEST(os_close(&h->ctx, fd));
}

// A file's contents, or a null string if it is not there.
static s8 get(host *h, s8 path)
{
    i32 fd = os_open(&h->ctx, path, OS_READ, scratch(h));
    if (fd < 0) {
        return (s8){0};
    }
    s8 r = {0};
    for (iz cap = 0;;) {
        if (r.len == cap) {
            cap  = cap ? 2*cap : 1<<12;
            r.s  = realloc(r.s, (uz)cap);
            TEST(r.s);
        }
        iz n = os_read(&h->ctx, fd, r.s+r.len, cap-r.len);
        TEST(n >= 0);
        if (!n) {
            break;
        }
        r.len += n;
    }
    os_close(&h->ctx, fd);
    return r;
}

static b32 same(s8 a, s8 b)
{
    return a.s && b.s && a.len==b.len && !memcmp(a.s, b.s, (uz)a.len);
}

static b32 equals(s8 a, char const *z)
{
    return same(a, (s8){(u8 *)z, (iz)strlen(z)});
}

// Whether a directory holds a temporary file of zip's (zi*).
static b32 has_temp(host *h, s8 dir)
{
    iz         n    = 0;
    arena      a    = scratch(h);
    os_dirent *list = os_listdir(&h->ctx, dir, 1, &n, &a);
    for (iz i = 0; i < n; i++) {
        s8 name = list[i].name;
        if (name.len>=2 && name.s[0]=='z' && name.s[1]=='i') {
            return 1;
        }
    }
    return 0;
}

// Remove a tree, its files read-only or not.
static void rmtree(host *h, s8 path)
{
    iz         n    = 0;
    arena      a    = scratch(h);
    os_dirent *list = os_listdir(&h->ctx, path, 1, &n, &a);
    for (iz i = 0; i < n; i++) {
        s8      k    = kid(path, list[i].name);
        os_info info = {0};
        if (os_stat(&h->ctx, k, 0, &info, scratch(h)) &&
            info.type==FT_DIR) {
            rmtree(h, k);
        } else {
            os_remove(&h->ctx, k, scratch(h));
        }
        free(k.s);
    }
    arena t = scratch(h);
    RemoveDirectoryW(winpath(&t, path));
}

// An archive's entries, as unzip lists them (unzip_call's visit).
typedef struct {
    i64 at[16];
    s8  stored[16];
    s8  name[16];
    i32 n;
} listing;

static s8 dup8(s8 s)
{
    s8 r = {malloc((uz)s.len + 1), s.len};
    TEST(r.s);
    memcpy(r.s, s.s, (uz)s.len);
    return r;
}

static void visit_hook(void *data, unzipvisit *v)
{
    listing *l = data;
    if (v->e) {
        TEST(l->n < countof(l->at));
        l->at[l->n]     = v->at;
        l->stored[l->n] = dup8(v->e->name);
        l->name[l->n]   = dup8(v->name);
        l->n++;
    }
}

static listing list(host *h, s8 archive)
{
    listing   l = {0};
    unzipcall c = {0};
    c.archive = archive;
    c.mode    = CALL_LIST;
    c.visit   = visit_hook;
    c.data    = &l;
    TEST(call(h, 0, &c, 0) == 0);
    return l;
}

static void free_list(listing *l)
{
    for (i32 i = 0; i < l->n; i++) {
        free(l->stored[i].s);
        free(l->name[i].s);
    }
}

int main(void)
{
    host *h = calloc(1, sizeof(*h));
    TEST(h);
    os_init(&h->ctx);
    reserve(&h->ctx);
    h->lo = h->ctx.lo;
    h->hi = h->ctx.hi;
    h->ctx.meter = host_meter;

    // A fresh directory in TEMP
    c16 tmp[512];
    u32 len = GetTempPathW(countof(tmp), tmp);
    TEST(len && len<countof(tmp)-40);
    char tail[32];
    snprintf(tail, sizeof(tail), "tugz-callwin-%u", (unsigned)GetCurrentProcessId());
    for (i32 i = 0; tail[i]; i++) {
        tmp[len++] = (c16)tail[i];
    }
    tmp[len] = 0;
    arena a = scratch(h);
    s8 w = dup8(towtf8(&a, tmp));
    rmtree(h, w);
    TEST(os_mkdir(&h->ctx, w, scratch(h)));

    // Files to add, with Unicode names, one large enough to take many
    // reads, and one past MAX_PATH
    s8 src = join(w, "/src");
    TEST(os_mkdir(&h->ctx, src, scratch(h)));
    s8 auml  = join(src, "/\xc3\xa4.txt");                 // ä.txt
    s8 japan = join(src, "/\xe6\x97\xa5\xe6\x9c\xac");     // 日本
    s8 big   = join(japan, "/x.bin");
    put(h, auml, (s8){(u8 *)"hello\n", 6});
    TEST(os_mkdir(&h->ctx, japan, scratch(h)));
    s8 bigdata = {malloc(1<<20), 1<<20};
    TEST(bigdata.s);
    for (iz i = 0; i < bigdata.len; i++) {
        bigdata.s[i] = (u8)(i*7 ^ i>>9);
    }
    put(h, big, bigdata);
    s8 deep = join(src, "");
    for (i32 i = 0; i < 30; i++) {
        s8 next = join(deep, "/abcdefghij");
        free(deep.s);
        deep = next;
        TEST(os_mkdir(&h->ctx, deep, scratch(h)));
    }
    TEST(deep.len > 300);
    s8 deepfile = join(deep, "/f.txt");
    put(h, deepfile, (s8){(u8 *)"deep\n", 5});

    // Added under names of the caller's: a new archive
    s8 archive = join(w, "/a.zip");
    zipsource add[] = {
        {auml,     S("docs/\xc3\xa4.txt")},
        {japan,    S("docs/\xe6\x97\xa5\xe6\x9c\xac")},
        {deepfile, S("docs/deep.txt")},
    };
    zipcall z = {0};
    z.archive  = archive;
    z.sources  = add;
    z.nsources = countof(add);
    z.level    = 6;
    TEST(call(h, 1, &z, 0) == 0);
    TEST(z.total == 6 + (1<<20) + 5);
    listing l = list(h, archive);
    TEST(l.n == 4);
    TEST(equals(l.name[0], "docs/\xc3\xa4.txt"));
    TEST(equals(l.name[1], "docs/\xe6\x97\xa5\xe6\x9c\xac/"));
    TEST(equals(l.name[2], "docs/\xe6\x97\xa5\xe6\x9c\xac/x.bin"));
    TEST(equals(l.name[3], "docs/deep.txt"));

    // Renamed by ordinal, under Unicode names, then extracted under the
    // caller's names, one past MAX_PATH
    zipchange ren[] = {{0, l.stored[0], S("docs/\xc3\xb6.txt")}};  // ö.txt
    z.sources  = 0;
    z.nsources = 0;
    z.changes  = ren;
    z.nchanges = 1;
    TEST(call(h, 1, &z, 0) == 0);
    free_list(&l);
    l = list(h, archive);
    TEST(l.n==4 && equals(l.name[0], "docs/\xc3\xb6.txt"));
    s8 out = join(w, "/out");
    unzippick picks[] = {
        {l.at[0], l.stored[0], S("\xc3\xb6.txt"), 0, 0},
        {l.at[2], l.stored[2], S("x.bin"), 0, 0},
        {l.at[3], l.stored[3], {0}, 0, 0},
    };
    s8 deepout = join(out, "");
    for (i32 i = 0; i < 30; i++) {
        s8 next = join(deepout, "/abcdefghij");
        free(deepout.s);
        deepout = next;
    }
    s8 deepas = {deepout.s+out.len+1, deepout.len-out.len-1};
    s8 deepname = join(deepas, "/deep.txt");
    picks[2].as = deepname;
    unzipcall u = {0};
    u.archive = archive;
    u.mode    = CALL_EXTRACT;
    u.exdir   = out;
    u.picks   = picks;
    u.npicks  = countof(picks);
    TEST(call(h, 0, &u, 0) == 0);
    TEST(picks[0].done && picks[1].done && picks[2].done);
    TEST(equals(get(h, join(out, "/\xc3\xb6.txt")), "hello\n"));
    TEST(same(get(h, join(out, "/x.bin")), bigdata));
    TEST(equals(get(h, join(deepout, "/deep.txt")), "deep\n"));

    // Stopped at each of the meter's calls in turn, changing the archive
    // (adding to it, or deleting from it), or extracting, leaves it, and
    // what was extracted, whole, with no temporary file, until done
    s8 before = get(h, archive);
    zipchange del[] = {{2, l.stored[2], {0}}};
    zipsource more[] = {{big, S("docs/big.bin")}};
    for (i32 kind = 0; kind < 3; kind++) {
        i32 stops = 0;
        for (i32 at = 1;; at++) {
            i32 r = 0;
            if (kind == 2) {
                rmtree(h, out);
                for (iz i = 0; i < u.npicks; i++) {
                    picks[i].met = picks[i].done = 0;
                }
                r = call(h, 0, &u, at);
            } else {
                zipcall c = {0};
                c.archive  = archive;
                c.level    = 6;
                c.sources  = kind ? 0 : more;
                c.nsources = kind ? 0 : 1;
                c.changes  = kind ? del : 0;
                c.nchanges = kind ? 1 : 0;
                r = call(h, 1, &c, at);
            }
            if (r != EXIT_STOP) {
                TEST(r == 0);
                break;
            }
            stops++;
            TEST(same(get(h, archive), before));
            TEST(!has_temp(h, w));
            s8 x = get(h, join(out, "/x.bin"));
            TEST(!x.s || same(x, bigdata));
        }
        TEST(stops > 3);
        if (kind < 2) {
            // Done: changed, then put back as it was for the next kind
            TEST(!same(get(h, archive), before));
            os_remove(&h->ctx, archive, scratch(h));
            put(h, archive, before);
        }
    }

    // A read-only archive is not changed
    arena t = scratch(h);
    TEST(SetFileAttributesW(winpath(&t, archive), FILE_ATTRIBUTE_READONLY));
    zipcall c = {0};
    c.archive  = archive;
    c.changes  = del;
    c.nchanges = 1;
    TEST(call(h, 1, &c, 0) == ZE_CREAT);
    TEST(same(get(h, archive), before));
    t = scratch(h);
    SetFileAttributesW(winpath(&t, archive), FILE_ATTRIBUTE_NORMAL);

    rmtree(h, w);
    puts("all call tests pass");
    return 0;
}
