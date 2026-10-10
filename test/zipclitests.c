// Unit tests of the zip program (src/zipcli.c), run in memory
// (test/zipos.c): reading an existing archive through its window, out of
// order and across the window's edge; an archive that shrinks, or whose
// reads or writes fail; a file swapped for a link as zip reads it;
// messages held, paced on a terminal, and kept in order with standard
// error; and, in every run, memory claimed only before any output. On
// success prints "all zip program tests pass". A failure traps.
// $ cc -g3 -fsanitize=address,undefined -o tests-zipcli test/zipclitests.c
#include "zipos.c"

#define TEST CHECK

static b32 equals(s8 a, char const *z)
{
    return s8equals(a, cstrs8(z));
}

// Run zip with arguments, which must exit with a status, else its
// messages are shown.
static void expect(os *ctx, i32 status, char **argv, i32 argc)
{
    i32 got = zipos_run(ctx, argv, argc, 0);
    if (got != status) {
        s8 err = zipos_output(ctx, 2);
        fprintf(stderr, "zip exited %d, not %d:\n%.*s", got, status,
                (int)err.len, err.s);
    }
    TEST(got == status);
}
#define ZIP(ctx, status, ...) \
    expect(ctx, status, (char *[]){__VA_ARGS__}, \
           countof(((char *[]){__VA_ARGS__})))

// Whether no file is left whose name a temporary file's would be.
static b32 no_temp(os *ctx)
{
    for (i32 i = 0; i < MAX_FILES; i++) {
        mfile *f = ctx->files + i;
        if (f->live && f->name.len>=2 && !memcmp(f->name.s, "zi", 2)) {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    s8 name;
    s8 data;
} zspec;

// An archive of stored entries, its central directory in the order
// given, their local headers and data in file order (null for the
// same), as a malloc'd string.
static s8 build(zspec const *spec, i32 n, i32 const *order)
{
    iz cap = ZIP_END64_LEN + ZIP_LOC64_LEN + ZIP_END_LEN;
    for (i32 i = 0; i < n; i++) {
        cap += ZIP_LOCAL_LEN + ZIP_CENTRAL_LEN + 2*spec[i].name.len +
               spec[i].data.len;
    }
    TEST(n >= 0);
    u8     *buf = malloc((uz)cap);
    zentry *e   = calloc((uz)n, sizeof(*e));
    TEST(buf && e);
    u8 *p = buf;
    for (i32 k = 0; k < n; k++) {
        i32 i = order ? order[k] : k;
        e[i].name    = spec[i].name;
        e[i].made    = 0x031e;
        e[i].method  = ZIP_STORE;
        e[i].dostime = 0x57654321;
        e[i].crc     = crc32_update(0, spec[i].data.s, spec[i].data.len,
                                    &(i32){0});
        e[i].csize   = spec[i].data.len;
        e[i].usize   = spec[i].data.len;
        e[i].extattr = 0x81a40000;
        e[i].offset  = p - buf;
        p = zip_local(p, e+i);
        p = putbytes(p, spec[i].data);
    }
    i64 cdoff = p - buf;
    for (i32 i = 0; i < n; i++) {
        p = zip_central(p, e+i);
    }
    i64 cdsize = p - buf - cdoff;
    p = zip_end(p, n, cdsize, cdoff, (s8){0}, 0x031e);
    TEST(p-buf <= cap);
    free(e);
    return (s8){buf, p-buf};
}

// The entries of an archive, as src/zip.c parses them, with their data.
typedef struct {
    zentry *entries;
    s8     *data;
    iz      count;
} zlist;

static zlist entries(s8 z, arena *a)
{
    zlist r    = {0};
    zend  end  = {0};
    i32   got  = zip_find_end(z.s, z.len, z.len, &end);
    if (got==ZIP_OK && end.end64>=0) {
        got = zip_parse_end64(z.s+end.end64, &end);
    }
    TEST(got == ZIP_OK);
    r.count   = (iz)end.count;
    r.entries = zip_parse_central(z.s+end.cdoff, (iz)end.cdsize, end.count,
                                  end.cdoff, a);
    TEST(r.entries);
    r.data = new(a, r.count, s8);
    for (iz i = 0; i < r.count; i++) {
        zentry *e = r.entries + i;
        u8     *h = z.s + e->offset;
        iz      v = zip_local_varlen(h);
        TEST(v>=0 && e->offset+ZIP_LOCAL_LEN+v+e->csize<=end.cdoff);
        TEST(s8equals((s8){h+ZIP_LOCAL_LEN, (iz)get16(h+26)}, e->name));
        r.data[i] = (s8){h+ZIP_LOCAL_LEN+v, (iz)e->csize};
    }
    return r;
}

// Files to add: f, of text, d, and d/g, with an existing archive
static void put_files(os *ctx, s8 archive)
{
    u8 text[3000];
    for (iz i = 0; i < countof(text); i++) {
        text[i] = (u8)"hello, world\n"[i%13];
    }
    mfs_put(ctx, "f", FT_FILE, text, countof(text), 1700000000);
    mfs_put(ctx, "d", FT_DIR, 0, 0, 1700000000);
    mfs_put(ctx, "d/g", FT_FILE, "g\n", 2, 1700000000);
    if (archive.s) {
        mfs_put(ctx, "a.zip", FT_FILE, archive.s, archive.len, 1600000000);
    }
}

static s8 some_bytes(iz len, u32 seed)
{
    s8 r = {malloc((uz)len + 1), len};
    TEST(r.s);
    for (iz i = 0; i < len; i++) {
        seed = seed*1103515245 + 12345;
        r.s[i] = (u8)(seed >> 16);
    }
    return r;
}

// A new archive, added to, then deleted from
static void test_basic(os *ctx, arena a)
{
    mfs_reset(ctx);
    put_files(ctx, (s8){0});
    s8 text = mfs_get(ctx, "f");
    ZIP(ctx, 0, "a.zip", "f", "d/g");
    TEST(equals(zipos_output(ctx, 1),
                "  adding: f (deflated 99%)\n  adding: d/g (stored 0%)\n"));
    TEST(!zipos_output(ctx, 2).len);
    zlist l = entries(mfs_get(ctx, "a.zip"), &a);
    TEST(l.count == 2);
    TEST(equals(l.entries[0].name, "f") && equals(l.entries[1].name, "d/g"));
    TEST(l.entries[0].method == ZIP_DEFLATE);
    TEST(l.entries[0].crc == crc32_update(0, text.s, text.len, &(i32){0}));
    TEST(l.entries[0].usize == text.len);
    TEST(equals(l.data[1], "g\n"));

    ZIP(ctx, 0, "-q", "a.zip", "d");
    l = entries(mfs_get(ctx, "a.zip"), &a);
    TEST(l.count==3 && equals(l.entries[2].name, "d/"));

    ZIP(ctx, 0, "-d", "a.zip", "f");
    TEST(equals(zipos_output(ctx, 1), "deleting: f\n"));
    l = entries(mfs_get(ctx, "a.zip"), &a);
    TEST(l.count==2 && equals(l.entries[0].name, "d/g"));
    TEST(no_temp(ctx));
}

// Entries are read through a window of 1 MiB: here a central directory
// larger than that, so that headers straddle its edge, an entry larger
// than that, and entries out of file order, each a jump, which must all
// be copied as they were
static void test_window(os *ctx, arena a)
{
    enum { N = 5000 };
    zspec *spec  = calloc(N, sizeof(*spec));
    i32   *order = calloc(N, sizeof(*order));
    TEST(spec && order);
    char  *names = malloc(N*256);
    TEST(names);
    for (i32 i = 0; i < N; i++) {
        char *s = names + i*256;
        iz    n = 200 + i%50;
        memset(s, 'n', (uz)n);
        snprintf(s, 16, "e%05d", i);
        s[6] = '/';
        spec[i].name = (s8){(u8 *)s, n};
        spec[i].data = some_bytes(i%7, (u32)i);
        order[i] = i ^ 1;  // pairs swapped
    }
    free(spec[N-2].data.s);
    spec[N-2].data = some_bytes((iz)3 << 19, 7);  // 1.5 MiB
    order[N-1] = order[0];  // ...but the large one first in the file
    order[0]   = N - 2;

    s8 z = build(spec, N, order);
    mfs_reset(ctx);
    put_files(ctx, z);
    ZIP(ctx, 0, "-q", "a.zip", "f");
    TEST(!zipos_output(ctx, 2).len);
    zlist l = entries(mfs_get(ctx, "a.zip"), &a);
    TEST(l.count == N+1);
    for (i32 i = 0; i < N; i++) {
        TEST(s8equals(l.entries[i].name, spec[i].name));
        TEST(s8equals(l.data[i], spec[i].data));
    }
    TEST(equals(l.entries[N].name, "f"));

    // Its directory now in file order, but for one entry deleted
    char *gone = (char *)spec[N/2].name.s;
    gone[spec[N/2].name.len] = 0;
    ZIP(ctx, 0, "-qd", "a.zip", gone);
    l = entries(mfs_get(ctx, "a.zip"), &a);
    TEST(l.count == N);
    for (i32 i = 0; i < N; i++) {
        i32 k = i + (i >= N/2);
        if (k < N) {
            TEST(s8equals(l.entries[i].name, spec[k].name));
            TEST(s8equals(l.data[i], spec[k].data));
        }
    }
    TEST(no_temp(ctx));

    for (i32 i = 0; i < N; i++) {
        free(spec[i].data.s);
    }
    free(z.s);
    free(names);
    free(order);
    free(spec);
}

// Local headers are read through the window too, which here first holds
// the page (4 KiB) from the first entry's start, since the archive is too
// large for the window's first read, of its final 64 KiB, to include it.
// Each size of the first entry puts the second's header, or its name,
// within that page, across its edge, or past it.
static void test_edges(os *ctx, arena a)
{
    s8 big   = some_bytes(70000, 4);
    s8 small = some_bytes(10, 5);
    for (iz len = 3980; len <= 4070; len++) {
        s8    first   = some_bytes(len, 6);
        zspec spec[3] = {{S("a"), first}, {S("bbbbbbbbbb"), small},
                         {S("c"), big}};
        s8    z       = build(spec, 3, 0);
        mfs_reset(ctx);
        put_files(ctx, z);
        ZIP(ctx, 0, "-q", "a.zip", "f");
        TEST(!zipos_output(ctx, 2).len);
        arena tmp = a;
        zlist l   = entries(mfs_get(ctx, "a.zip"), &tmp);
        TEST(l.count == 4);
        for (i32 i = 0; i < 3; i++) {
            TEST(s8equals(l.entries[i].name, spec[i].name));
            TEST(s8equals(l.data[i], spec[i].data));
        }
        free(z.s);
        free(first.s);
    }
    free(small.s);
    free(big.s);
}

// An archive that shrinks while zip works ends early, as Info-ZIP reports
// it (2), naming the entry being copied, or else the archive. Reading
// ahead past the end fails, but the bytes needed are then read alone,
// so that only the entry cut short fails, not those before it. (The
// large one keeps them out of the window, which first holds the final
// 64 KiB.) A failed read is an I/O error (11). Either leaves the archive
// alone and no temporary file.
static void test_shrunk(os *ctx)
{
    s8    big     = some_bytes(100000, 1);
    s8    small   = some_bytes(100, 2);
    zspec spec[3] = {{S("a"), small}, {S("b"), small}, {S("c"), big}};
    s8    z       = build(spec, 3, 0);
    i64   cut     = 2*(ZIP_LOCAL_LEN + 1 + small.len) + ZIP_LOCAL_LEN + 1 + 10;

    struct {
        i32   when;
        i64   shrinkto;
        i64   failreadat;
        i32   status;
        char *err;
    } cases[] = {
        {FAULT_TEMP, cut, -1, 2,
         "\nzip error: Unexpected end of zip file (was copying c)\n"},
        {FAULT_OPEN, z.len-1, -1, 2,
         "\nzip error: Unexpected end of zip file (a.zip)\n"},
        {FAULT_TEMP, -1, cut, 11,
         "zip I/O error: Input/output error\n"
         "zip error: Input file read failure (was copying c)\n"},
        {FAULT_OPEN, -1, z.len-1, 11,
         "zip I/O error: Input/output error\n"
         "zip error: Input file read failure (a.zip)\n"},
    };
    for (i32 i = 0; i < countof(cases); i++) {
        mfs_reset(ctx);
        put_files(ctx, z);
        ctx->when       = cases[i].when;
        ctx->shrinkto   = cases[i].shrinkto;
        ctx->failreadat = cases[i].failreadat;
        ZIP(ctx, cases[i].status, "-q", "a.zip", "f");
        TEST(equals(zipos_output(ctx, 2), cases[i].err));
        iz len = cases[i].shrinkto<0 ? z.len : (iz)cases[i].shrinkto;
        TEST(s8equals(mfs_get(ctx, "a.zip"), (s8){z.s, len}));
        TEST(no_temp(ctx));
    }
    free(z.s);
    free(big.s);
    free(small.s);
}

// A file that an entry names (-u without paths), which the scan refuses
// as a link, swapped for a link between the scan and its reading: not
// read through it, but its entry kept, with the scan's warning. A path
// named on the command line is still read through one, as in Info-ZIP.
static void test_swapped_link(os *ctx, arena a)
{
    zspec spec[2] = {{S("f"), S("old\n")}, {S("e"), S("e\n")}};
    s8    z       = build(spec, 2, 0);
    for (i32 named = 0; named < 2; named++) {
        mfs_reset(ctx);
        mfs_put(ctx, "a.zip", FT_FILE, z.s, z.len, 1600000000);
        mfs_put(ctx, "f", FT_FILE, "new\n", 4, 1700000000);
        mfs_put(ctx, "secret", FT_FILE, "sec\n", 4, 1700000000);
        ctx->swapname = "f";
        ctx->swapto   = "secret";
        if (named) {
            ZIP(ctx, 0, "a.zip", "f");
            TEST(equals(zipos_output(ctx, 1), "updating: f (stored 0%)\n"));
            TEST(!zipos_output(ctx, 2).len);
        } else {
            ZIP(ctx, 0, "-u", "a.zip");
            TEST(!zipos_output(ctx, 1).len);
            TEST(equals(zipos_output(ctx, 2), "zip warning: not following "
                        "link that an entry names: f\n"));
        }
        TEST(!ctx->swapname);  // it was opened
        zlist l = entries(mfs_get(ctx, "a.zip"), &a);
        TEST(l.count==2 && equals(l.entries[0].name, "f"));
        TEST(equals(l.data[0], named ? "sec\n" : "old\n"));
        TEST(equals(l.data[1], "e\n"));
        TEST(no_temp(ctx));
    }
    free(z.s);
}

// A failed write stops zip (14), as Info-ZIP words it, leaving the
// archive alone and no temporary file
static void test_write_failure(os *ctx)
{
    s8    data    = some_bytes(3000, 3);
    zspec spec[2] = {{S("a"), data}, {S("b"), data}};
    s8    z       = build(spec, 2, 0);
    for (i64 at = 0; at < z.len; at += 1000) {
        mfs_reset(ctx);
        put_files(ctx, z);
        ctx->failwriteat = at;
        ZIP(ctx, 14, "-q", "a.zip", "f");
        TEST(equals(zipos_output(ctx, 2),
                    "zip I/O error: No space left on device\nzip error: "
                    "Output file write failure (write error on zip file)\n"));
        TEST(s8equals(mfs_get(ctx, "a.zip"), z));
        TEST(no_temp(ctx));
    }
    free(z.s);
    free(data.s);
}

// Small files to add, a to e, and their progress lines
static void put_small(os *ctx)
{
    mfs_reset(ctx);
    for (char name[2] = "a"; name[0] <= 'e'; name[0]++) {
        mfs_put(ctx, name, FT_FILE, name, 1, 1700000000);
    }
}
static char const small_lines[] =
    "  adding: a (stored 0%)\n  adding: b (stored 0%)\n"
    "  adding: c (stored 0%)\n  adding: d (stored 0%)\n"
    "  adding: e (stored 0%)\n";
#define ZIP_SMALL(ctx, status) \
    ZIP(ctx, status, "a.zip", "a", "b", "c", "d", "e")

// Messages to standard output are held, for a pipe or file until the
// end, and for a terminal until a check point (each entry, and each
// chunk of its data, read or copied) finds them waiting 100 ms since the
// buffer began to fill, the clock read only for a terminal with text
// waiting; the bytes are the same either way
static void test_pace(os *ctx)
{
    // Not a terminal: written once, no clock, however slow
    put_small(ctx);
    ZIP_SMALL(ctx, 0);
    TEST(equals(zipos_output(ctx, 1), small_lines));
    TEST(ctx->nwrites==1 && !ctx->clock.reads);
    TEST(!ctx->held);  // let go at the end
    put_small(ctx);
    ctx->clock.tick = 1000;
    ZIP_SMALL(ctx, 0);
    TEST(equals(zipos_output(ctx, 1), small_lines));
    TEST(ctx->nwrites==1 && !ctx->clock.reads);

    // A terminal, fast: the clock read, nothing flushed early
    put_small(ctx);
    ctx->tty[1] = 1;
    ZIP_SMALL(ctx, 0);
    TEST(equals(zipos_output(ctx, 1), small_lines));
    TEST(ctx->nwrites==1 && ctx->clock.reads);

    // Slow: flushed now and then, a flush taking at least two readings,
    // as the buffer begins to fill and at a check point. 100 ms apart,
    // each line is flushed at the next entry's first check point; 20 ms
    // apart, a few lines at a time.
    for (i64 tick = 20; tick <= 100; tick += 80) {
        put_small(ctx);
        ctx->tty[1]     = 1;
        ctx->clock.tick = tick;
        ZIP_SMALL(ctx, 0);
        TEST(equals(zipos_output(ctx, 1), small_lines));
        TEST(tick==100 ? ctx->nwrites==5
                       : ctx->nwrites>1 && ctx->nwrites<5);
        TEST(ctx->nwrites <= ctx->clock.reads/2 + 1);
    }

    // A large file, stored or deflated: the line before it is flushed
    // between chunks of its data (256 KiB reads), before all is written
    s8 big  = some_bytes((iz)1 << 21, 5);
    s8 text = some_bytes((iz)1 << 21, 6);
    for (iz i = 0; i < text.len; i++) {
        text.s[i] = (u8)('a' + (text.s[i]&3));
    }
    for (i32 level = 0; level < 2; level++) {
        mfs_reset(ctx);
        mfs_put(ctx, "a", FT_FILE, "a", 1, 1700000000);
        mfs_put(ctx, "big", FT_FILE, level ? text.s : big.s, big.len,
                1700000000);
        ctx->tty[1]     = 1;
        ctx->clock.tick = 50;
        ZIP(ctx, 0, level ? "-1" : "-0", "a.zip", "a", "big");
        TEST(ctx->nwrites == 2);
        s8 line = S("  adding: a (stored 0%)\n");
        s8 out  = zipos_output(ctx, 1);
        TEST(ctx->writes[0].end == line.len);
        TEST(ctx->writes[0].written < big.len);
        TEST(s8equals((s8){out.s, line.len}, line));
        TEST(equals((s8){out.s+line.len, out.len-line.len}, level
            ? "  adding: big (deflated 69%)\n"
            : "  adding: big (stored 0%)\n"));
    }
    free(text.s);

    // Copying a large entry: the line before it is flushed as its data
    // is copied, as a pipe's is only at the end
    zspec spec[2] = {{S("x"), S("x\n")}, {S("big"), big}};
    s8    z       = build(spec, 2, 0);
    for (i32 tty = 0; tty < 2; tty++) {
        mfs_reset(ctx);
        mfs_put(ctx, "a.zip", FT_FILE, z.s, z.len, 1600000000);
        ctx->tty[1]     = tty;
        ctx->clock.tick = 50;
        ZIP(ctx, 0, "-d", "a.zip", "x");
        TEST(equals(zipos_output(ctx, 1), "deleting: x\n"));
        TEST(ctx->nwrites == 1);
        TEST(tty ? ctx->writes[0].written<big.len
                 : ctx->writes[0].written>big.len);
    }
    free(z.s);
    free(big.s);

    // The usage, on a terminal, and the version, flushed at the end
    mfs_reset(ctx);
    ctx->tty[1] = 1;
    TEST(zipos_run(ctx, 0, 0, 0) == 0);
    s8 usage = zipos_output(ctx, 1);
    TEST(usage.len == zip_usage.len+zip_usage_posix.len+zip_usage_tail.len);
    TEST(s8equals((s8){usage.s, zip_usage.len}, zip_usage));
    TEST(ctx->nwrites == 1);
    ZIP(ctx, 0, "-v");
    s8 version = zipos_output(ctx, 1);
    TEST(version.len && version.s[version.len-1]=='\n');
    TEST(s8equals(version, (s8){zip_usage.s, version.len}));
}

// Standard output is flushed before anything written to standard error,
// so that the two keep their order where they are one (a terminal, or a
// file given both), and a run that fails keeps all its output
static void test_order(os *ctx)
{
    for (i32 tty = 0; tty < 2; tty++) {
        // A file gone as it is read, between two others, after a name
        // that matches nothing
        mfs_reset(ctx);
        mfs_put(ctx, "a", FT_FILE, "a", 1, 1700000000);
        mfs_put(ctx, "f", FT_FILE, "f", 1, 1700000000);
        mfs_put(ctx, "b", FT_FILE, "b", 1, 1700000000);
        ctx->swapname   = "f";
        ctx->swapto     = "nowhere";
        ctx->tty[1]     = tty;
        ctx->clock.tick = 1;
        ZIP(ctx, ZE_OPEN, "a.zip", "a", "missing", "f", "b");
        TEST(equals(zipos_output(ctx, 3),
            "zip warning: name not matched: missing\n"
            "  adding: a (stored 0%)\n"
            "  adding: f\n"
            "zip warning: No such file or directory\n"
            "zip warning: could not open for reading: f\n"
            "  adding: b (stored 0%)\n"
            "\nzip warning: Not all files were readable\n"
            "  files/entries read:  2 (2 bytes)  skipped:  1 (1 bytes)\n"));
        TEST(ctx->nwrites == 2);

        // A write that fails once all the lines are made: none is lost
        put_small(ctx);
        ctx->failwriteat = 100;
        ctx->tty[1]      = tty;
        ZIP_SMALL(ctx, ZE_WRITE);
        TEST(equals(zipos_output(ctx, 1), small_lines));
        TEST(ctx->nwrites == 1);
        s8 both = zipos_output(ctx, 3);
        iz n    = (iz)strlen(small_lines);
        TEST(both.len>n && !memcmp(both.s, small_lines, (uz)n));
        TEST(equals((s8){both.s+n, both.len-n},
                    "zip I/O error: No space left on device\nzip error: "
                    "Output file write failure (write error on zip file)\n"));
        TEST(no_temp(ctx));
    }
}

int main(void)
{
    (void)bytemove;
    os   *ctx = zipos_new((iz)1 << 28);
    iz    cap = (iz)1 << 26;
    arena a   = {0};
    a.beg = malloc((uz)cap);
    a.end = a.beg + cap;
    TEST(a.beg);

    test_basic(ctx, a);
    test_window(ctx, a);
    test_edges(ctx, a);
    test_shrunk(ctx);
    test_write_failure(ctx);
    test_swapped_link(ctx, a);
    test_pace(ctx);
    test_order(ctx);

    free(a.beg);
    zipos_free(ctx);
    puts("all zip program tests pass");
    return 0;
}
