// Unit tests of the unzip program (src/unzipcli.c), run in memory
// (test/unzipos.c): extraction and what it leaves, overwriting by -n,
// -o, -f, -u, and the prompt, links made last and nothing written
// through a link, zip bombs, an archive changed as it is read, damaged
// entries discarded, faults injected, and Windows conventions on any
// host; in every run, memory claimed only before the file system
// changes, and nothing changed but below the -d directory. On success
// prints "all unzip program tests pass". A failure traps.
// $ cc -g3 -fsanitize=address,undefined -o tests-unzipcli test/unzipclitests.c
#include "unzipos.c"
#include "../src/deflate.c"

#define TEST CHECK

static b32 equals(s8 a, char const *z)
{
    return s8equals(a, cstrs8(z));
}

// Run unzip with arguments, its name and environment (or null), which
// must exit with a status, else its messages are shown. A run that
// returns must leave nothing open.
static void expect(os *ctx, i32 status, unzipenv *env, char **argv,
                   i32 argc)
{
    i32 open = 0;
    i32 got  = unzipos_run(ctx, argv, argc, env, &open);
    if (got != status) {
        s8 out = unzipos_output(ctx, 3);
        fprintf(stderr, "unzip exited %d, not %d:\n%.*s", got, status,
                (int)out.len, out.s);
    }
    TEST(got == status);
    TEST(!open);
}
#define UNZIP(ctx, status, ...) \
    expect(ctx, status, 0, (char *[]){__VA_ARGS__}, \
           countof(((char *[]){__VA_ARGS__})))
// As UNZIP, run as zipinfo
#define ZIPINFO(ctx, status, ...) \
    expect(ctx, status, &(unzipenv){.argv0="zipinfo"}, \
           (char *[]){__VA_ARGS__}, countof(((char *[]){__VA_ARGS__})))
// As UNZIP, with a name and environment, an unzipenv's fields in
// parentheses: RUNENV(ctx, 0, (.argv0="ii", .zipinfo="-1"), "a.zip")
#define RUNENV(ctx, status, env, ...) \
    expect(ctx, status, &(unzipenv){RUNENV_ env}, (char *[]){__VA_ARGS__}, \
           countof(((char *[]){__VA_ARGS__})))
#define RUNENV_(...) __VA_ARGS__

// Compare output with what is expected, showing both if they differ.
static b32 output_is(os *ctx, i32 fd, char const *want)
{
    s8 got = unzipos_output(ctx, fd);
    if (!equals(got, want)) {
        fprintf(stderr, "output (%d):\n[%.*s]\nexpected:\n[%s]\n", fd,
                (int)got.len, got.s, want);
        return 0;
    }
    return 1;
}

// Whether output holds a string.
static b32 output_has(os *ctx, i32 fd, char const *want)
{
    s8 got = unzipos_output(ctx, fd);
    s8 w   = cstrs8(want);
    for (iz i = 0; i+w.len <= got.len; i++) {
        if (s8equals((s8){got.s+i, w.len}, w)) {
            return 1;
        }
    }
    return 0;
}

static void put_stdin(os *ctx, char const *text)
{
    ctx->in    = (u8 *)text;
    ctx->inlen = (iz)strlen(text);
}

// Archives

// 2020-01-02 03:04:06, as an MS-DOS date<<16 | time
#define DOSTIME (40u<<25 | 1u<<21 | 2u<<16 | 3u<<11 | 4u<<5 | 3u)
#define DOSUNIX 1577934246  // that, in UTC

enum { UNIX = 3<<8 | 30, FAT = 0<<8 | 20 };

typedef struct {
    char const *name;
    char const *data;   // or null for none
    i32  method;        // 0 stored, 8 deflated, or another
    u32  made;          // "version made by", or 0 for UNIX
    u32  extattr;       // or 0 for a 0644 Unix file
    u32  intattr;       // internal attributes (bit 0: text)
    u32  flags;
    u32  dostime;       // or 0 for DOSTIME
    s8   lextra;
    s8   cextra;
    s8   comp;          // data as stored, if not as the method makes it
    i64  crc;           // the CRC, if not -1... (0 for the real one)
    b32  badcrc;
    i64  usize;         // if not -1 (0 for the real size)
    i64  offset;        // a central offset, if not 0, else its own
    b32  nolocal;       // no local header or data of its own
    i32  datalen;       // data's length, if not by NUL
    s8   comment;
} xspec;

static s8 deflated(s8 data)
{
    iz    cap = (iz)1 << 25;
    byte *mem = malloc((uz)cap);
    TEST(mem);
    arena     a = {mem, mem+cap, 0, 0};
    deflator *d = deflate_new(&a, 9);
    s8        r = {malloc((uz)data.len*2 + 64), 0};
    TEST(r.s);
    zbuf b = {data.s, data.len, r.s, data.len*2 + 64};
    for (;;) {
        i32 got = deflate_stream(d, &b, DEF_FINISH);
        if (got == GZ_OK) {
            break;
        }
        TEST(got==GZ_NEEDOUT || got==GZ_NEEDIN);
    }
    r.len = b.out - r.s;
    free(mem);
    return r;
}

typedef struct {
    u8 *s;
    iz  len;
} zbytes;

static void zput(mbuf *b, void const *p, iz len)
{
    mbuf_put(b, p, len);
}

static void zput16(mbuf *b, u32 v)
{
    u8 t[2];
    put16(t, v);
    zput(b, t, 2);
}

static void zput32(mbuf *b, u32 v)
{
    u8 t[4];
    put32(t, v);
    zput(b, t, 4);
}

// An archive of entries, written as given, as a malloc'd string.
static s8 build(xspec const *spec, i32 n, char const *comment)
{
    mbuf  z     = {0};
    i64  *offs  = calloc((uz)n+1, sizeof(i64));
    s8   *comps = calloc((uz)n+1, sizeof(s8));
    u32  *crcs  = calloc((uz)n+1, sizeof(u32));
    i64  *sizes = calloc((uz)n+1, sizeof(i64));
    TEST(offs && comps && crcs && sizes);
    for (i32 i = 0; i < n; i++) {
        xspec const *e = spec + i;
        s8 data = e->data ? (s8){(u8 *)e->data, e->datalen ? e->datalen :
                                 (iz)strlen(e->data)} : (s8){0};
        comps[i] = e->comp.s ? e->comp :
                   e->method==8 ? deflated(data) : data;
        crcs[i]  = e->badcrc ? 0x12345678 :
                   crc32_update(0, data.s, data.len, &(i32){0});
        sizes[i] = e->usize ? e->usize : data.len;
        offs[i]  = e->offset ? e->offset : z.len;
        if (e->nolocal) {
            continue;
        }
        s8 name = cstrs8(e->name);
        zput32(&z, ZIP_LOCAL_SIG);
        zput16(&z, 20);
        zput16(&z, e->flags);
        zput16(&z, (u32)e->method);
        zput32(&z, e->dostime ? e->dostime : DOSTIME);
        zput32(&z, crcs[i]);
        zput32(&z, (u32)comps[i].len);
        zput32(&z, (u32)sizes[i]);
        zput16(&z, (u32)name.len);
        zput16(&z, (u32)e->lextra.len);
        zput(&z, name.s, name.len);
        zput(&z, e->lextra.s, e->lextra.len);
        zput(&z, comps[i].s, comps[i].len);
    }
    i64 cdoff = z.len;
    for (i32 i = 0; i < n; i++) {
        xspec const *e = spec + i;
        s8 name = cstrs8(e->name);
        zput32(&z, ZIP_CENTRAL_SIG);
        zput16(&z, e->made ? e->made : UNIX);
        zput16(&z, 20);
        zput16(&z, e->flags);
        zput16(&z, (u32)e->method);
        zput32(&z, e->dostime ? e->dostime : DOSTIME);
        zput32(&z, crcs[i]);
        zput32(&z, (u32)comps[i].len);
        zput32(&z, (u32)sizes[i]);
        zput16(&z, (u32)name.len);
        zput16(&z, (u32)e->cextra.len);
        zput16(&z, (u32)e->comment.len);
        zput16(&z, 0);
        zput16(&z, e->intattr);
        zput32(&z, e->extattr ? e->extattr : 0100644u<<16);
        zput32(&z, (u32)offs[i]);
        zput(&z, name.s, name.len);
        zput(&z, e->cextra.s, e->cextra.len);
        zput(&z, e->comment.s, e->comment.len);
    }
    i64 cdsize = z.len - cdoff;
    s8  c = comment ? cstrs8(comment) : (s8){0};
    zput32(&z, ZIP_END_SIG);
    zput16(&z, 0);
    zput16(&z, 0);
    zput16(&z, (u32)n);
    zput16(&z, (u32)n);
    zput32(&z, (u32)cdsize);
    zput32(&z, (u32)cdoff);
    zput16(&z, (u32)c.len);
    zput(&z, c.s, c.len);
    for (i32 i = 0; i < n; i++) {
        xspec const *e = spec + i;
        if (!e->comp.s && e->method==8) {
            free(comps[i].s);
        }
    }
    free(offs);
    free(comps);
    free(crcs);
    free(sizes);
    return (s8){z.s, z.len};
}

static void put_archive(os *ctx, char const *name, s8 z)
{
    mfs_put(ctx, name, FT_FILE, z.s, z.len, 1600000000);
}

// A local extended timestamp ("UT") with a modification and access time
static s8 utl(u32 mtime, u32 atime)
{
    static u8 buf[16][13];
    static i32 next;
    u8 *p = buf[next++ % countof(buf)];
    put16(p, 0x5455);
    put16(p+2, 9);
    p[4] = 3;
    put32(p+5, mtime);
    put32(p+9, atime);
    return (s8){p, 13};
}

// A local extended timestamp ("UT") with the times its flags announce:
// modification, access, and creation, in that order
static s8 utf(u32 flags, u32 mtime, u32 atime, u32 ctime)
{
    static u8 buf[16][17];
    static i32 next;
    u8 *p = buf[next++ % countof(buf)];
    u32 t[] = {mtime, atime, ctime};
    iz  len = 5;
    p[4] = (u8)flags;
    for (i32 k = 0; k < 3; k++) {
        if (flags & 1u<<k) {
            put32(p+len, t[k]);
            len += 4;
        }
    }
    put16(p, 0x5455);
    put16(p+2, (u32)(len - 4));
    return (s8){p, len};
}

// A Unix owner field ("ux", version 1, 4-byte IDs)
static s8 owner(u32 uid, u32 gid)
{
    static u8 buf[8][15];
    static i32 next;
    u8 *p = buf[next++ % countof(buf)];
    put16(p, 0x7875);
    put16(p+2, 11);
    p[4] = 1;
    p[5] = 4;
    put32(p+6, uid);
    p[10] = 4;
    put32(p+11, gid);
    return (s8){p, 15};
}

// An Info-ZIP Unicode path field ("up", version 1) naming utf8 for a
// stored name
static s8 upath(char const *stored, char const *utf8)
{
    static u8 buf[4][64];
    static i32 next;
    u8 *p = buf[next++ % countof(buf)];
    s8  s = cstrs8(stored);
    s8  u = cstrs8(utf8);
    TEST(u.len <= 64-9);
    put16(p, 0x7075);
    put16(p+2, (u32)(5 + u.len));
    p[4] = 1;
    put32(p+5, crc32_update(0, s.s, s.len, &(i32){0}));
    bytecopy(p+9, u.s, u.len);
    return (s8){p, 9 + u.len};
}

#define TEXT "hello hello hello hello\n" "hello hello hello hello\n" \
             "hello hello hello hello\n" "hello hello hello hello\n"

// The modes of files
#define DIRM(m)  ((0040000u|(m))<<16 | 0x10)
#define FILEM(m) ((0100000u|(m))<<16)
#define LINKM    ((0120777u)<<16)

static s8 file_data(os *ctx, char const *name)
{
    mfile *f = mfs_get(ctx, name);
    TEST(f && f->type==FT_FILE);
    return (s8){f->data ? f->data : (u8 *)"", f->len};
}

// Extraction of a small tree: directories made as needed, data, modes
// (Unix modes as they are, others with the umask), times from the local
// UT field, else the DOS time (in UTC here), and the messages
static void test_extract(os *ctx)
{
    xspec spec[] = {
        {.name="top/", .extattr=DIRM(0750), .lextra=utl(1500000000,
                                                         1500000100)},
        {.name="top/a.txt", .data=TEXT, .method=8, .extattr=FILEM(0640),
         .lextra=utl(1600000000, 1600000100)},
        {.name="top/s.sh", .data="#!/bin/sh\n", .extattr=FILEM(0755)},
        {.name="top/sub/deep/f", .data="deep\n"},
        {.name="fat.txt", .data="fat\r\n", .made=FAT, .extattr=0x21},
        {.name="empty", .data=""},
    };
    s8 z = build(spec, countof(spec), "the comment");
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->dest = S("out/");
    UNZIP(ctx, 0, "a.zip", "-d", "out");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "the comment\n"
        "   creating: out/top/\n"
        "  inflating: out/top/a.txt           \n"
        " extracting: out/top/s.sh            \n"
        " extracting: out/top/sub/deep/f      \n"
        " extracting: out/fat.txt             \n"
        " extracting: out/empty               \n"));
    TEST(output_is(ctx, 2, ""));

    TEST(equals(file_data(ctx, "out/top/a.txt"), TEXT));
    TEST(equals(file_data(ctx, "out/top/s.sh"), "#!/bin/sh\n"));
    TEST(equals(file_data(ctx, "out/top/sub/deep/f"), "deep\n"));
    TEST(equals(file_data(ctx, "out/empty"), ""));
    mfile *f = mfs_get(ctx, "out/top");
    TEST(f->type==FT_DIR && f->perm==0750 && f->mtime==1500000000 &&
         f->atime==1500000100);
    f = mfs_get(ctx, "out/top/a.txt");
    TEST(f->perm==0640 && f->mtime==1600000000 && f->atime==1600000100);
    f = mfs_get(ctx, "out/top/s.sh");
    TEST(f->perm==0755 && f->mtime==DOSUNIX);
    f = mfs_get(ctx, "out/top/sub");  // made along the way
    TEST(f->type==FT_DIR && f->perm==0755);
    f = mfs_get(ctx, "out/fat.txt");
    TEST(f->perm==0444);  // read-only, the umask taken
    ctx->umask = 077;
    mfs_get(ctx, "out/fat.txt")->live = 0;
    UNZIP(ctx, 0, "-qd", "out", "a.zip", "fat.txt");
    TEST(mfs_get(ctx, "out/fat.txt")->perm == 0400);
    TEST(output_is(ctx, 3, ""));
    free(z.s);

    // Owners, with -X, and set-ID bits, with -K
    xspec own[] = {
        {.name="own.txt", .data="o\n", .lextra=owner(123, 456),
         .extattr=FILEM(04755)},
    };
    z = build(own, countof(own), 0);
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    UNZIP(ctx, 0, "-q", "a.zip");
    f = mfs_get(ctx, "own.txt");
    TEST(f->uid==1000 && f->gid==1000 && f->perm==0755);
    UNZIP(ctx, 0, "-qoXK", "a.zip");
    f = mfs_get(ctx, "own.txt");
    TEST(f->uid==123 && f->gid==456 && f->perm==04755);
    free(z.s);
}

// Six files, each with a time of 1600000000 in its UT field
static s8 six_files(void)
{
    xspec spec[6] = {0};
    static char names[6][8];
    static char datas[6][8];
    for (i32 i = 0; i < 6; i++) {
        snprintf(names[i], sizeof(names[i]), "p/%c.txt", 'a'+i);
        snprintf(datas[i], sizeof(datas[i]), "%c%c%c\n", 'a'+i, 'a'+i,
                 'a'+i);
        spec[i].name   = names[i];
        spec[i].data   = datas[i];
        spec[i].lextra = utl(1600000000, 1600000000);
    }
    return build(spec, 6, 0);
}

// Whether a file is the one given, unreplaced.
static b32 same_file(os *ctx, char const *name, u64 id)
{
    mfile *f = mfs_get(ctx, name);
    return f && f->id==id;
}

// Existing files: kept by -n, all replaced by -o, the older replaced and
// none created by -f, the older replaced and the missing created by -u,
// each replacement a new file (not one written over)
static void test_overwrite(os *ctx)
{
    s8 z = six_files();
    struct {
        char *opts;
        b32   replaced[4];  // a older, b newer, c missing, d same time
    } cases[] = {
        {"-qn",  {0, 0, 1, 0}},
        {"-qo",  {1, 1, 1, 1}},
        {"-qfo", {1, 0, 0, 0}},
        {"-quo", {1, 0, 1, 0}},
        {"-qf",  {1, 0, 0, 0}},  // asking, answered y
        {"-qu",  {1, 0, 1, 0}},
    };
    for (i32 k = 0; k < countof(cases); k++) {
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        ctx->dest = S("out/");
        put_stdin(ctx, "y\n");
        mfs_put(ctx, "out", FT_DIR, 0, 0, 0);
        mfs_put(ctx, "out/p", FT_DIR, 0, 0, 0);
        u64 a = mfs_put(ctx, "out/p/a.txt", FT_FILE, "old", 3,
                        1500000000)->id;
        u64 b = mfs_put(ctx, "out/p/b.txt", FT_FILE, "new", 3,
                        1700000000)->id;
        u64 d = mfs_put(ctx, "out/p/d.txt", FT_FILE, "now", 3,
                        1600000000)->id;
        UNZIP(ctx, 0, cases[k].opts, "a.zip", "p/[abcd].txt", "-d", "out");
        TEST(same_file(ctx, "out/p/a.txt", a) != cases[k].replaced[0]);
        TEST(same_file(ctx, "out/p/b.txt", b) != cases[k].replaced[1]);
        TEST(!!mfs_get(ctx, "out/p/c.txt") == cases[k].replaced[2]);
        TEST(same_file(ctx, "out/p/d.txt", d) != cases[k].replaced[3]);
        if (cases[k].replaced[0]) {
            TEST(equals(file_data(ctx, "out/p/a.txt"), "aaa\n"));
        }
        TEST(equals(file_data(ctx, "out/p/b.txt"),
                    cases[k].replaced[1] ? "bbb\n" : "new"));
        TEST(!mfs_get(ctx, "out/p/e.txt"));
        TEST(output_is(ctx, 2, k<4 ? "" : "replace out/p/a.txt? [y]es, "
                       "[n]o, [A]ll, [N]one, [r]ename: "));
    }
    free(z.s);
}

// Answers to the prompt, as UnZip reads them, a line at a time: yes, no,
// a bad answer, rename, All, then None (for another run), and the end of
// input, treated as None with a warning
static void test_prompt(os *ctx)
{
    s8 z = six_files();
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    UNZIP(ctx, 0, "-q", "a.zip");
    u64 id[6];
    for (i32 i = 0; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "p/%c.txt", 'a'+i);
        id[i] = mfs_get(ctx, name)->id;
    }

    put_stdin(ctx, "y\nn\nwhat\n\nr\n\n\nren/x.txt\nA\n");
    UNZIP(ctx, 0, "a.zip");
    TEST(output_is(ctx, 3,
        "Archive:  a.zip\n"
        "replace p/a.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        " extracting: p/a.txt                 \n"
        "replace p/b.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        "replace p/c.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        "error:  invalid response [what]\n"
        "replace p/c.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        "error:  invalid response [{ENTER}]\n"
        "replace p/c.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        "new name: new name: new name: "
        " extracting: ren/x.txt               \n"
        "replace p/d.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        " extracting: p/d.txt                 \n"
        " extracting: p/e.txt                 \n"
        " extracting: p/f.txt                 \n"));
    TEST(!same_file(ctx, "p/a.txt", id[0]));
    TEST(same_file(ctx, "p/b.txt", id[1]));
    TEST(same_file(ctx, "p/c.txt", id[2]));
    TEST(equals(file_data(ctx, "ren/x.txt"), "ccc\n"));
    for (i32 i = 3; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "p/%c.txt", 'a'+i);
        TEST(!same_file(ctx, name, id[i]));
        id[i] = mfs_get(ctx, name)->id;
    }

    put_stdin(ctx, "n\nN\n");
    UNZIP(ctx, 0, "-q", "a.zip", "p/[def].txt");
    TEST(output_is(ctx, 2,
        "replace p/d.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        "replace p/e.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "));
    for (i32 i = 3; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "p/%c.txt", 'a'+i);
        TEST(same_file(ctx, name, id[i]));
    }

    put_stdin(ctx, "");
    UNZIP(ctx, 1, "a.zip", "p/f.txt", "p/e.txt");
    TEST(output_is(ctx, 3,
        "Archive:  a.zip\n"
        "replace p/e.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        " NULL\n(EOF or read error, treating as \"[N]one\" ...)\n"));
    TEST(same_file(ctx, "p/e.txt", id[4]));
    TEST(same_file(ctx, "p/f.txt", id[5]));

    // A new name as a directory makes one; the end of input when asked
    // for one keeps the name, as UnZip, and so asks again
    put_stdin(ctx, "r\nnew/dir/\n");
    UNZIP(ctx, 0, "-q", "a.zip", "p/f.txt");
    TEST(mfs_get(ctx, "new/dir")->type == FT_DIR);
    TEST(same_file(ctx, "p/f.txt", id[5]));
    put_stdin(ctx, "r\n");
    UNZIP(ctx, 1, "-q", "a.zip", "p/f.txt");
    TEST(output_is(ctx, 2,
        "replace p/f.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: new name: "
        "replace p/f.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        " NULL\n(EOF or read error, treating as \"[N]one\" ...)\n"));
    TEST(same_file(ctx, "p/f.txt", id[5]));

    // Many empty new names, each asked for again, need no more memory
    static char many[4096];
    char *p = many;
    p += snprintf(p, 8, "r\n");
    for (i32 i = 0; i < 1000; i++) {
        *p++ = '\n';
    }
    snprintf(p, 16, "many.txt\n");
    put_stdin(ctx, many);
    UNZIP(ctx, 0, "-q", "a.zip", "p/f.txt");
    TEST(equals(file_data(ctx, "many.txt"), "fff\n"));

    // So do many new names, each taken, then the longest, of directories
    // with names to filter, under a long -d directory, so long in all
    // that the system refuses it
    static char longest[1<<16];
    p = longest;
    for (i32 i = 0; i < 300; i++) {
        p += snprintf(p, 16, "r\np/%c.txt\n", 'a'+i%6);
    }
    p += snprintf(p, 8, "r\n");
    for (i32 i = 0; i < 4000/200; i++) {
        p += snprintf(p, 8, "\x01\x7f\x80");
        memset(p, 'x', 196);
        p += 196;
        *p++ = '/';
    }
    p += snprintf(p, 8, "z\n");
    static char exdir[255+1+200+1];
    static char dest[countof(exdir)+1];
    memset(exdir, 'd', 255);
    mfs_put(ctx, exdir, FT_DIR, 0, 0, 0);
    memset(exdir+255, '/', 1);
    memset(exdir+256, 'd', 200);
    snprintf(dest, sizeof(dest), "%s/", exdir);
    ctx->dest = cstrs8(dest);
    UNZIP(ctx, 0, "-qod", dest, "a.zip");
    put_stdin(ctx, longest);
    UNZIP(ctx, 2, "-qd", exdir, "a.zip", "p/a.txt", "p/b.txt");
    s8 err = unzipos_output(ctx, 2);
    s8 end = S("? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
               " NULL\n(EOF or read error, treating as \"[N]one\" ...)\n");
    TEST(err.len>end.len && s8equals((s8){err.s+err.len-end.len, end.len},
                                     end));
    free(z.s);
}

// A tree outside the -d directory, out, which nothing may change, and
// links within out that lead to it, as an earlier run or another
// program may have left them
static void put_outside(os *ctx)
{
    mfs_put(ctx, "outside", FT_DIR, 0, 0, 1);
    mfs_put(ctx, "outside/f", FT_FILE, "sentinel\n", 9, 1);
    mfs_put(ctx, "outside/tmp", FT_DIR, 0, 0, 1);
    mfs_put(ctx, "out", FT_DIR, 0, 0, 1);
    mfs_put(ctx, "out/esc", FT_LINK, "../outside", 10, 1);
    mfs_put(ctx, "out/abs", FT_LINK, "/outside", 8, 1);
    mfs_put(ctx, "out/fesc", FT_LINK, "../outside/f", 12, 1);
    mfs_put(ctx, "out/new", FT_LINK, "../outside/new", 14, 1);
}

// Links are made last, each in place of the empty file that held its
// place, and none is written through: not those of the archive, nor
// those already there, which an entry of the same name replaces
static void test_links(os *ctx)
{
    xspec spec[] = {
        {.name="top/link", .data="a.txt", .extattr=LINKM},
        {.name="top/a.txt", .data=TEXT, .method=8},
        {.name="lnk", .data="../outside", .extattr=LINKM},
        {.name="lnk/pwned.txt", .data="pwned\n"},
        {.name="abs", .data="/", .extattr=LINKM},
        {.name="abs/outside/pwned.txt", .data="pwned\n"},
        {.name="esc/pwned.txt", .data="pwned\n"},
        {.name="abs/tmp/pwned.txt", .data="pwned\n"},
        {.name="fesc", .data="replaced\n"},
        {.name="new", .data="not through\n"},
        {.name="zero", .data="", .extattr=LINKM},  // empty: a file
    };
    s8 z = build(spec, countof(spec), 0);
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    put_outside(ctx);
    ctx->dest = S("out/");
    msnap before = mfs_snapshot(ctx);
    UNZIP(ctx, 2, "-o", "a.zip", "-d", "out");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "    linking: out/top/link            -> a.txt \n"
        "  inflating: out/top/a.txt           \n"
        "    linking: out/lnk                 -> ../outside \n"
        "    linking: out/abs                 -> / \n"
        " extracting: out/fesc                \n"
        " extracting: out/new                 \n"
        " extracting: out/zero                \n"
        "finishing deferred symbolic links:\n"
        "  out/top/link           -> a.txt\n"
        "  out/lnk                -> ../outside\n"
        "  out/abs                -> /\n"));
    TEST(output_is(ctx, 2,
        "checkdir error:  out/lnk exists but is not directory\n"
        "                 unable to process lnk/pwned.txt.\n"
        "checkdir error:  out/abs exists but is not directory\n"
        "                 unable to process abs/outside/pwned.txt.\n"
        "checkdir error:  out/esc exists but is not directory\n"
        "                 unable to process esc/pwned.txt.\n"
        "checkdir error:  out/abs exists but is not directory\n"
        "                 unable to process abs/tmp/pwned.txt.\n"));
    TEST(mfs_unchanged(ctx, before, S("out")));
    mfs_snapfree(before);
    mfile *f = mfs_get(ctx, "out/top/link");
    TEST(f->type==FT_LINK && equals((s8){f->data, f->len}, "a.txt"));
    TEST(mfs_get(ctx, "out/lnk")->type == FT_LINK);
    TEST(mfs_get(ctx, "out/esc")->type == FT_LINK);
    TEST(equals(file_data(ctx, "out/fesc"), "replaced\n"));
    TEST(equals(file_data(ctx, "out/new"), "not through\n"));
    TEST(equals(file_data(ctx, "out/zero"), ""));

    // Again, every link made now in the way of its entries
    before = mfs_snapshot(ctx);
    UNZIP(ctx, 2, "-qo", "a.zip", "-d", "out");
    TEST(mfs_unchanged(ctx, before, S("out")));
    mfs_snapfree(before);

    // The -d directory itself may be a link, followed
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    put_outside(ctx);
    mfs_put(ctx, "dlink", FT_LINK, "out", 3, 1);
    ctx->dest = S("dlink/");
    before = mfs_snapshot(ctx);
    UNZIP(ctx, 2, "-qd", "dlink", "a.zip");
    TEST(mfs_unchanged(ctx, before, S("out")));
    mfs_snapfree(before);
    TEST(equals(file_data(ctx, "out/top/a.txt"), TEXT));
    TEST(mfs_get(ctx, "out/top/link")->type == FT_LINK);

    // A link and a file of the same name: the file, once the link's
    // placeholder is gone, so the link is not made
    xspec dup[] = {
        {.name="same", .data="a target", .extattr=LINKM},
        {.name="same", .data="a file\n"},
    };
    s8 d = build(dup, countof(dup), 0);
    mfs_reset(ctx);
    put_archive(ctx, "d.zip", d);
    UNZIP(ctx, 0, "-o", "d.zip");
    TEST(output_is(ctx, 3,
        "Archive:  d.zip\n"
        "    linking: same                    -> a target \n"
        " extracting: same                    \n"
        "finishing deferred symbolic links:\n"
        "warning:  deferred symlink (same) failed:\n"
        "          invalid placeholder file\n"));
    TEST(equals(file_data(ctx, "same"), "a file\n"));
    free(d.s);

    // A link's target is what its data holds, though its size says more,
    // not that and whatever was in the room for the rest (a fuzzer's find)
    xspec longer[] = {
        {.name="l", .data="a target", .method=8, .usize=17,
         .extattr=LINKM},
    };
    d = build(longer, countof(longer), 0);
    mfs_reset(ctx);
    put_archive(ctx, "l.zip", d);
    UNZIP(ctx, 0, "-q", "l.zip");
    f = mfs_get(ctx, "l");
    TEST(f->type==FT_LINK && equals((s8){f->data, f->len}, "a target"));
    ctx->windows = 1;  // as a file, with no NUL to end it
    UNZIP(ctx, 0, "-qo", "l.zip");
    TEST(equals(file_data(ctx, "l"), "a target"));
    free(d.s);

    // A target too long to keep (over 4096 bytes) is refused with a
    // warning (1), ending the line that names it, and nothing is made
    static char far[5000];
    memset(far, 'x', sizeof(far)-1);
    xspec toolong[] = {
        {.name="far", .data=far, .extattr=LINKM},
        {.name="after.txt", .data="after\n"},
    };
    d = build(toolong, countof(toolong), 0);
    mfs_reset(ctx);
    put_archive(ctx, "t.zip", d);
    UNZIP(ctx, 1, "t.zip");
    TEST(output_is(ctx, 3,
        "Archive:  t.zip\n"
        "    linking: far                     warning:  symbolic link (far) "
        "failed: target too long\n"
        " extracting: after.txt               \n"));
    TEST(!mfs_get(ctx, "far"));
    TEST(equals(file_data(ctx, "after.txt"), "after\n"));
    UNZIP(ctx, 1, "-qo", "t.zip");
    TEST(output_is(ctx, 3, "warning:  symbolic link (far) failed: target "
                           "too long\n"));
    TEST(!mfs_get(ctx, "far"));
    free(d.s);

    // A placeholder replaced by a later entry makes no link, though the
    // file system gives the later file its inode, as ext4 does (and
    // unzipos.c): a later link is made, and an empty file left, as by
    // UnZip, whose placeholder holds the target
    xspec linklink[] = {
        {.name="x", .data="first-target", .extattr=LINKM},
        {.name="x", .data="second-target", .extattr=LINKM},
    };
    d = build(linklink, countof(linklink), 0);
    mfs_reset(ctx);
    put_archive(ctx, "ll.zip", d);
    UNZIP(ctx, 0, "-o", "ll.zip");
    TEST(output_is(ctx, 3,
        "Archive:  ll.zip\n"
        "    linking: x                       -> first-target \n"
        "    linking: x                       -> second-target \n"
        "finishing deferred symbolic links:\n"
        "warning:  deferred symlink (x) failed:\n"
        "          invalid placeholder file\n"
        "  x                      -> second-target\n"));
    f = mfs_get(ctx, "x");
    TEST(f->type==FT_LINK && equals((s8){f->data, f->len}, "second-target"));
    free(d.s);
    xspec linkempty[] = {
        {.name="x", .data="first-target", .extattr=LINKM},
        {.name="x", .data=""},
    };
    d = build(linkempty, countof(linkempty), 0);
    mfs_reset(ctx);
    put_archive(ctx, "le.zip", d);
    UNZIP(ctx, 0, "-o", "le.zip");
    TEST(output_is(ctx, 3,
        "Archive:  le.zip\n"
        "    linking: x                       -> first-target \n"
        " extracting: x                       \n"
        "finishing deferred symbolic links:\n"
        "warning:  deferred symlink (x) failed:\n"
        "          invalid placeholder file\n"));
    TEST(mfs_get(ctx, "x")->type == FT_FILE);
    TEST(equals(file_data(ctx, "x"), ""));
    // Not its name but its identity: on Windows, a name in another case
    linkempty[1].name = "X";
    free(d.s);
    d = build(linkempty, countof(linkempty), 0);
    mfs_reset(ctx);
    ctx->windows = 1;
    put_archive(ctx, "le.zip", d);
    UNZIP(ctx, 0, "-qo", "le.zip");
    TEST(output_is(ctx, 3,
        "warning:  deferred symlink (x) failed:\n"
        "          invalid placeholder file\n"));
    TEST(equals(file_data(ctx, "x"), ""));
    free(d.s);
    free(z.s);
}

// Overlapped entries, a zip bomb, are refused before anything is
// written (12), as is an entry whose data reaches into the next
static void test_bomb(os *ctx)
{
    xspec spec[] = {
        {.name="a.txt", .data=TEXT, .method=8},
        {.name="b.txt", .data=TEXT, .method=8, .offset=-1, .nolocal=1},
    };
    s8 z = build(spec, countof(spec), 0);
    // the second's offset, as the first's: 0 (the builder's -1 marks it)
    u8 *cd = z.s + z.len - 22 - 2*(46+5);
    put32(cd + 46+5 + 42, 0);
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->dest = S("out/");
    UNZIP(ctx, 12, "-d", "out", "a.zip");
    TEST(output_is(ctx, 3,
        "Archive:  a.zip\n"
        "error: invalid zip file with overlapped components (possible zip "
        "bomb)\n"));
    TEST(!ctx->nchanges);
    UNZIP(ctx, 12, "-t", "a.zip");
    free(z.s);

    // A compressed size reaching past the central directory, even to
    // 2^63 - 1 (Zip64), is listed, as UnZip lists it, but its data is
    // refused before any entry's is read, the others' too, where UnZip
    // reads it by its local header's size
    for (i32 big = 0; big < 2; big++) {
        xspec spec2[] = {
            {.name="a.txt", .data=TEXT, .method=8},
            {.name="r", .data="x",
             .cextra={(u8 *)"\x01\x00\x08\x00" "\xff\xff\xff\xff\xff\xff"
                      "\xff\x7f", big ? 12 : 0}},
            {.name="c.txt", .data=TEXT},
        };
        z = build(spec2, countof(spec2), 0);
        u8 *r = z.s + get32(z.s+z.len-22+16) + 46+5;  // the second's header
        put32(r+20, big ? 0xffffffff : 268435457);
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        UNZIP(ctx, 0, "-l", "a.zip");
        TEST(output_has(ctx, 1, "   r\n"));
        TEST(output_has(ctx, 1, "3 files\n"));
        UNZIP(ctx, 0, "-v", "a.zip");
        TEST(output_has(ctx, 1, big ? " 9223372036854775807 -214748364% "
                                    : " 268435457 -214748364% "));
        for (i32 how = 0; how < 3; how++) {
            mfs_reset(ctx);
            put_archive(ctx, "a.zip", z);
            ctx->dest = S("out/");
            if (how == 2) {
                UNZIP(ctx, 12, "-o", "-d", "out", "a.zip");
            } else {
                UNZIP(ctx, 12, how ? "-p" : "-t", "a.zip");
            }
            TEST(output_has(ctx, 3,
                "error: invalid zip file with overlapped components "
                "(possible zip bomb)\n"));
            TEST(!output_has(ctx, 3, "a.txt"));
            TEST(!output_has(ctx, 3, "hello"));
            TEST(!ctx->nchanges);
        }
        // Selected alone, the others are read
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        UNZIP(ctx, 0, "-t", "a.zip", "a.txt", "c.txt");
        free(z.s);
    }
}

// An archive rewritten as it is extracted, as by another process, its
// central directory read again for each entry: an entry then not among
// those that the overlap check found, or found again, is refused as
// overlapped (12), and one that no longer reads as planned, nor fits
// the room planned for it, ends the run as a read error (3), those
// extracted before kept, and no memory claimed meanwhile (unzipos.c
// checks that)
static void test_changed(os *ctx)
{
    xspec spec[] = {
        {.name="a.txt", .data="a\n"},
        {.name="b.txt", .data="b\n"},
        {.name="c.txt", .data="c\n",
         .cextra=S8("\x99\x99\x06\x00" "abcdef")},  // an unknown field
    };
    s8 z = build(spec, countof(spec), 0);
    for (i32 how = 0; how < 5; how++) {
        s8  w = {malloc((uz)z.len), z.len};
        TEST(w.s);
        bytecopy(w.s, z.s, z.len);
        u8 *b = w.s + get32(w.s+w.len-22+16) + 46+5;  // the second's header
        u8 *c = b + 46+5;                             // the third's
        switch (how) {
        case 0:
            put32(c+42, get32(b+42));  // where the second begins
            break;
        case 1:
            put32(c+42, get32(c+42)-1);  // within the second
            break;
        case 2:
            put16(c+10, 99);  // a method not planned for
            break;
        case 3:
            put16(c+28, 6+10);  // its name reaching into the end record
            break;
        case 4:
            put16(c+28, 5+10);  // its name longer, its extra field in it
            put16(c+30, 0);
            break;
        }
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        ctx->when    = FAULT_CHANGE;
        ctx->rewrite = w;
        UNZIP(ctx, how<2 ? 12 : 3, "-q", "a.zip");
        TEST(output_has(ctx, 3, how<2 ?
            "error: invalid zip file with overlapped components (possible "
            "zip bomb)\n" : "error:  zipfile read error\n"));
        TEST(equals(file_data(ctx, "a.txt"), "a\n"));
        TEST(equals(file_data(ctx, "b.txt"), "b\n"));
        TEST(!mfs_get(ctx, "c.txt"));
        free(w.s);
    }
    free(z.s);
}

// Damaged entries are not kept, but the others are: a bad CRC, stored
// or deflated, invalid deflated data, and more data than its size
static void test_damaged(os *ctx)
{
    xspec spec[] = {
        {.name="good.txt", .data="good\n", .method=8},
        {.name="bad.txt", .data=TEXT, .method=8, .badcrc=1},
        {.name="badstored.txt", .data="x\n", .badcrc=1},
        {.name="baddata.txt", .data=TEXT, .method=8,
         .comp={(u8 *)"\xff\xff\xff\xff\xff\xff\xff\xff", 8}},
        {.name="overrun.txt", .data=TEXT, .method=8, .usize=10},
        {.name="last.txt", .data="last\n"},
    };
    s8 z = build(spec, countof(spec), 0);
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    put_stdin(ctx, "y\n");  // no prompts
    UNZIP(ctx, 2, "a.zip");
    TEST(output_is(ctx, 3,
        "Archive:  a.zip\n"
        "  inflating: good.txt                \n"
        "  inflating: bad.txt                  bad CRC 51ef3ca2  (should be "
        "12345678)\n"
        " extracting: badstored.txt            bad CRC 46ea081f  (should be "
        "12345678)\n"
        "  inflating: baddata.txt             \n"
        "  error:  invalid compressed data to inflate\n"
        "  inflating: overrun.txt             \n"
        "  error:  invalid compressed data to inflate\n"
        " extracting: last.txt                \n"));
    TEST(equals(file_data(ctx, "good.txt"), "good\n"));
    TEST(equals(file_data(ctx, "last.txt"), "last\n"));
    TEST(!mfs_get(ctx, "bad.txt") && !mfs_get(ctx, "badstored.txt"));
    TEST(!mfs_get(ctx, "baddata.txt") && !mfs_get(ctx, "overrun.txt"));

    // Piped, the overrun stops at its size
    UNZIP(ctx, 2, "-p", "a.zip", "overrun.txt");
    TEST(output_is(ctx, 1, "hello hell"));
    free(z.s);

    // A directory that ends early (a bad third header), whose offsets
    // only seem to be shifted (its own offset, short by two), and so are
    // shifted back for the first entry, as UnZip does: only the entries
    // read are shifted, not the rest, which were never set (a fuzzer's
    // find)
    xspec three[] = {
        {.name="a.txt", .data="a\n"},
        {.name="b.txt", .data="b\n"},
        {.name="c.txt", .data="c\n"},
    };
    z = build(three, countof(three), 0);
    u8 *end   = z.s + z.len - 22;
    u32 cdoff = get32(end+16);
    put32(end+16, cdoff-2);
    z.s[cdoff + 2*(46+5)] = 'X';
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    UNZIP(ctx, 3, "-q", "a.zip");
    TEST(output_is(ctx, 2,
        "warning [a.zip]:  2 extra bytes at beginning or within zipfile\n"
        "  (attempting to process anyway)\n"
        "error:  expected central file header signature not found (file "
        "#3).\n"
        "  (please check that you have transferred or created the zipfile "
        "in the\n"
        "  appropriate BINARY mode and that you have compiled UnZip "
        "properly)\n"
        "file #1:  bad zipfile offset (local header sig):  2\n"
        "  (attempting to re-compensate)\n"));
    TEST(equals(file_data(ctx, "a.txt"), "a\n"));
    TEST(equals(file_data(ctx, "b.txt"), "b\n"));

    // Data before an archive whose Zip64 record counts more entries than
    // 2^63, which its offsets, unadjusted, leave unchecked: a damaged
    // archive, not too large for memory (a fuzzer's find), whose entries
    // are read, as UnZip reads them, but for the count
    s8     two = build(three, 2, 0);
    mbuf   b   = {0};
    iz     cd  = two.len - 22;
    u32    off = get32(two.s+cd+16);
    u32    len = get32(two.s+cd+12);
    zput(&b, "#!/bin/sh\nexit\n", 15);
    zput(&b, two.s, cd);
    i64 rec = b.len;
    zput32(&b, ZIP_END64_SIG);
    zput32(&b, 44);
    zput32(&b, 0);
    zput16(&b, 45);
    zput16(&b, 45);
    zput32(&b, 0);
    zput32(&b, 0);
    zput32(&b, 2);  // on this disk
    zput32(&b, 0);
    zput32(&b, 2);  // in all: 2 + 2^63
    zput32(&b, 0x80000000);
    zput32(&b, len);
    zput32(&b, 0);
    zput32(&b, off);
    zput32(&b, 0);
    zput32(&b, ZIP_LOC64_SIG);
    zput32(&b, 0);
    zput32(&b, (u32)rec);
    zput32(&b, 0);
    zput32(&b, 1);
    zput32(&b, ZIP_END_SIG);
    zput32(&b, 0);
    zput32(&b, 0xffffffff);
    zput32(&b, 0xffffffff);
    zput32(&b, 0xffffffff);
    zput16(&b, 0);
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", (s8){b.s, b.len});
    UNZIP(ctx, 3, "-q", "a.zip");
    TEST(output_is(ctx, 2,
        "warning [a.zip]:  15 extra bytes at beginning or within zipfile\n"
        "  (attempting to process anyway)\n"
        "error:  expected central file header signature not found (file "
        "#3).\n"
        "  (please check that you have transferred or created the zipfile "
        "in the\n"
        "  appropriate BINARY mode and that you have compiled UnZip "
        "properly)\n"));
    TEST(equals(file_data(ctx, "a.txt"), "a\n"));
    TEST(equals(file_data(ctx, "b.txt"), "b\n"));
    free(b.s);
    free(two.s);
    free(z.s);
}

// An archive from build, its end record replaced by Zip64 records and a
// saturated end record, after prefix, its offsets unadjusted for it: as
// it ends up after "cat stub archive.zip", or with a zipapp's #! line.
static s8 zip64_after(s8 z, char const *prefix)
{
    mbuf b   = {0};
    iz   cd  = z.len - 22;
    u32  n   = get16(z.s+cd+10);
    u32  len = get32(z.s+cd+12);
    u32  off = get32(z.s+cd+16);
    zput(&b, prefix, (iz)strlen(prefix));
    zput(&b, z.s, cd);
    zput32(&b, ZIP_END64_SIG);
    zput32(&b, 44);
    zput32(&b, 0);
    zput16(&b, 45);
    zput16(&b, 45);
    zput32(&b, 0);
    zput32(&b, 0);
    for (i32 i = 0; i < 2; i++) {  // on this disk, in all
        zput32(&b, n);
        zput32(&b, 0);
    }
    zput32(&b, len);
    zput32(&b, 0);
    zput32(&b, off);
    zput32(&b, 0);
    zput32(&b, ZIP_LOC64_SIG);
    zput32(&b, 0);
    zput32(&b, off+len);  // where the record would be without prefix
    zput32(&b, 0);
    zput32(&b, 1);
    zput32(&b, ZIP_END_SIG);
    zput32(&b, 0);
    zput32(&b, 0xffffffff);
    zput32(&b, 0xffffffff);
    zput32(&b, 0xffffffff);
    zput16(&b, 0);
    return (s8){b.s, b.len};
}

// Central directories read as UnZip reads them, header by header while
// they parse, whatever the end record's count: more headers than it
// counts, or fewer, even than the directory could hold, are processed,
// then reported (3); a count that wrapped past 65,535 entries, as
// writers without Zip64 leave it, is no error; a Zip64 end record just
// before its locator, rather than where that says, is data before the
// archive (1); and an end record whose comment runs past the end of the
// file is taken, its comment cut short (1)
static void test_directory(os *ctx)
{
    xspec three[] = {
        {.name="a.txt", .data="a\n"},
        {.name="b.txt", .data="b\n"},
        {.name="c.txt", .data="c\n"},
    };
    s8  z   = build(three, countof(three), 0);
    u8 *end = z.s + z.len - 22;
    u32 counts[] = {0, 2, 4, 5};  // 5: more than the directory holds
    for (i32 i = 0; i < countof(counts); i++) {
        put16(end+8, counts[i]);
        put16(end+10, counts[i]);
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        UNZIP(ctx, 3, "-q", "a.zip");
        TEST(output_is(ctx, 2,
            "error:  expected central file header signature not found (file "
            "#4).\n"
            "  (please check that you have transferred or created the "
            "zipfile in the\n"
            "  appropriate BINARY mode and that you have compiled UnZip "
            "properly)\n"));
        TEST(equals(file_data(ctx, "a.txt"), "a\n"));
        TEST(equals(file_data(ctx, "c.txt"), "c\n"));
        UNZIP(ctx, 3, "-l", "a.zip");
        TEST(output_has(ctx, 1, "c.txt\n"));
        TEST(!output_has(ctx, 1, "3 files"));
    }
    put16(end+8, 3);
    put16(end+10, 3);

    // A comment that the file cuts short: shown as far as it goes, then
    // UnZip's caution, even for one with none of it there
    char const *cuts[][2] = {
        {"12345", "Archive:  a.zip\n12345\ncaution:  zipfile comment "
                  "truncated\n"},
        {"", "Archive:  a.zip\n\ncaution:  zipfile comment truncated\n"},
    };
    for (i32 i = 0; i < countof(cuts); i++) {
        s8 c = build(three, countof(three), cuts[i][0]);
        put16(c.s+c.len-(iz)strlen(cuts[i][0])-2, i ? 0xffff : 10);
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", c);
        UNZIP(ctx, 1, "-z", "a.zip");
        TEST(output_is(ctx, 3, cuts[i][1]));
        UNZIP(ctx, 1, "-t", "a.zip");
        TEST(output_has(ctx, 1, cuts[i][1]));
        TEST(output_has(ctx, 1, "No errors detected in compressed data"));
        UNZIP(ctx, 0, "-tq", "a.zip");  // not shown: no caution
        UNZIP(ctx, 1, "-o", "a.zip");
        TEST(equals(file_data(ctx, "c.txt"), "c\n"));
        free(c.s);
    }

    // Zip64 records after data that their offsets do not account for: the
    // record is found just before its locator, as find_ecrec64 looks
    s8 sfx = zip64_after(z, "#!/bin/sh\nexit\n");
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", sfx);
    UNZIP(ctx, 1, "-q", "a.zip");
    TEST(output_is(ctx, 2,
        "[a.zip]\n"
        "error: End-of-centdir-64 signature not where expected (prepended "
        "bytes?)\n"
        "  (attempting to process anyway)\n"
        "warning [a.zip]:  15 extra bytes at beginning or within zipfile\n"
        "  (attempting to process anyway)\n"));
    TEST(equals(file_data(ctx, "a.txt"), "a\n"));
    TEST(equals(file_data(ctx, "c.txt"), "c\n"));
    UNZIP(ctx, 0, "-z", "a.zip");
    TEST(output_is(ctx, 3,
        "Archive:  a.zip\n"
        "error: End-of-centdir-64 signature not where expected (prepended "
        "bytes?)\n"
        "  (attempting to process anyway)\n"));
    // ...and not there either: a damaged archive, perhaps not the one
    iz rec = sfx.len - 22 - 20 - 56;
    TEST(get32(sfx.s+rec) == ZIP_END64_SIG);
    sfx.s[rec] ^= 1;
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", sfx);
    UNZIP(ctx, 9, "-t", "a.zip");
    TEST(output_is(ctx, 3,
        "Archive:  a.zip\n"
        "fatal error: read failure while seeking for End-of-centdir-64 "
        "signature.\n"
        "  This zipfile is corrupt.\n"
        "unzip:  cannot find zipfile directory in one of a.zip or\n"
        "        a.zip.zip, and cannot find a.zip.ZIP, period.\n"));
    free(sfx.s);
    free(z.s);

    // A count wrapped past 65,535 entries, without Zip64 records, and
    // read whole, but one more is an error
    i32    n    = 65536 + 3;
    xspec *many = calloc((uz)n, sizeof(*many));
    char  *names = calloc((uz)n, 8);
    TEST(many && names);
    for (i32 i = 0; i < n; i++) {
        snprintf(names+8*i, 8, "%x", i);
        many[i] = (xspec){.name=names+8*i};
    }
    z = build(many, n, 0);
    TEST(get16(z.s+z.len-22+10) == 3);
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    UNZIP(ctx, 0, "-l", "a.zip");
    TEST(output_has(ctx, 1, "65539 files\n"));
    put16(z.s+z.len-22+10, 4);
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    UNZIP(ctx, 3, "-tqq", "a.zip");
    TEST(output_is(ctx, 3,
        "error:  expected central file header signature not found (file "
        "#65540).\n"
        "  (please check that you have transferred or created the zipfile "
        "in the\n"
        "  appropriate BINARY mode and that you have compiled UnZip "
        "properly)\n"));
    free(z.s);
    free(names);
    free(many);
}

// Faults: a failed write asks whether to go on, and either way the file
// is not kept (50), and one to standard output, however short, ends the
// archive (50); a directory, a file, or an attribute that cannot be
// made is reported, and the rest go on; an archive that shrinks or
// fails to read once extraction began leaves no partial file
static void test_faults(os *ctx)
{
    static char big[200000];
    for (iz i = 0; i < countof(big)-1; i++) {
        big[i] = (char)('a' + ((i64)i*i/7)%26);
    }
    xspec spec[] = {
        {.name="d/big.txt", .data=big, .method=8},
        {.name="d/small.txt", .data="small\n"},
        {.name="e/", .extattr=DIRM(0700)},
        {.name="e/f.txt", .data="f\n", .extattr=FILEM(0600)},
    };
    s8 z = build(spec, countof(spec), 0);

    for (i32 go = 0; go < 2; go++) {
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        ctx->failwriteat = 1000;
        put_stdin(ctx, go ? "y\n" : "n\n");
        UNZIP(ctx, 50, "-q", "a.zip");
        TEST(output_is(ctx, 2, "d/big.txt:  write error (disk full?).  "
                               "Continue? (y/n/^C) "));
        TEST(!mfs_get(ctx, "d/big.txt"));
        TEST(!!mfs_get(ctx, "e/f.txt") == go);
    }

    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->failmkdir = 2;
    ctx->dest = S("out/");
    UNZIP(ctx, 2, "-q", "a.zip", "-d", "out");
    TEST(output_is(ctx, 2,
        "checkdir error:  cannot create out/d\n"
        "                 Permission denied\n"
        "                 unable to process d/big.txt.\n"));
    TEST(!mfs_get(ctx, "out/d/big.txt"));  // made for the next
    TEST(equals(file_data(ctx, "out/d/small.txt"), "small\n"));
    TEST(equals(file_data(ctx, "out/e/f.txt"), "f\n"));

    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->failcreate = 2;
    UNZIP(ctx, 50, "-q", "a.zip");
    TEST(output_is(ctx, 2, "error:  cannot create d/small.txt\n"
                           "        Permission denied\n"));
    TEST(equals(file_data(ctx, "e/f.txt"), "f\n"));

    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->failattrs = OS_AMODE | OS_ATIMES;
    UNZIP(ctx, 1, "a.zip", "e/*");
    TEST(output_is(ctx, 3,
        "Archive:  a.zip\n"
        "   creating: e/\n"
        " extracting: e/f.txt                 fchmod (file attributes) "
        "error: Operation not permitted\n"
        " (warning) cannot set modif./access times\n"
        "          Operation not permitted\n"
        "warning:  cannot set modif./access times for e/\n"
        "          Operation not permitted\n"
        "warning:  cannot set permissions for e/\n"
        "          Operation not permitted\n"
        "warning:  set times/attribs failed for e/\n"
        "     failed setting times/attribs for 1 dir entries"));

    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->failkeep = 1;
    UNZIP(ctx, 50, "-q", "a.zip");
    TEST(output_is(ctx, 2, "d/big.txt:  write error (disk full?).  "
                           "Continue? (y/n/^C) "));
    TEST(!mfs_get(ctx, "d/big.txt"));

    // Standard output that fails, its failure blamed on the entry whose
    // data it held, however short
    for (i32 c = 0; c < 2; c++) {
        char *opt = c ? "-c" : "-p";
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        ctx->failstdout = 1;
        UNZIP(ctx, 50, opt, "a.zip", "d/small.txt", "e/f.txt");
        TEST(output_is(ctx, 2, "d/small.txt:  write error (disk full?).\n"));
        UNZIP(ctx, 50, opt, "a.zip", "d/big.txt");
        TEST(output_is(ctx, 2, "d/big.txt:  write error (disk full?).\n"));
    }

    // The archive cut short, or failing, once extraction has begun
    for (i64 at = 100; at < z.len; at += z.len/7) {
        for (i32 shrink = 0; shrink < 2; shrink++) {
            mfs_reset(ctx);
            put_archive(ctx, "a.zip", z);
            ctx->when = FAULT_CHANGE;
            *(shrink ? &ctx->shrinkto : &ctx->failreadat) = at;
            i32 open   = 0;
            i32 status = unzipos_run(ctx, (char *[]){"-q", "a.zip"}, 2, 0,
                                     &open);
            TEST(!open);
            TEST(status==0 || status==2 || status==3 || status==51);
            for (i32 i = 0; i < countof(spec); i++) {
                mfile *f = mfs_get(ctx, spec[i].name);
                TEST(!f || f->type==FT_DIR ||
                     equals((s8){f->data, f->len}, spec[i].data));
            }
        }
    }
    free(z.s);
}

// Windows conventions, on any host: names mapped for its file systems,
// names in a code page decoded, names that differ in case taken for one,
// attributes rather than modes, without the umask, links made as files
// holding their targets, and -K and -X refused
static void test_windows(os *ctx)
{
    xspec spec[] = {
        {.name="aux.txt", .data="1\n"},
        {.name="a:b<c>|d\"?*.txt", .data="2\n"},
        {.name="dots. . ", .data="3\n"},
        {.name="dir\\sub\\f.txt", .data="4\n", .made=FAT},
        {.name="RO.TXT", .data="5\n", .made=FAT, .extattr=0x21},
        {.name="HIDDEN.SYS", .data="6\n", .made=FAT, .extattr=0x26},
        {.name="link", .data="target", .extattr=LINKM},
        {.name="caf\xe9.txt", .data="7\n", .made=FAT},
        {.name="CASE.txt", .data="8\n"},
        {.name="case.TXT", .data="9\n"},
        {.name="mode.sh", .data="10\n", .extattr=FILEM(0777)},
    };
    s8 z = build(spec, countof(spec), 0);
    mfs_reset(ctx);
    UNZIP(ctx, 0, "-hh");  // elsewhere, '\' escapes
    TEST(output_has(ctx, 1, "[...] a set, and \\ escapes."));
    ctx->windows = 1;
    put_archive(ctx, "a.zip", z);
    UNZIP(ctx, 1, "-o", "a.zip");
    TEST(output_is(ctx, 3,
        "Archive:  a.zip\n"
        " extracting: _aux.txt                \n"
        " extracting: a_b_c__d___.txt         \n"
        " extracting: dots                    \n"
        "warning:  a.zip appears to use backslashes as path separators\n"
        " extracting: dir/sub/f.txt           \n"
        " extracting: RO.TXT                  \n"
        " extracting: HIDDEN.SYS              \n"
        "    linking: link                    -> target \n"
        " extracting: caf\xc3\xa9.txt               \n"  // bytes
        " extracting: CASE.txt                \n"
        " extracting: case.TXT                \n"
        " extracting: mode.sh                 \n"
        "finishing deferred symbolic links:\n"
        "  link                   -> target\n"));
    TEST(equals(file_data(ctx, "_aux.txt"), "1\n"));
    TEST(equals(file_data(ctx, "a_b_c__d___.txt"), "2\n"));
    TEST(equals(file_data(ctx, "dots"), "3\n"));
    TEST(equals(file_data(ctx, "dir/sub/f.txt"), "4\n"));
    TEST(mfs_get(ctx, "RO.TXT")->dosattr == 0x21);
    TEST(mfs_get(ctx, "HIDDEN.SYS")->dosattr == 0x26);
    TEST(mfs_get(ctx, "_aux.txt")->dosattr == 0x20);
    TEST(equals(file_data(ctx, "link"), "target"));
    TEST(equals(file_data(ctx, "caf\xc3\xa9.txt"), "7\n"));
    TEST(equals(file_data(ctx, "case.txt"), "9\n"));
    mfile *f = mfs_get(ctx, "case.txt");
    TEST(equals(f->name, "case.TXT"));
    TEST(mfs_get(ctx, "mode.sh")->perm == 0777);  // no umask

    UNZIP(ctx, 10, "-K", "a.zip");
    UNZIP(ctx, 10, "-X", "a.zip");
    put_stdin(ctx, "n\n");
    UNZIP(ctx, 0, "-q", "a.zip", "CASE.txt");
    TEST(output_is(ctx, 2, "replace CASE.txt? [y]es, [n]o, [A]ll, [N]one, "
                           "[r]ename: "));

    // Member names separate with either slash, so '\' escapes nothing,
    // as the help says
    UNZIP(ctx, 0, "-hh");
    TEST(output_has(ctx, 1, "A \\\nseparates directories, as / does."));
    TEST(!output_has(ctx, 1, "escapes"));
    s8 six = six_files();
    put_archive(ctx, "six.zip", six);
    UNZIP(ctx, 0, "-p", "six.zip", "p\\b.txt");
    TEST(output_is(ctx, 1, "bbb\n"));
    UNZIP(ctx, 0, "-p", "six.zip", "p/*", "-x", "p\\[a-e]*");
    TEST(output_is(ctx, 1, "fff\n"));
    UNZIP(ctx, 11, "-p", "six.zip", "\\p*");

    // A wildcard archive name matches hidden and system files too, as
    // the port's Readdir lists them
    put_archive(ctx, "hid.zip", six);
    put_archive(ctx, "sys.zip", six);
    mfs_get(ctx, "hid.zip")->dosattr = 0x22;
    mfs_get(ctx, "sys.zip")->dosattr = 0x24;
    UNZIP(ctx, 0, "-p", "[hs][iy][ds].zip", "p/a.txt");
    TEST(output_is(ctx, 1, "aaa\naaa\n"));
    free(six.s);

    // Answers typed at a console end in CRLF, the CR dropped
    put_stdin(ctx, "what\r\n\r\nr\r\nren\\y.txt\r\n");
    UNZIP(ctx, 0, "-q", "a.zip", "CASE.txt");
    TEST(output_is(ctx, 2,
        "replace CASE.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        "error:  invalid response [what]\n"
        "replace CASE.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        "error:  invalid response [{ENTER}]\n"
        "replace CASE.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        "new name: "));
    TEST(equals(file_data(ctx, "ren_y.txt"), "8\n"));
    free(z.s);
}

// Times with Windows conventions, as Info-ZIP's port sets them (win32.c,
// getNTfiletime, close_outfile, set_direc_attribs), against POSIX's
// (unix.c, get_extattribs). A local UT field's times count only with
// its modification time, else the DOS time is both modification and
// access time; without its access time, the access time is left
// unchanged; and its creation time is set, which a central UT field
// never holds. All for files and directories, -D skipping them for
// directories and -DD for both, and for a link, extracted as a file
// holding its target, as any file. On POSIX, the field's times count
// alone, the access time defaulting to the modification time, never a
// creation time, and a link gets none.
static void test_wintimes(os *ctx)
{
    xspec spec[] = {
        {.name="d/", .extattr=DIRM(0755),
         .lextra=utf(7, 1500000000, 1500000100, 1400000000)},
        {.name="d/all.txt", .data="a\n",
         .lextra=utf(7, 1600000000, 1600000100, 1300000000)},
        {.name="noatime.txt", .data="b\n",
         .lextra=utf(5, 1600000000, 0, 1200000000)},
        {.name="nomtime.txt", .data="c\n",
         .lextra=utf(6, 0, 1600000100, 1100000000)},
        {.name="central.txt", .data="d\n", .lextra=utl(1600000000, 0),
         .cextra=utf(7, 1600000000, 1600000100, 1000000000)},
        {.name="dos.txt", .data="e\n"},
        {.name="m/", .extattr=DIRM(0755), .lextra=utf(1, 1500000000, 0, 0)},
        {.name="a/", .extattr=DIRM(0755), .lextra=utf(2, 0, 1500000100, 0)},
        {.name="link", .data="target", .extattr=LINKM|1,
         .lextra=utf(1, 1600000000, 0, 0)},
    };
    s8 z = build(spec, countof(spec), 0);
    char *D[] = {"-o", "-oD", "-oDD"};
    for (i32 i = 0; i < 4; i++) {
        mfs_reset(ctx);
        ctx->windows = i < 3;
        put_archive(ctx, "a.zip", z);
        UNZIP(ctx, 0, "-q", D[i%3], "a.zip");
        TEST(output_is(ctx, 3, ""));
        b32 win   = ctx->windows;
        b32 files = i<2 || !win;
        b32 dirs  = i<1 || !win;
        i64 dos   = mfs_get(ctx, "dos.txt")->mtime;
        TEST(files == (dos < 1900000000));

        mfile *f = mfs_get(ctx, "d");
        TEST(dirs&&win ? f->ctime==1400000000 : f->ctime>=1900000000);
        TEST(!dirs || (f->mtime==1500000000 && f->atime==1500000100));
        f = mfs_get(ctx, "m");
        TEST(!dirs || f->mtime==1500000000);
        TEST(!dirs || (win ? f->atime==f->ctime && f->atime>=1900000000
                           : f->atime==1500000000));
        f = mfs_get(ctx, "a");
        TEST(!dirs || (win ? f->mtime==dos && f->atime==dos
                           : f->mtime==dos && f->atime==1500000100));

        f = mfs_get(ctx, "d/all.txt");
        TEST(files&&win ? f->ctime==1300000000 : f->ctime>=1900000000);
        TEST(!files || (f->mtime==1600000000 && f->atime==1600000100));
        f = mfs_get(ctx, "noatime.txt");
        TEST(files&&win ? f->ctime==1200000000 : f->ctime>=1900000000);
        TEST(!files || f->mtime==1600000000);
        TEST(!files || (win ? f->atime>=1900000000 : f->atime==1600000000));
        f = mfs_get(ctx, "nomtime.txt");
        TEST(f->ctime >= 1900000000);
        TEST(!files || (win ? f->mtime==dos && f->atime==dos
                            : f->mtime==dos && f->atime==1600000100));
        TEST(mfs_get(ctx, "central.txt")->ctime >= 1900000000);

        f = mfs_get(ctx, "link");
        if (win) {
            TEST(equals(file_data(ctx, "link"), "target"));
            TEST(f->dosattr == 0x21);
            TEST(files ? f->mtime==1600000000 : f->mtime>=1900000000);
            TEST(f->atime==f->ctime && f->atime>=1900000000);
        } else {
            TEST(f->type==FT_LINK && f->mtime>=1900000000);
        }
    }
    free(z.s);
}

// An archive on standard input, a regular file read in place, or else
// read whole, never overwriting unless -o
// A local name decoded through its Unicode path field, unlike the
// central one, is reported from where it was decoded, not from memory
// the message reuses (a fuzzer's find: AddressSanitizer reported the
// overlapping copy)
#define LONGA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.txt"
#define LONGB "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.txt"
static void test_local_name(os *ctx)
{
    xspec spec[] = {
        {.name=LONGA, .data="a\n", .lextra=upath(LONGA, LONGB)},
    };
    s8 z = build(spec, countof(spec), 0);
    mfs_reset(ctx);
    put_archive(ctx, "n.zip", z);
    UNZIP(ctx, 1, "-o", "n.zip");
    TEST(output_is(ctx, 3,
        "Archive:  n.zip\n"
        LONGA ":  mismatching \"local\" filename (" LONGB "),\n"
        "         continuing with \"central\" filename version\n"
        " extracting: " LONGA "  \n"));
    TEST(equals(file_data(ctx, LONGA), "a\n"));
    free(z.s);

    // A long one, once the file system has changed, shown in the room
    // planned for it
    iz n = 60000;
    s8 x = {malloc((uz)n + 9), n + 9};
    TEST(x.s);
    put16(x.s, 0x7075);
    put16(x.s+2, (u32)n + 5);
    x.s[4] = 1;
    put32(x.s+5, crc32_update(0, (u8 *)"b.txt", 5, &(i32){0}));
    memset(x.s+9, 'b', (uz)n);
    xspec spec2[] = {
        {.name="a.txt", .data="a\n"},
        {.name="b.txt", .data="b\n", .lextra=x},
    };
    z = build(spec2, countof(spec2), 0);
    mfs_reset(ctx);
    put_archive(ctx, "n.zip", z);
    UNZIP(ctx, 1, "-q", "n.zip");
    TEST(output_has(ctx, 2, "b.txt:  mismatching \"local\" filename (bbbb"));
    TEST(equals(file_data(ctx, "a.txt"), "a\n"));
    TEST(equals(file_data(ctx, "b.txt"), "b\n"));
    free(x.s);
    free(z.s);
}

// Memory committed by the last run, perm's and scratch's.
static iz committed(os *ctx)
{
    return (ctx->lo - ctx->mem) + (ctx->mem + ctx->cap - ctx->hi);
}

// Comments of many lines and tabs, the longest an archive's can be, and
// an entry's, are shown in memory in proportion to their length, where
// joining each piece to those before took memory in proportion to its
// square (2 GB for this archive comment)
static void test_comments(os *ctx)
{
    enum { N = 65535 };
    static char comment[N+1];
    static char shown[2*N+1];
    iz len = 0;
    for (iz i = 0; i < N; i++) {
        static char const pattern[] = "\n\ta\x01\r\n\t";
        comment[i] = pattern[i % (countof(pattern)-1)];
        switch (comment[i]) {
        case '\r': break;
        case 0x01: shown[len++] = '^'; shown[len++] = 'A'; break;
        default:   shown[len++] = comment[i];
        }
    }
    xspec spec[] = {
        {.name="a", .data="a\n", .comment={(u8 *)comment, N}},
    };
    s8 z = build(spec, countof(spec), comment);
    mfs_reset(ctx);
    put_archive(ctx, "c.zip", z);
    spec[0].comment = (s8){0};
    s8 plain = build(spec, countof(spec), 0);
    put_archive(ctx, "p.zip", plain);
    UNZIP(ctx, 0, "-l", "p.zip");
    iz base = committed(ctx);  // and each comment's length, three times
    UNZIP(ctx, 0, "-z", "c.zip");
    s8 out = unzipos_output(ctx, 3);
    s8 head = S("Archive:  c.zip\n");
    TEST(out.len == head.len+len+(shown[len-1]!='\n'));
    TEST(s8equals((s8){out.s, head.len}, head));
    TEST(s8equals((s8){out.s+head.len, len}, (s8){(u8 *)shown, len}));
    TEST(committed(ctx) < base + 4*N);
    UNZIP(ctx, 0, "-l", "c.zip");  // both comments
    out = unzipos_output(ctx, 3);
    TEST(out.len > 2*len);
    TEST(committed(ctx) < base + 4*N);
    free(plain.s);
    free(z.s);
}

// The compression factor, -v's, as UnZip computes it, but without
// overflow where UnZip's overflows: for a Zip64 size and a small
// encrypted entry, its compressed size less 12 wrapped, and for a
// compressed size far larger than a small size
static void test_factor(os *ctx)
{
    byte *mem = malloc(1<<12);
    TEST(mem);
    arena a = {mem, mem+(1<<12), ctx, 0};
    TEST(equals(cfactor(&a, 8589930571000, (u64)-1), "-214748364%"));
    TEST(equals(cfactor(&a, 1, 268435457), "-214748364%"));
    TEST(equals(cfactor(&a, 1, (u64)-1), "-214748364%"));
    TEST(equals(cfactor(&a, (u64)-1, 1), "100%"));
    TEST(equals(cfactor(&a, 0, 5), " 0%"));
    TEST(equals(cfactor(&a, 1000, 0), "100%"));
    TEST(equals(cfactor(&a, 1000, 1000), " 0%"));
    TEST(equals(cfactor(&a, 1000, 1999), "100%"));  // as UnZip shows it
    TEST(equals(cfactor(&a, 3000000, 1000000), " 67%"));
    TEST(equals(cfactor(&a, 3000000, 9000000), "-200%"));
    // and as UnZip's arithmetic, where that does not overflow
    u64 rng = 1;
    for (i32 i = 0; i < 100000; i++) {
        rng = rng*0x3243f6a8885a308d + 1;
        u64 uc = (rng >> 20) >> (rng & 31);
        u64 c  = (rng >> 30) % (uc*(i&1 ? 2 : 1000) + 2);
        i64 f  = 0;
        if (!uc) {
            f = 0;
        } else if (uc > 2000000) {
            u64 denom = uc / 1000;
            f = uc>=c ? (i64)((uc-c + (denom>>1)) / denom)
                      : -(i64)((c-uc + (denom>>1)) / denom);
        } else {
            f = uc>=c ? (i64)((1000*(uc-c) + (uc>>1)) / uc)
                      : -(i64)((1000*(c-uc) + (uc>>1)) / uc);
        }
        char want[32];
        char sgn = f<0 ? '-' : ' ';
        f = f<0 ? (-f + 5)/10 : (f + 5)/10;
        if (f == 100) {
            snprintf(want, sizeof(want), "100%%");
        } else {
            snprintf(want, sizeof(want), "%c%lld%%", sgn, (long long)f);
        }
        arena tmp = a;
        TEST(equals(cfactor(&tmp, uc, c), want));
    }
    free(mem);
}

// An entry whose offset lies past the end of the file is listed, and
// once read, reported as UnZip reports it, the others read: at its
// header ("EOF"), or at the 8 KiB block that holds it ("lseek").
static void test_past(os *ctx)
{
    for (i32 far = 0; far < 2; far++) {
        xspec spec[] = {
            {.name="a.txt", .data=TEXT, .method=8},
            {.name="b.txt", .data="bbb\n", .offset=(i64)1<<30},
            {.name="c.txt", .data=TEXT},
        };
        s8  z = build(spec, countof(spec), 0);
        u8 *b = z.s + get32(z.s+z.len-22+16) + 46+5;  // the second's header
        put32(b+42, far ? 0xfffffffe : (u32)z.len+100);
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        UNZIP(ctx, 0, "-l", "a.zip");
        TEST(output_has(ctx, 1, "3 files\n"));
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "file #2:  bad zipfile offset (%s):  %lld\n",
                 far ? "lseek" : "EOF",
                 far ? 0xffffe000ll : (long long)z.len+100);
        UNZIP(ctx, 3, "-t", "a.zip");
        TEST(output_has(ctx, 1, "    testing: a.txt                    OK\n"));
        TEST(output_has(ctx, 1, msg));
        TEST(output_has(ctx, 1, "    testing: c.txt                    OK\n"));
        ctx->dest = S("out/");
        UNZIP(ctx, 3, "-q", "-d", "out", "a.zip");
        TEST(output_is(ctx, 2, msg));
        TEST(mfs_get(ctx, "out/a.txt") && mfs_get(ctx, "out/c.txt"));
        TEST(!mfs_get(ctx, "out/b.txt"));
        free(z.s);
    }
}

// Offsets that a writer without Zip64 wrapped past 4 GiB, unwrapped as
// 7-Zip unwraps them: each the least past where the entry before it
// ends, at least, its 32-bit offset modulo 4 GiB. (unzip.sh reads such
// an archive, 4.5 GiB and sparse, too large here.)
static void test_unwrap(os *ctx)
{
    (void)ctx;
    i64 g = (i64)1 << 32;
    TEST(zar_unwrap(0, 0) == 0);
    TEST(zar_unwrap(100, 50) == 100);
    TEST(zar_unwrap(50, 50) == 50);
    TEST(zar_unwrap(49, 50) == g+49);
    TEST(zar_unwrap(536870979, 4831838275) == 4831838275);
    TEST(zar_unwrap(536870980, 4831838275) == 4831838276);
    TEST(zar_unwrap(536870978, 4831838275) == 4831838274+g);
    TEST(zar_unwrap(g-1, 3*g) == 4*g-1);
    TEST(zar_unwrap(g+5, 3*g) == g+5);  // from Zip64, as it is
    TEST(zar_unwrap(g-1, ZAR_MAXOFF) == ZAR_MAXOFF+g-1);

    zentry e = {.offset=2415919137, .csize=2415919106};
    TEST(zar_floor(&e) == 2415919137 + 30 + 2415919106);
    e = (zentry){.offset=ZAR_MAXOFF-31, .csize=1};
    TEST(zar_floor(&e) == ZAR_MAXOFF);
    e.csize = 2;
    TEST(zar_floor(&e) == ZAR_MAXOFF);
    e = (zentry){.offset=ZAR_MAXOFF+g, .csize=(i64)((u64)-1>>1)};
    TEST(zar_floor(&e) == ZAR_MAXOFF);
}

// Data decoding past an entry's size is all of it where a writer without
// Zip64 wrapped that size to 32 bits: by 4 GiB or a multiple of it, the
// size from its 32-bit field, the CRC right, and not stored (whose data
// is its compressed size). (unzip.sh decodes 5 GiB, too much here.)
static void test_wrapped(os *ctx)
{
    (void)ctx;
    i64    g = (i64)1 << 32;
    zentry e = {.method=ZIP_DEFLATE, .crc=0x2a1847ff};
    TEST(size_wrapped(&e, 1073754169, 5368721465, 0x2a1847ff));
    TEST(size_wrapped(&e, 0, g, 0x2a1847ff));
    TEST(size_wrapped(&e, 0, 3*g, 0x2a1847ff));
    TEST(size_wrapped(&e, g-1, 2*g-1, 0x2a1847ff));
    TEST(!size_wrapped(&e, 10, 10, 0x2a1847ff));         // not past it
    TEST(!size_wrapped(&e, 10, 20, 0x2a1847ff));         // an overrun
    TEST(!size_wrapped(&e, 10, g+11, 0x2a1847ff));       // not congruent
    TEST(!size_wrapped(&e, 10, g+10, 0x12345678));       // a bad CRC
    TEST(!size_wrapped(&e, g+10, 2*g+10, 0x2a1847ff));   // not 32 bits
    e.usize64 = 1;
    TEST(!size_wrapped(&e, 10, g+10, 0x2a1847ff));       // from Zip64
    e.usize64 = 0;
    e.method  = ZIP_STORE;
    TEST(!size_wrapped(&e, 10, g+10, 0x2a1847ff));
}

static void test_stdin(os *ctx)
{
    s8 z = six_files();
    for (i32 file = 0; file < 2; file++) {
        mfs_reset(ctx);
        mfs_put(ctx, "p", FT_DIR, 0, 0, 0);
        u64 a = mfs_put(ctx, "p/a.txt", FT_FILE, "old", 3, 1)->id;
        ctx->in        = z.s;
        ctx->inlen     = z.len;
        ctx->stdinfile = file;
        ctx->archive   = "-";
        UNZIP(ctx, 0, "-q", "-");
        TEST(same_file(ctx, "p/a.txt", a));
        TEST(equals(file_data(ctx, "p/f.txt"), "fff\n"));
        UNZIP(ctx, 0, "-qo", "-");
        TEST(!same_file(ctx, "p/a.txt", a));
        TEST(equals(file_data(ctx, "p/a.txt"), "aaa\n"));
        UNZIP(ctx, 0, "-p", "-", "p/b.txt", "p/c.txt");
        TEST(output_is(ctx, 3, "bbb\nccc\n"));
    }
    free(z.s);
}

// Output to a terminal, otherwise held until full as to a pipe, is
// flushed at a check point (between entries, and chunks of their data)
// once it has waited 100 ms since the buffer began to fill, the clock
// read only for a terminal with text waiting; the bytes are the same
static void test_pace(os *ctx)
{
    s8 z = six_files();
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    UNZIP(ctx, 0, "-o", "a.zip");
    s8 want = unzipos_output(ctx, 1);
    want.s = memcpy(malloc((uz)want.len), want.s, (uz)want.len);
    TEST(ctx->nwrites==1 && !ctx->clock.reads);

    // Not a terminal: no clock, however slow
    ctx->clock.tick = 1000;
    UNZIP(ctx, 0, "-o", "a.zip");
    TEST(ctx->nwrites==1 && !ctx->clock.reads);
    TEST(s8equals(unzipos_output(ctx, 1), want));

    // A terminal, fast: the clock read, nothing flushed early
    ctx->tty[1]     = 1;
    ctx->clock.tick = 0;
    UNZIP(ctx, 0, "-o", "a.zip");
    TEST(ctx->nwrites==1 && ctx->clock.reads);
    TEST(s8equals(unzipos_output(ctx, 1), want));

    // Slow: flushed now and then, a flush taking at least two readings,
    // when the buffer begins to fill and at a check point
    for (i64 tick = 50; tick <= 100; tick += 50) {
        ctx->clock.tick = tick;
        UNZIP(ctx, 0, "-o", "a.zip");
        TEST(s8equals(unzipos_output(ctx, 1), want));
        TEST(ctx->nwrites > 1);
        TEST(ctx->nwrites <= ctx->clock.reads/2 + 1);
    }

    // Listing and testing, an entry at a time
    UNZIP(ctx, 0, "-l", "a.zip");
    TEST(ctx->nwrites > 1);
    UNZIP(ctx, 0, "-t", "a.zip");
    TEST(ctx->nwrites > 1);

    // Messages are held for a signal to write, but not entries' data
    UNZIP(ctx, 0, "-o", "a.zip");
    TEST(ctx->washeld);
    UNZIP(ctx, 0, "-t", "a.zip");
    TEST(ctx->washeld);
    UNZIP(ctx, 0, "-p", "a.zip");
    TEST(!ctx->washeld);
    UNZIP(ctx, 0, "-c", "a.zip");
    TEST(!ctx->washeld);
    free(z.s);

    // One small stored entry: the clock read as "Archive:" begins to
    // fill the buffer, then at two check points, before the entry and in
    // its data, so 49 ms apart nothing is flushed early, 50 ms apart its
    // line is, unfinished, before its data is written
    xspec one[] = {{.name="f", .data="f\n"}};
    z = build(one, countof(one), 0);
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->tty[1]     = 1;
    ctx->clock.tick = 49;
    UNZIP(ctx, 0, "-o", "a.zip");
    TEST(ctx->clock.reads==3 && ctx->nwrites==1);
    ctx->clock.tick = 50;
    UNZIP(ctx, 0, "-o", "a.zip");
    TEST(ctx->clock.reads==4 && ctx->nwrites==2);  // and its "\n"
    TEST(ctx->writes[0].end == (iz)strlen(
        "Archive:  a.zip\n extracting: f                       "));
    TEST(ctx->writes[0].written == 0);
    TEST(output_is(ctx, 1, "Archive:  a.zip\n"
                           " extracting: f                       \n"));
    free(z.s);

    // A large entry: its line is flushed during its data once the clock
    // has advanced, and its data, under -c, likewise, in pieces, but
    // never under -p, which is data alone
    iz    len  = (iz)1 << 20;
    char *data = malloc((uz)len);
    TEST(data);
    u32 rng = 1;
    for (iz i = 0; i < len; i++) {
        rng = rng*1103515245u + 12345u;
        data[i] = (char)('a' + (rng>>16)%26);
    }
    xspec big[] = {{.name="big", .data=data, .datalen=(i32)len, .method=8}};
    z = build(big, countof(big), 0);
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->tty[1]     = 1;
    ctx->clock.tick = 40;
    UNZIP(ctx, 0, "-o", "a.zip");
    TEST(file_data(ctx, "big").len == len);
    TEST(ctx->nwrites == 2);
    TEST(ctx->writes[0].end == (iz)strlen(
        "Archive:  a.zip\n  inflating: big                     "));
    TEST(ctx->writes[0].written == UZ_WSIZE);  // the first window only
    TEST(output_is(ctx, 1, "Archive:  a.zip\n"
                           "  inflating: big                     \n"));

    UNZIP(ctx, 0, "-c", "a.zip");
    TEST(ctx->nwrites > 2);
    s8 out = unzipos_output(ctx, 1);
    s8 head = S("Archive:  a.zip\n  inflating: big                     \n");
    TEST(out.len == head.len+len+1);
    TEST(s8equals((s8){out.s, head.len}, head));
    TEST(!memcmp(out.s+head.len, data, (uz)len));

    UNZIP(ctx, 0, "-p", "a.zip");
    TEST(!ctx->clock.reads);
    TEST(unzipos_output(ctx, 1).len == len);
    free(z.s);
    free(data);
    free(want.s);
}

// A central extended timestamp ("UT") with a modification time
static s8 utc(u32 mtime)
{
    static u8 buf[4][9];
    static i32 next;
    u8 *p = buf[next++ % countof(buf)];
    put16(p, 0x5455);
    put16(p+2, 5);
    p[4] = 1;
    put32(p+5, mtime);
    return (s8){p, 9};
}

#define ZFIRST "error:  -Z must be first option for ZipInfo mode " \
               "(check UNZIP variable?)\n"

// ZipInfo mode: chosen by the program's name or a first argument of -Z,
// its environment, its options' header, totals, and comment, its
// listings, statuses, and messages, those for standard error going to
// standard output while totals show; memory independent of the number
// of entries
static void test_zipinfo(os *ctx)
{
    xspec spec[] = {
        {.name="d/", .extattr=DIRM(0755)},
        {.name="d/a.txt", .data=TEXT, .method=8, .intattr=1},
        {.name="B.EXE", .data="bb", .made=FAT, .extattr=0x21},
        {.name="u.txt", .data="ut\n", .cextra=utc(1600000000)},
        {.name="enc", .data="0123456789abcdef", .flags=1,
         .extattr=FILEM(04755)},
    };
    s8 z = build(spec, countof(spec), "A comment\n");
    s8 empty = build(spec, 0, 0);
    s8 bad   = build(spec+1, 3, 0);
    for (iz i = bad.len-4, n = 0; i >= 0; i--) {  // the second header
        if (!memcmp(bad.s+i, "PK\1\2", 4) && ++n==2) {
            bad.s[i+3] = 9;
            break;
        }
    }
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    put_archive(ctx, "empty.zip", empty);
    put_archive(ctx, "bad.zip", bad);
    mfs_put(ctx, "notzip", FT_FILE, "not a zip file\n", 15, 1600000000);


    // Formats: the header, entries, totals, and comment by option
    ZIPINFO(ctx, 0, "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-1", "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-2", "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-2htz", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "A comment\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-1z", "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-m", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b-  0% stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- 87% defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b-  0% stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx  0% stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- 75% stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-l", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b-        0 stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t-       13 defN 20-Jan-02 "
        "03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b-        2 stor 20-Jan-02 "
        "03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx        3 stor 20-Sep-13 "
        "12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B-       16 stor 20-Jan-02 "
        "03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-T", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b- stor 20200102.030406 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20200102.030406 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20200102.030406 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20200913.122640 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20200102.030406 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-z", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "A comment\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-h", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-t", "a.zip");
    TEST(output_is(ctx, 1, "5 files, 117 bytes uncompressed, 22 bytes "
                           "compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-hz", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "A comment\n"
        "Zip file size: 499 bytes, number of entries: 5\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "--h", "a.zip");
    TEST(output_is(ctx, 1,
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "--t", "-l", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b-        0 stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t-       13 defN 20-Jan-02 "
        "03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b-        2 stor 20-Jan-02 "
        "03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx        3 stor 20-Sep-13 "
        "12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B-       16 stor 20-Jan-02 "
        "03:04 enc\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-l-t", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b-        0 stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t-       13 defN 20-Jan-02 "
        "03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b-        2 stor 20-Jan-02 "
        "03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx        3 stor 20-Sep-13 "
        "12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B-       16 stor 20-Jan-02 "
        "03:04 enc\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "--", "-h", "a.zip");
    TEST(output_is(ctx, 1,
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    // Members turn off the header and totals unless asked for
    ZIPINFO(ctx, 0, "a.zip", "*.txt");
    TEST(output_is(ctx, 1,
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-lz", "a.zip", "*.txt");
    TEST(output_is(ctx, 1,
        "A comment\n"
        "-rw-r--r--  3.0 unx       96 t-       13 defN 20-Jan-02 "
        "03:04 d/a.txt\n"
        "-rw-r--r--  3.0 unx        3 bx        3 stor 20-Sep-13 "
        "12:26 u.txt\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-t", "a.zip", "*.txt");
    TEST(output_is(ctx, 1,
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "2 files, 99 bytes uncompressed, 16 bytes compressed:  83.8%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-C", "a.zip", "D/*");
    TEST(output_is(ctx, 1,
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 11, "a.zip", "D/*");
    TEST(output_is(ctx, 2, "caution: filename not matched:  D/*\n"));
    TEST(output_is(ctx, 1, ""));
    ZIPINFO(ctx, 0, "a.zip", "-x", "d/*");
    TEST(output_is(ctx, 1,
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-h", "a.zip", "-x", "d/*", "-x");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"));
    TEST(output_is(ctx, 2, "caution: excluded filename not matched:  -x\n"));
    ZIPINFO(ctx, 0, "a.zip", "-d", "out");
    TEST(output_is(ctx, 1,
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"));
    TEST(output_is(ctx, 2, "caution:  not extracting; -d ignored\n"));
    ZIPINFO(ctx, 0, "-t", "a.zip", "-dout");
    TEST(output_is(ctx, 1,
        "caution:  not extracting; -d ignored\n"
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 10, "-t", "a.zip", "-d");
    TEST(output_is(ctx, 1,
        "error:  must specify directory to which to extract with -d "
        "option\n"));
    TEST(output_is(ctx, 2, ""));
    // Statuses: unmatched members (11 if none matched), messages routed by
    // the totals flag
    ZIPINFO(ctx, 11, "a.zip", "nomatch");
    TEST(output_is(ctx, 2, "caution: filename not matched:  nomatch\n"));
    TEST(output_is(ctx, 1, ""));
    ZIPINFO(ctx, 11, "-t", "a.zip", "nomatch");
    TEST(output_is(ctx, 1,
        "0 files, 0 bytes uncompressed, 0 bytes compressed:  0.0%\n"
        "caution: filename not matched:  nomatch\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-h", "a.zip", "enc", "nomatch");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"));
    TEST(output_is(ctx, 2, "caution: filename not matched:  nomatch\n"));
    ZIPINFO(ctx, 0, "-st", "a.zip", "-x", "nomatch");
    TEST(output_is(ctx, 1,
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"
        "caution: excluded filename not matched:  nomatch\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 11, "a.zip", "-x", "*");
    TEST(output_is(ctx, 1, ""));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 9, "nothere");
    TEST(output_is(ctx, 1,
        "zipinfo:  cannot find or open nothere, nothere.zip or "
        "nothere.ZIP.\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 9, "-1", "nothere");
    TEST(output_is(ctx, 2,
        "zipinfo:  cannot find or open nothere, nothere.zip or "
        "nothere.ZIP.\n"));
    TEST(output_is(ctx, 1, ""));
    ZIPINFO(ctx, 9, "-h", "notzip");
    TEST(output_is(ctx, 1, "Archive:  notzip\n"));
    TEST(output_is(ctx, 2,
        "[notzip]\n"
        "  End-of-central-directory signature not found.  Either "
        "this file is not\n"
        "  a zipfile, or it constitutes one disk of a multi-part "
        "archive.  In the\n"
        "  latter case the central directory and zipfile comment "
        "will be found on\n"
        "  the last disk(s) of this archive.\n"
        "zipinfo:  cannot find zipfile directory in one of notzip or\n"
        "          notzip.zip, and cannot find notzip.ZIP, period.\n"));
    ZIPINFO(ctx, 9, "-2", "notzip");
    TEST(output_is(ctx, 2,
        "[notzip]\n"
        "  End-of-central-directory signature not found.  Either "
        "this file is not\n"
        "  a zipfile, or it constitutes one disk of a multi-part "
        "archive.  In the\n"
        "  latter case the central directory and zipfile comment "
        "will be found on\n"
        "  the last disk(s) of this archive.\n"
        "zipinfo:  cannot find zipfile directory in one of notzip or\n"
        "          notzip.zip, and cannot find notzip.ZIP, period.\n"));
    TEST(output_is(ctx, 1, ""));
    ZIPINFO(ctx, 1, "empty.zip");
    TEST(output_is(ctx, 1,
        "Archive:  empty.zip\n"
        "Zip file size: 22 bytes, number of entries: 0\n"
        "Empty zipfile.\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 1, "-1", "empty.zip");
    TEST(output_is(ctx, 1, "Empty zipfile.\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 3, "-t", "bad.zip");
    TEST(output_is(ctx, 1,
        "error:  expected central file header signature not found "
        "(file #2).\n"
        "  (please check that you have transferred or created the "
        "zipfile in the\n"
        "  appropriate BINARY mode and that you have compiled UnZip "
        "properly)\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 3, "-1", "bad.zip");
    TEST(output_is(ctx, 1, "d/a.txt\n"));
    TEST(output_is(ctx, 2,
        "error:  expected central file header signature not found "
        "(file #2).\n"
        "  (please check that you have transferred or created the "
        "zipfile in the\n"
        "  appropriate BINARY mode and that you have compiled UnZip "
        "properly)\n"));
    ZIPINFO(ctx, 11, "-h", "a.zip", "bad.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"));
    TEST(output_is(ctx, 2, "caution: filename not matched:  bad.zip\n"));
    ZIPINFO(ctx, 3, "*.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"
        "\n"
        "Archive:  bad.zip\n"
        "Zip file size: 311 bytes, number of entries: 3\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "error:  expected central file header signature not found "
        "(file #2).\n"
        "  (please check that you have transferred or created the "
        "zipfile in the\n"
        "  appropriate BINARY mode and that you have compiled UnZip "
        "properly)\n"
        "\n"
        "Archive:  empty.zip\n"
        "Zip file size: 22 bytes, number of entries: 0\n"
        "Empty zipfile.\n"
        "\n"
        "1 archive was successfully processed.\n"
        "1 archive had warnings but no fatal errors.\n"
        "1 archive had fatal errors.\n"));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 3, "-1", "*.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"
        "\n"
        "d/a.txt\n"
        "\n"
        "Empty zipfile.\n"));
    TEST(output_is(ctx, 2,
        "error:  expected central file header signature not found "
        "(file #2).\n"
        "  (please check that you have transferred or created the "
        "zipfile in the\n"
        "  appropriate BINARY mode and that you have compiled UnZip "
        "properly)\n"
        "\n"
        "1 archive was successfully processed.\n"
        "1 archive had warnings but no fatal errors.\n"
        "1 archive had fatal errors.\n"));
    ZIPINFO(ctx, 10, "-q", "a.zip");
    TEST(output_is(ctx, 1, ""));
    TEST(output_has(ctx, 2, "tugz zipinfo "));
    ZIPINFO(ctx, 10, "-x", "a.zip");
    TEST(output_is(ctx, 1, ""));
    TEST(output_has(ctx, 2, "tugz zipinfo "));
    expect(ctx, 0, &(unzipenv){.argv0="zipinfo"}, 0, 0);  // no arguments
    TEST(output_has(ctx, 1, "tugz zipinfo "));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-h");
    TEST(output_has(ctx, 1, "tugz zipinfo "));
    TEST(output_is(ctx, 2, ""));
    ZIPINFO(ctx, 0, "-1");
    TEST(output_has(ctx, 1, "tugz zipinfo "));
    TEST(output_is(ctx, 2, ""));
    // By name, and by a first argument of -Z
    RUNENV(ctx, 0, (.argv0="ii"), "-1", "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"));
    TEST(output_is(ctx, 2, ""));
    UNZIP(ctx, 0, "-Z1", "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"));
    TEST(output_is(ctx, 2, ""));
    UNZIP(ctx, 0, "-Z", "-2t", "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    UNZIP(ctx, 0, "-Z");
    TEST(output_has(ctx, 1, "tugz zipinfo "));
    TEST(output_is(ctx, 2, ""));
    UNZIP(ctx, 10, "-lZ", "a.zip");
    TEST(output_is(ctx, 1, ""));
    TEST(output_has(ctx, 2, "tugz unzip "));
    TEST(output_has(ctx, 2, ZFIRST));
    UNZIP(ctx, 10, "-tZ", "a.zip");
    TEST(output_has(ctx, 1, "tugz unzip "));
    TEST(output_has(ctx, 1, ZFIRST));
    TEST(output_is(ctx, 2, ""));
    UNZIP(ctx, 0, "-hZ", "a.zip");
    TEST(output_has(ctx, 1, "tugz unzip "));
    TEST(output_is(ctx, 2, ZFIRST));
    UNZIP(ctx, 11, "-l", "a.zip", "-Z");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "A comment\n"
        "  Length      Date    Time    Name\n"
        "---------  ---------- -----   ----\n"
        "---------                     -------\n"
        "        0                     0 files\n"));
    TEST(output_is(ctx, 2, ""));
    // The environment: ZIPINFO, else ZIPINFOOPT, first; UNZIP ignored
    RUNENV(ctx, 0, (.argv0="zipinfo", .zipinfo="-1"), "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"));
    TEST(output_is(ctx, 2, ""));
    RUNENV(ctx, 0, (.argv0="zipinfo", .zipinfo=" ", .zipinfoopt="-2t"),
           "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    RUNENV(ctx, 0, (.argv0="zipinfo", .zipinfo="-1", .zipinfoopt="-2t"),
           "--1", "-h", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"
        "drwxr-xr-x  3.0 unx        0 b- stor 20-Jan-02 03:04 d/\n"
        "-rw-r--r--  3.0 unx       96 t- defN 20-Jan-02 03:04 d/a.txt\n"
        "-r-xa--     2.0 fat        2 b- stor 20-Jan-02 03:04 B.EXE\n"
        "-rw-r--r--  3.0 unx        3 bx stor 20-Sep-13 12:26 u.txt\n"
        "-rwsr-xr-x  3.0 unx       16 B- stor 20-Jan-02 03:04 enc\n"
        "5 files, 117 bytes uncompressed, 22 bytes compressed:  81.2%\n"));
    TEST(output_is(ctx, 2, ""));
    RUNENV(ctx, 0, (.argv0="zipinfo", .unzip="-l"), "-1", "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"));
    TEST(output_is(ctx, 2, ""));
    RUNENV(ctx, 0, (.zipinfo="-1"), "-Z", "a.zip");
    TEST(output_is(ctx, 1,
        "d/\n"
        "d/a.txt\n"
        "B.EXE\n"
        "u.txt\n"
        "enc\n"));
    TEST(output_is(ctx, 2, ""));
    RUNENV(ctx, 0, (.zipinfo="-1"), "-l", "a.zip", "enc");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "A comment\n"
        "  Length      Date    Time    Name\n"
        "---------  ---------- -----   ----\n"
        "       16  2020-01-02 03:04   enc\n"
        "---------                     -------\n"
        "       16                     1 file\n"));
    TEST(output_is(ctx, 2, ""));
    RUNENV(ctx, 10, (.unzip="-Z"), "-l", "a.zip");
    TEST(output_is(ctx, 1, ""));
    TEST(output_has(ctx, 2, "tugz unzip "));
    TEST(output_has(ctx, 2, ZFIRST));
    RUNENV(ctx, 0, (.unzip="-Z1"), "-Z", "-h", "a.zip");
    TEST(output_is(ctx, 1,
        "Archive:  a.zip\n"
        "Zip file size: 499 bytes, number of entries: 5\n"));
    TEST(output_is(ctx, 2, ""));

    // ZipInfo's letters that tugz refuses: M, its pager, and U, Debian's
    // Unicode escapes
    ZIPINFO(ctx, 10, "-M", "a.zip");
    TEST(output_is(ctx, 3, "error:  -M option not supported\n"));
    TEST(output_is(ctx, 2, "error:  -M option not supported\n"));
    ZIPINFO(ctx, 10, "-hU", "a.zip");
    TEST(output_is(ctx, 2, "error:  -U option not supported\n"));
    ZIPINFO(ctx, 0, "--v", "-1", "a.zip");
    TEST(output_is(ctx, 3, "d/\nd/a.txt\nB.EXE\nu.txt\nenc\n"));

    // An entry without a name ends the listing, as for unzip -l, where
    // UnZip shows the name before it again
    xspec noname[] = {{.name="a", .data="a"}, {.name="", .data="b"},
                      {.name="c", .data="c"}};
    s8 nz = build(noname, countof(noname), 0);
    put_archive(ctx, "noname.zip", nz);
    ZIPINFO(ctx, 3, "-1", "noname.zip");
    TEST(output_is(ctx, 1, "a\n"));
    TEST(output_is(ctx, 2, ":  bad filename length (central)\n"));
    free(nz.s);

    // While totals show, messages for standard error go to standard
    // output, and with that alone redirected, to standard error too, as
    // under unzip -t
    ctx->tty[2] = 1;
    ZIPINFO(ctx, 11, "-t", "a.zip", "nomatch");
    TEST(output_is(ctx, 1, "0 files, 0 bytes uncompressed, 0 bytes "
                           "compressed:  0.0%\n"
                           "caution: filename not matched:  nomatch\n"));
    TEST(output_is(ctx, 2, "caution: filename not matched:  nomatch\n"));
    ZIPINFO(ctx, 11, "-1", "a.zip", "nomatch");
    TEST(output_is(ctx, 1, ""));
    TEST(output_is(ctx, 2, "caution: filename not matched:  nomatch\n"));
    ctx->tty[2] = 0;

    // Memory is the same for 20,000 entries as for one
    enum { N = 20000 };
    xspec *many  = calloc(N, sizeof(*many));
    char (*names)[8] = calloc(N, sizeof(*names));
    TEST(many && names);
    for (i32 i = 0; i < N; i++) {
        snprintf(names[i], sizeof(names[i]), "%05d", i);
        many[i] = (xspec){.name=names[i], .data="x"};
    }
    s8 one = build(many, 1, 0);
    s8 all = build(many, N, 0);
    put_archive(ctx, "one.zip", one);
    put_archive(ctx, "all.zip", all);
    ZIPINFO(ctx, 0, "-l", "one.zip");
    iz base = committed(ctx);
    ZIPINFO(ctx, 0, "-l", "all.zip");
    TEST(output_has(ctx, 1, "\n20000 files, 20000 bytes uncompressed"));
    TEST(committed(ctx) < base+64);  // (the totals' longer numbers)
    ZIPINFO(ctx, 0, "-1", "one.zip", "0*", "-x", "*7");
    base = committed(ctx);
    ZIPINFO(ctx, 0, "-1", "all.zip", "0*", "-x", "*7");
    TEST(output_has(ctx, 1, "\n09996\n09998\n09999\n"));
    TEST(committed(ctx) <= base);
    free(all.s);
    free(one.s);
    free(names);
    free(many);

    // Windows conventions: the program's name after a '\', and members
    // separated by '\' too
    ctx->windows = 1;
    RUNENV(ctx, 0, (.argv0="C:\\bin\\ZipInfo.exe"), "-1", "a.zip",
           "d\\a.txt");
    TEST(output_is(ctx, 3, "d/a.txt\n"));
    RUNENV(ctx, 0, (.argv0="C:\\zipinfo\\unzip.exe"), "-Z1", "a.zip", "B*");
    TEST(output_is(ctx, 3, "B.EXE\n"));
    RUNENV(ctx, 0, (.argv0="C:\\zipinfo\\unzip.exe"), "-qq", "-l", "a.zip",
           "B*");
    TEST(output_is(ctx, 3, "        2  2020-01-02 03:04   B.EXE\n"));
    ctx->windows = 0;
    free(bad.s);
    free(empty.s);
    free(z.s);
}

// An archive from build, after prefix, its offsets adjusted for it: as
// the first part of a split archive begins with a signature.
static s8 shifted(s8 z, char const *prefix)
{
    mbuf b = {0};
    iz   n = (iz)strlen(prefix);
    zput(&b, prefix, n);
    zput(&b, z.s, z.len);
    u8 *p   = b.s + n;
    iz  end = z.len - 22;
    for (; get32(p+end) != ZIP_END_SIG; end--) {}
    u32 cdoff = get32(p+end+16);
    put32(p+end+16, cdoff + (u32)n);
    for (iz i = cdoff; get32(p+i) == ZIP_CENTRAL_SIG;) {
        put32(p+i+42, get32(p+i+42) + (u32)n);
        i += 46 + get16(p+i+28) + get16(p+i+30) + get16(p+i+32);
    }
    return (s8){b.s, b.len};
}

// ZipInfo's verbose report, -v: its sections on the archive comment and
// the end records, each entry's fields, subfields, and comment, and data
// between entries by their offsets as recorded, carried to the next
// archive from offset 4; archives empty, with data before them and Zip64
// records, or with a comment cut short; the error for a subfield cut
// short to standard error, but under totals; and memory bounded with a
// 64 KiB extra field, and independent of the number of entries. Outputs
// as Info-ZIP's zipinfo gives them.
static void test_zipinfo_verbose(os *ctx)
{
    xspec spec[] = {
        {.name="a.txt", .data="a\n", .cextra=utc(1600000000),
         .comment=S("an entry comment")},
        {.name="b", .data="bb", .lextra=owner(1, 2)},
        {.name="c", .data="c", .made=FAT, .extattr=0x21},
        {.name="d", .data="d", .cextra={(u8 *)"UT\5\0\1\0\0\0\0"
                                        "4\x12,\1abc", 16}},
    };
    s8 z     = build(spec, 3, "A comment\n");
    s8 bare  = build(spec, 3, 0);
    s8 p64   = zip64_after(bare, "#!/bin/sh\n");
    s8 empty = build(spec, 0, 0);
    s8 cut   = {malloc((uz)z.len), z.len};
    TEST(cut.s);
    bytecopy(cut.s, z.s, z.len);
    put16(cut.s+z.len-12, 15);  // its comment's length, past the end
    s8 sub   = build(spec+3, 1, 0);
    s8 one   = build(spec+2, 1, 0);
    s8 split = shifted(one, "PK\7\x08");
    mfs_reset(ctx);
    put_archive(ctx, "v.zip", z);
    put_archive(ctx, "p64.zip", p64);
    put_archive(ctx, "empty.zip", empty);
    put_archive(ctx, "cut.zip", cut);
    put_archive(ctx, "sub.zip", sub);
    mfs_put(ctx, "w", FT_DIR, 0, 0, 1600000000);
    put_archive(ctx, "w/a.zip", split);
    put_archive(ctx, "w/b.zip", z);

    // Every section: an entry with a timestamp and a comment, one whose
    // local header is longer than its central header tells, its owner
    // field (the next is 15 bytes later than expected), one from MS-DOS
    ZIPINFO(ctx, 0, "-v", "v.zip");
    TEST(output_is(ctx, 1,
        "Archive:  v.zip\n"
        "The zipfile comment is 10 bytes long and contains the following "
        "text:\n"
        "======================== zipfile comment begins "
        "==========================\n"
        "A comment\n"
        "========================= zipfile comment ends "
        "===========================\n"
        "\n"
        "End-of-central-directory record:\n"
        "-------------------------------\n"
        "\n"
        "  Zip archive file size:                       319 "
        "(000000000000013Fh)\n"
        "  Actual end-cent-dir record offset:           287 "
        "(000000000000011Fh)\n"
        "  Expected end-cent-dir record offset:         287 "
        "(000000000000011Fh)\n"
        "  (based on the length of the central directory and its expected "
        "offset)\n"
        "\n"
        "  This zipfile constitutes the sole disk of a single-part archive; "
        "its\n"
        "  central directory contains 3 entries.\n"
        "  The central directory is 170 (00000000000000AAh) bytes long,\n"
        "  and its (expected) offset in bytes from the beginning of the "
        "zipfile\n"
        "  is 117 (0000000000000075h).\n"
        "\n"
        "\n"
        "Central directory entry #1:\n"
        "---------------------------\n"
        "\n"
        "  a.txt\n"
        "\n"
        "  offset of local header from start of archive:   0\n"
        "                                                  (0000000000000000h) "
        "bytes\n"
        "  file system or operating system of origin:      Unix\n"
        "  version of encoding software:                   3.0\n"
        "  minimum file system compatibility required:     MS-DOS, OS/2 or NT "
        "FAT\n"
        "  minimum software version required to extract:   2.0\n"
        "  compression method:                             none (stored)\n"
        "  file security status:                           not encrypted\n"
        "  extended local header:                          no\n"
        "  file last modified on (DOS date/time):          2020 Jan 2 "
        "03:04:06\n"
        "  file last modified on (UT extra field modtime): 2020 Sep 13 "
        "12:26:40 local\n"
        "  file last modified on (UT extra field modtime): 2020 Sep 13 "
        "12:26:40 UTC\n"
        "  32-bit CRC value (hex):                         ddeaa107\n"
        "  compressed size:                                2 bytes\n"
        "  uncompressed size:                              2 bytes\n"
        "  length of filename:                             5 characters\n"
        "  length of extra field:                          9 bytes\n"
        "  length of file comment:                         16 characters\n"
        "  disk number on which file begins:               disk 1\n"
        "  apparent file type:                             binary\n"
        "  Unix file attributes (100644 octal):            -rw-r--r--\n"
        "  MS-DOS file attributes (00 hex):                none\n"
        "\n"
        "  The central-directory extra field contains:\n"
        "  - A subfield with ID 0x5455 (universal time) and 5 data bytes.\n"
        "    The local extra field has UTC/GMT modification time.\n"
        "\n"
        "------------------------- file comment begins "
        "----------------------------\n"
        "an entry comment\n"
        "-------------------------- file comment ends "
        "-----------------------------\n"
        "\n"
        "Central directory entry #2:\n"
        "---------------------------\n"
        "\n"
        "  There are an extra -9 bytes preceding this file.\n"
        "\n"
        "  b\n"
        "\n"
        "  offset of local header from start of archive:   37\n"
        "                                                  (0000000000000025h) "
        "bytes\n"
        "  file system or operating system of origin:      Unix\n"
        "  version of encoding software:                   3.0\n"
        "  minimum file system compatibility required:     MS-DOS, OS/2 or NT "
        "FAT\n"
        "  minimum software version required to extract:   2.0\n"
        "  compression method:                             none (stored)\n"
        "  file security status:                           not encrypted\n"
        "  extended local header:                          no\n"
        "  file last modified on (DOS date/time):          2020 Jan 2 "
        "03:04:06\n"
        "  32-bit CRC value (hex):                         b5ae1bae\n"
        "  compressed size:                                2 bytes\n"
        "  uncompressed size:                              2 bytes\n"
        "  length of filename:                             1 characters\n"
        "  length of extra field:                          0 bytes\n"
        "  length of file comment:                         0 characters\n"
        "  disk number on which file begins:               disk 1\n"
        "  apparent file type:                             binary\n"
        "  Unix file attributes (100644 octal):            -rw-r--r--\n"
        "  MS-DOS file attributes (00 hex):                none\n"
        "\n"
        "  There is no file comment.\n"
        "\n"
        "Central directory entry #3:\n"
        "---------------------------\n"
        "\n"
        "  There are an extra 15 bytes preceding this file.\n"
        "\n"
        "  c\n"
        "\n"
        "  offset of local header from start of archive:   85\n"
        "                                                  (0000000000000055h) "
        "bytes\n"
        "  file system or operating system of origin:      MS-DOS, OS/2 or NT "
        "FAT\n"
        "  version of encoding software:                   2.0\n"
        "  minimum file system compatibility required:     MS-DOS, OS/2 or NT "
        "FAT\n"
        "  minimum software version required to extract:   2.0\n"
        "  compression method:                             none (stored)\n"
        "  file security status:                           not encrypted\n"
        "  extended local header:                          no\n"
        "  file last modified on (DOS date/time):          2020 Jan 2 "
        "03:04:06\n"
        "  32-bit CRC value (hex):                         06b9df6f\n"
        "  compressed size:                                1 bytes\n"
        "  uncompressed size:                              1 bytes\n"
        "  length of filename:                             1 characters\n"
        "  length of extra field:                          0 bytes\n"
        "  length of file comment:                         0 characters\n"
        "  disk number on which file begins:               disk 1\n"
        "  apparent file type:                             binary\n"
        "  non-MSDOS external file attributes:             000000 hex\n"
        "  MS-DOS file attributes (21 hex):                rdo arc \n"
        "\n"
        "  There is no file comment.\n"
        "\n"
    ));
    TEST(output_is(ctx, 2, ""));

    // With totals, before the last blank line, and without the header
    ZIPINFO(ctx, 0, "-v", "-t", "--h", "v.zip");
    TEST(!output_has(ctx, 1, "Archive:"));
    TEST(output_has(ctx, 1, "  MS-DOS file attributes (21 hex):                "
                            "rdo arc \n\n  There is no file comment.\n3 files, "
                            "5 bytes uncompressed, 5 bytes compressed:  0.0%"
                            "\n\n"));

    // An entry not listed: the next's offset is not checked
    ZIPINFO(ctx, 0, "-v", "v.zip", "a.txt", "c");
    TEST(output_has(ctx, 1, "Central directory entry #3:\n"
                            "---------------------------\n\n  c\n"));
    TEST(!output_has(ctx, 1, "There are an extra"));

    // Offsets as recorded, with data before the archive unaccounted for,
    // which follows Zip64 end records just before their locator
    ZIPINFO(ctx, 1, "-v", "p64.zip");
    TEST(output_has(ctx, 1,
        "Archive:  p64.zip\n"
        "There is no zipfile comment.\n"
        "\n"
        "End-of-central-directory record:\n"
        "-------------------------------\n"
        "\n"
        "  Zip archive file size:                       395 "
        "(000000000000018Bh)\n"
        "  Actual end-cent-dir record offset:           297 "
        "(0000000000000129h)\n"
        "  Expected end-cent-dir record offset:         287 "
        "(000000000000011Fh)\n"
        "  (based on the length of the central directory and its expected "
        "offset)\n"
        "\n"
        "  This zipfile constitutes the sole disk of a single-part archive; "
        "its\n"
        "  central directory contains 3 entries.\n"
        "  The central directory is 170 (00000000000000AAh) bytes long,\n"
        "  and its (expected) offset in bytes from the beginning of the "
        "zipfile\n"
        "  is 117 (0000000000000075h).\n"
        "\n"
        "\n"
        "Central directory entry #1:\n"
        "---------------------------\n"
        "\n"
        "  a.txt\n"
        "\n"
        "  offset of local header from start of archive:   0\n"));
    TEST(output_is(ctx, 2,
        "[p64.zip]\n"
        "error: End-of-centdir-64 signature not where expected (prepended "
        "bytes?)\n"
        "  (attempting to process anyway)\n"
        "warning [p64.zip]:  10 extra bytes at beginning or within zipfile\n"
        "  (attempting to process anyway)\n"));

    // An empty archive
    ZIPINFO(ctx, 1, "-v", "empty.zip");
    TEST(output_is(ctx, 1,
        "Archive:  empty.zip\n"
        "There is no zipfile comment.\n"
        "\n"
        "End-of-central-directory record:\n"
        "-------------------------------\n"
        "\n"
        "  Zip archive file size:                        22 "
        "(0000000000000016h)\n"
        "  Actual end-cent-dir record offset:             0 "
        "(0000000000000000h)\n"
        "  Expected end-cent-dir record offset:           0 "
        "(0000000000000000h)\n"
        "  (based on the length of the central directory and its expected "
        "offset)\n"
        "\n"
        "  This zipfile constitutes the sole disk of a single-part archive; "
        "its\n"
        "  central directory contains 0 entries.\n"
        "  The central directory is 0 (0000000000000000h) bytes long,\n"
        "  and its (expected) offset in bytes from the beginning of the "
        "zipfile\n"
        "  is 0 (0000000000000000h).\n"
        "\n"
        "\n"
        "  Empty zipfile.\n"
    ));
    TEST(output_is(ctx, 2, ""));

    // A comment cut short by the end of the file
    ZIPINFO(ctx, 1, "-v", "--h", "cut.zip");
    TEST(output_has(ctx, 1,
        "The zipfile comment is 15 bytes long and contains the following "
        "text:\n"
        "======================== zipfile comment begins "
        "==========================\n"
        "A comment\n"
        "========================= zipfile comment ends "
        "===========================\n"
        "\n"
        "  The zipfile comment is truncated.\n"
        "\n"
        "End-of-central-directory record:\n"));
    TEST(output_is(ctx, 2, ""));

    // A subfield whose length runs past the extra field's end: its error
    // to standard error, or with totals, to standard output, and to
    // standard error too where that alone is a terminal
    ZIPINFO(ctx, 0, "-v", "sub.zip");
    TEST(output_has(ctx, 1,
        "  The central-directory extra field contains:\n"
        "  - A subfield with ID 0x5455 (universal time) and 5 data bytes.\n"
        "    The local extra field has UTC/GMT modification time.\n"
        "  - A subfield with ID 0x1234 (unknown) and 3 data bytes:\n"
        "    61 62 63.\n"
        "\n"
        "  There is no file comment.\n"));
    TEST(output_is(ctx, 2,
        "\n"
        "\n"
        "  error: EF data block (type 0x1234) size 300 exceeds remaining extra "
        "field\n"
        "         space 3; block length has been truncated.\n"));
    ctx->tty[2] = 1;
    ZIPINFO(ctx, 0, "-vt", "sub.zip");
    TEST(output_has(ctx, 1,
        "    The local extra field has UTC/GMT modification time.\n"
        "\n"
        "  error: EF data block (type 0x1234) size 300 exceeds remaining extra "
        "field\n"
        "         space 3; block length has been truncated.\n"
        "\n"
        "  - A subfield with ID 0x1234 (unknown) and 3 data bytes:\n"
        "    61 62 63.\n"));
    TEST(output_is(ctx, 2,
        "\n"
        "\n"
        "  error: EF data block (type 0x1234) size 300 exceeds remaining extra "
        "field\n"
        "         space 3; block length has been truncated.\n"));
    ctx->tty[2] = 0;

    // Archives by a wildcard: the first ends with an entry at offset 4,
    // which UnZip carries to the next, as from a split archive's parts
    ZIPINFO(ctx, 0, "-v", "w/*.zip");
    TEST(output_has(ctx, 1,
        "Archive:  w/b.zip\n"));
    TEST(output_has(ctx, 1,
        "Central directory entry #1:\n"
        "---------------------------\n"
        "\n"
        "  There are an extra -4 bytes preceding this file.\n"
        "\n"
        "  a.txt\n"));
    TEST(output_is(ctx, 2, "\n2 archives were successfully processed.\n"));

    // Memory: an extra field of 16,383 empty subfields, each described in
    // room used again for the next, takes no more than for a short
    // listing (the central header's whole), and a 64 KiB comment no
    // more than in proportion; a single subfield of 64 KiB shows 20 bytes
    static u8 ex[65532];
    static char comment[65535+1];
    for (iz i = 0; i < countof(ex); i += 4) {
        put16(ex+i, 0x9999);
        put16(ex+i+2, 0);
    }
    bytefill(comment, 'c', countof(comment)-1);
    xspec x1[] = {{.name="x", .data="x", .cextra={ex, countof(ex)}}};
    s8 many = build(x1, 1, comment);
    put16(ex, 0x4444);
    put16(ex+2, countof(ex)-4);
    s8 large = build(x1, 1, 0);
    put_archive(ctx, "many.zip", many);
    put_archive(ctx, "large.zip", large);
    ZIPINFO(ctx, 0, "-s", "many.zip");
    iz base = committed(ctx);
    ZIPINFO(ctx, 0, "-v", "many.zip");
    TEST(output_has(ctx, 1, "  - A subfield with ID 0x9999 (unknown) and 0 "
                            "data bytes.\n\n  There is no file comment.\n"));
    TEST(committed(ctx) <= base + 3*65536);  // the comment, shown
    ZIPINFO(ctx, 0, "-s", "large.zip");
    base = committed(ctx);
    ZIPINFO(ctx, 0, "-v", "large.zip");
    TEST(output_has(ctx, 1, "  - A subfield with ID 0x4444 (unknown) and 65528 "
                            "data bytes.  The first\n    20 are:   99 99 00 00 "
                            "99 99 00 00 99 99 00 00 99 99 00 00 99 99 00 00."
                            "\n\n"));
    TEST(committed(ctx) <= base+8192);

    // and 2,000 entries as one
    enum { N = 2000 };
    xspec *all   = calloc(N, sizeof(*all));
    char (*names)[8] = calloc(N, sizeof(*names));
    TEST(all && names);
    for (i32 i = 0; i < N; i++) {
        snprintf(names[i], sizeof(names[i]), "%05d", i);
        all[i] = (xspec){.name=names[i], .data="x", .cextra=utc(1600000000),
                         .comment=S("a comment")};
    }
    s8 first = build(all, 1, 0);
    s8 every = build(all, N, 0);
    put_archive(ctx, "first.zip", first);
    put_archive(ctx, "every.zip", every);
    ZIPINFO(ctx, 0, "-v", "first.zip");
    base = committed(ctx);
    ZIPINFO(ctx, 0, "-vt", "every.zip");
    TEST(output_has(ctx, 1, "Central directory entry #2000:\n"));
    TEST(output_has(ctx, 1, "\n2000 files, 2000 bytes uncompressed"));
    TEST(committed(ctx) < base+64);  // (the totals' longer numbers)

    free(every.s);
    free(first.s);
    free(names);
    free(all);
    free(large.s);
    free(many.s);
    free(split.s);
    free(one.s);
    free(sub.s);
    free(cut.s);
    free(empty.s);
    free(p64.s);
    free(bare.s);
    free(z.s);
}

// What a caller lists (unzip_call's visit): each entry's offset, stored
// name, decoded name, and size, then the archive's end, its strings
// copied out of the run's memory, which the next run reuses.
typedef struct {
    i64  at[8];
    s8   stored[8];
    s8   name[8];
    i64  usize[8];
    i32  n;
    b32  ended;
    i32  cderr;
    s8   comment;
    u8   buf[1024];
    iz   used;
} listing;

static s8 keep_copy(listing *l, s8 s)
{
    TEST(s.len <= countof(l->buf) - l->used);
    s8 r = {l->buf + l->used, s.len};
    if (s.len) {
        memcpy(r.s, s.s, (uz)s.len);
    }
    l->used += s.len;
    return r;
}

static void visit_hook(void *data, unzipvisit *v)
{
    listing *l = data;
    TEST(!l->ended);
    if (!v->e) {
        l->ended   = 1;
        l->cderr   = v->cderr;
        l->comment = keep_copy(l, v->ar->end.comment);
        return;
    }
    TEST(l->n<countof(l->at) && v->ordinal==l->n);
    l->at[l->n]     = v->at;
    l->stored[l->n] = keep_copy(l, v->e->name);
    l->name[l->n]   = keep_copy(l, v->name);
    l->usize[l->n]  = v->e->usize;
    l->n++;
}

// A caller's answers to questions (unzip_call's ask), a line each in
// order, then none, as at the end of input, counted by question.
typedef struct {
    char const *lines[4];
    i32         next;
    i32         asked[3];
    s8          about;  // the path last asked about
    u8          buf[256];
} asker;

static iz ask_hook(void *data, i32 question, s8 path, u8 *buf, iz cap)
{
    asker *a = data;
    a->asked[question]++;
    TEST(path.len <= countof(a->buf));
    memcpy(a->buf, path.s, (uz)path.len);
    a->about = (s8){a->buf, path.len};
    char const *line = a->next<countof(a->lines) ? a->lines[a->next] : 0;
    if (!line) {
        return -1;
    }
    a->next++;
    iz n = (iz)strlen(line);
    TEST(n <= cap);
    memcpy(buf, line, (uz)n);
    return n;
}

// A caller's runs (unzip_call): an archive listed an entry at a time,
// then entries picked by where their central headers are, whatever their
// names hold, extracted under names of the caller's (still made safe, as
// entries' own names are), tested, or reported by ZipInfo -v, each marked
// as met and once done, with questions answered by the caller, never
// standard input, and cancelled at any point, nothing written half
static void test_call(os *ctx)
{
    xspec spec[] = {
        {.name="a[1].txt",    .data="one\n"},
        {.name="dup",         .data="first\n"},
        {.name="dup",         .data="second\n"},
        {.name="d/",          .extattr=DIRM(0755)},
        {.name="d/x.txt",     .data=TEXT, .method=8},
        {.name="d/sub/y.txt", .data="y\n"},
        {.name="enc",         .data="secret", .flags=1},
    };
    s8 z = build(spec, countof(spec), "note");

    // Listed quietly: each entry, then the end
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    listing   l    = {0};
    unzipcall c    = {0};
    i32       open = 0;
    c.archive = S("a.zip");
    c.mode    = CALL_LIST;
    c.visit   = visit_hook;
    c.data    = &l;
    TEST(unzipos_call(ctx, &c, &open) == 0 && !open);
    TEST(l.n==7 && l.ended && l.cderr==CD_OK);
    TEST(equals(l.stored[2], "dup") && equals(l.name[4], "d/x.txt"));
    TEST(l.usize[4] == (iz)strlen(TEXT));
    TEST(equals(l.comment, "note"));
    TEST(output_is(ctx, 3, ""));
    c.visit = 0;
    c.data  = 0;

    // Picked: the second "dup", not the first, and a name of wildcards,
    // which as a pattern would match neither itself nor only itself
    unzippick p1[] = {
        {l.at[0], l.stored[0], S("a[1].txt"), 0, 0},
        {l.at[2], l.stored[2], {0}, 0, 0},  // under its own name
    };
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->dest = S("out/");
    c.mode    = CALL_EXTRACT;
    c.exdir   = S("out");
    c.picks   = p1;
    c.npicks  = countof(p1);
    TEST(unzipos_call(ctx, &c, &open) == 0 && !open);
    TEST(equals(file_data(ctx, "out/a[1].txt"), "one\n"));
    TEST(equals(file_data(ctx, "out/dup"), "second\n"));
    TEST(p1[0].met && p1[0].done && p1[1].met && p1[1].done);
    TEST(output_has(ctx, 1, " extracting: out/dup"));

    // Under the caller's names, a folder and its entries put elsewhere,
    // a ".." dropped, and with Windows conventions, a device's name
    // mapped, all as the entries' own names would be
    unzippick p2[] = {
        {l.at[3], l.stored[3], S("dd/"), 0, 0},
        {l.at[4], l.stored[4], S("dd/x.txt"), 0, 0},
        {l.at[5], l.stored[5], S("../../dd/sub/con.txt"), 0, 0},
    };
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->dest    = S("out/");
    ctx->windows = 1;
    c.picks  = p2;
    c.npicks = countof(p2);
    TEST(unzipos_call(ctx, &c, &open) == 1 && !open);  // the ".." warned of
    TEST(equals(file_data(ctx, "out/dd/x.txt"), TEXT));
    TEST(equals(file_data(ctx, "out/dd/sub/_con.txt"), "y\n"));
    TEST(p2[0].done && p2[1].done && p2[2].done);
    TEST(output_has(ctx, 1, "skipped \"../\" path component"));

    // Met but not done: an encrypted entry, skipped (81); and a pick not
    // there, by its stored name, as when the archive has changed (11)
    unzippick p3[] = {{l.at[6], l.stored[6], {0}, 0, 0}};
    unzippick p4[] = {{l.at[0], S("zzz"), {0}, 0, 0}};
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    ctx->dest = S("out/");
    c.picks  = p3;
    c.npicks = 1;
    TEST(unzipos_call(ctx, &c, &open) == IZ_UNSUP && !open);
    TEST(p3[0].met && !p3[0].done);
    c.picks = p4;
    TEST(unzipos_call(ctx, &c, &open) == PK_FIND && !open);
    TEST(!p4[0].met && !p4[0].done);
    TEST(output_has(ctx, 2, "caution: filename not matched:  zzz\n"));
    TEST(!mfs_get(ctx, "out/a[1].txt"));

    // The caller answers whether to replace a file there, then any new
    // name: no, yes, rename, no answer (None, a warning), None, All
    struct {
        char const *lines[3];
        i32  status;
        char const *a;     // out/a[1].txt's data after
        char const *dup;   // out/dup's
        b32  done[2];
        i32  asked;
    } cases[] = {
        {{"n", "n"},         0, "old", "old", {0, 0}, 2},
        {{"y", "y"},         0, "one\n", "second\n", {1, 1}, 2},
        {{"r", "new.txt", "n"}, 0, "old", "old", {1, 0}, 2},
        {{0},                1, "old", "old", {0, 0}, 1},
        {{"N"},              0, "old", "old", {0, 0}, 1},
        {{"A"},              0, "one\n", "second\n", {1, 1}, 1},
    };
    for (i32 k = 0; k < countof(cases); k++) {
        asker a = {0};
        for (i32 i = 0; i < 3; i++) {
            a.lines[i] = cases[k].lines[i];
        }
        unzippick p[] = {
            {l.at[0], l.stored[0], {0}, 0, 0},
            {l.at[2], l.stored[2], {0}, 0, 0},
        };
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        mfs_put(ctx, "out", FT_DIR, 0, 0, 0);
        mfs_put(ctx, "out/a[1].txt", FT_FILE, "old", 3, 1500000000);
        mfs_put(ctx, "out/dup", FT_FILE, "old", 3, 1500000000);
        ctx->dest = S("out/");
        c.picks  = p;
        c.npicks = countof(p);
        c.ask    = ask_hook;
        c.data   = &a;
        TEST(unzipos_call(ctx, &c, &open) == cases[k].status && !open);
        TEST(equals(file_data(ctx, "out/a[1].txt"), cases[k].a));
        TEST(equals(file_data(ctx, "out/dup"), cases[k].dup));
        TEST(p[0].done==cases[k].done[0] && p[1].done==cases[k].done[1]);
        TEST(a.asked[ASK_REPLACE] == cases[k].asked);
        TEST(equals(a.about, k==2 ? "out/dup" : cases[k].asked==2
                                  ? "out/dup" : "out/a[1].txt"));
        if (k == 2) {
            TEST(a.asked[ASK_NAME] == 1);
            TEST(equals(file_data(ctx, "out/new.txt"), "one\n"));
        }
        TEST(!ctx->inoff);  // never standard input
    }
    c.ask  = 0;
    c.data = 0;

    // Tested: the picks, each done, then all, with UnZip's summaries, the
    // encrypted entry skipped
    unzippick p5[] = {
        {l.at[0], l.stored[0], {0}, 0, 0},
        {l.at[4], l.stored[4], {0}, 0, 0},
    };
    mfs_reset(ctx);
    put_archive(ctx, "a.zip", z);
    c.mode   = CALL_TEST;
    c.picks  = p5;
    c.npicks = countof(p5);
    TEST(unzipos_call(ctx, &c, &open) == 0 && !open);
    TEST(p5[0].done && p5[1].done);
    TEST(output_has(ctx, 1, "    testing: d/x.txt"));
    TEST(output_has(ctx, 1, "No errors detected in a.zip for the 2 files "
                            "tested.\n"));
    c.picks  = 0;
    c.npicks = 0;
    TEST(unzipos_call(ctx, &c, &open) == IZ_UNSUP && !open);
    TEST(output_has(ctx, 1, "1 file skipped because of unsupported "
                            "compression or encoding.\n"));

    // ZipInfo -v of one entry: the archive's sections, then its own,
    // the fifth's, and no other's
    unzippick p6[] = {{l.at[4], l.stored[4], {0}, 0, 0}};
    c.mode   = CALL_ZIPINFO;
    c.picks  = p6;
    c.npicks = 1;
    TEST(unzipos_call(ctx, &c, &open) == 0 && !open);
    TEST(output_has(ctx, 1, "Central directory entry #5:\n"));
    TEST(output_has(ctx, 1, "  d/x.txt\n"));
    TEST(!output_has(ctx, 1, "a[1].txt"));

    // Cancelled at every open, read, and write: whatever was extracted is
    // whole, the file being written gone, nothing left open, until done
    unzippick p7[] = {
        {l.at[0], l.stored[0], {0}, 0, 0},
        {l.at[4], l.stored[4], {0}, 0, 0},
    };
    i32 exits = 0;
    for (i32 at = 1;; at++) {
        mfs_reset(ctx);
        put_archive(ctx, "a.zip", z);
        ctx->dest   = S("out/");
        ctx->exitat = at;
        c.mode   = CALL_EXTRACT;
        c.picks  = p7;
        c.npicks = countof(p7);
        i32 r = unzipos_call(ctx, &c, &open);
        if (r != EXIT_CANCEL) {
            TEST(r==0 && !open);
            break;
        }
        exits++;
        mfile *a = mfs_get(ctx, "out/a[1].txt");
        mfile *x = mfs_get(ctx, "out/d/x.txt");
        TEST(!a || equals((s8){a->data, a->len}, "one\n"));
        TEST(!x || equals((s8){x->data, x->len}, TEXT));
    }
    TEST(exits > 3);
    ctx->exitat = 0;
    free(z.s);
}

int main(void)
{
    (void)bytemove;
    (void)deflate_memsize;
    os *ctx = unzipos_new((iz)1 << 28);
    test_extract(ctx);
    test_overwrite(ctx);
    test_prompt(ctx);
    test_links(ctx);
    test_bomb(ctx);
    test_changed(ctx);
    test_damaged(ctx);
    test_directory(ctx);
    test_faults(ctx);
    test_windows(ctx);
    test_wintimes(ctx);
    test_stdin(ctx);
    test_comments(ctx);
    test_factor(ctx);
    test_wrapped(ctx);
    test_past(ctx);
    test_unwrap(ctx);
    test_local_name(ctx);
    test_pace(ctx);
    test_zipinfo(ctx);
    test_zipinfo_verbose(ctx);
    test_call(ctx);
    unzipos_free(ctx);
    puts("all unzip program tests pass");
    return 0;
}
