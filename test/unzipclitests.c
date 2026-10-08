// Unit tests of the unzip program (src/unzipcli.c), run in memory
// (test/unzipos.c): extraction and what it leaves, overwriting by -n,
// -o, -f, -u, and the prompt, links made last and nothing written
// through a link, zip bombs, damaged entries discarded, faults injected,
// and Windows conventions on any host; in every run, memory claimed only
// before the file system changes, and nothing changed but below the -d
// directory. On success prints "all unzip program tests pass". A failure
// traps.
// $ cc -g3 -fsanitize=address,undefined -o tests-unzipcli test/unzipclitests.c
#include "unzipos.c"
#include "../src/deflate.c"

#define TEST CHECK

static b32 equals(s8 a, char const *z)
{
    return s8equals(a, cstrs8(z));
}

// Run unzip with arguments, which must exit with a status, else its
// messages are shown. A run that returns must leave nothing open.
static void expect(os *ctx, i32 status, char *env, char **argv, i32 argc)
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
        zput16(&z, 0);
        zput16(&z, 0);
        zput16(&z, 0);
        zput32(&z, e->extattr ? e->extattr : 0100644u<<16);
        zput32(&z, (u32)offs[i]);
        zput(&z, name.s, name.len);
        zput(&z, e->cextra.s, e->cextra.len);
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
static b32 same_file(os *ctx, char const *name, u64 ino)
{
    mfile *f = mfs_get(ctx, name);
    return f && f->ino==ino;
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
                        1500000000)->ino;
        u64 b = mfs_put(ctx, "out/p/b.txt", FT_FILE, "new", 3,
                        1700000000)->ino;
        u64 d = mfs_put(ctx, "out/p/d.txt", FT_FILE, "now", 3,
                        1600000000)->ino;
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
    u64 ino[6];
    for (i32 i = 0; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "p/%c.txt", 'a'+i);
        ino[i] = mfs_get(ctx, name)->ino;
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
    TEST(!same_file(ctx, "p/a.txt", ino[0]));
    TEST(same_file(ctx, "p/b.txt", ino[1]));
    TEST(same_file(ctx, "p/c.txt", ino[2]));
    TEST(equals(file_data(ctx, "ren/x.txt"), "ccc\n"));
    for (i32 i = 3; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "p/%c.txt", 'a'+i);
        TEST(!same_file(ctx, name, ino[i]));
        ino[i] = mfs_get(ctx, name)->ino;
    }

    put_stdin(ctx, "n\nN\n");
    UNZIP(ctx, 0, "-q", "a.zip", "p/[def].txt");
    TEST(output_is(ctx, 2,
        "replace p/d.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        "replace p/e.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "));
    for (i32 i = 3; i < 6; i++) {
        char name[8];
        snprintf(name, sizeof(name), "p/%c.txt", 'a'+i);
        TEST(same_file(ctx, name, ino[i]));
    }

    put_stdin(ctx, "");
    UNZIP(ctx, 1, "a.zip", "p/f.txt", "p/e.txt");
    TEST(output_is(ctx, 3,
        "Archive:  a.zip\n"
        "replace p/e.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        " NULL\n(EOF or read error, treating as \"[N]one\" ...)\n"));
    TEST(same_file(ctx, "p/e.txt", ino[4]));
    TEST(same_file(ctx, "p/f.txt", ino[5]));

    // A new name as a directory makes one; the end of input when asked
    // for one keeps the name, as UnZip, and so asks again
    put_stdin(ctx, "r\nnew/dir/\n");
    UNZIP(ctx, 0, "-q", "a.zip", "p/f.txt");
    TEST(mfs_get(ctx, "new/dir")->type == FT_DIR);
    TEST(same_file(ctx, "p/f.txt", ino[5]));
    put_stdin(ctx, "r\n");
    UNZIP(ctx, 1, "-q", "a.zip", "p/f.txt");
    TEST(output_is(ctx, 2,
        "replace p/f.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: new name: "
        "replace p/f.txt? [y]es, [n]o, [A]ll, [N]one, [r]ename: "
        " NULL\n(EOF or read error, treating as \"[N]one\" ...)\n"));
    TEST(same_file(ctx, "p/f.txt", ino[5]));

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
    // archive, not too large for memory (a fuzzer's find)
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
        "error [a.zip]:  start of central directory not found;\n"
        "  zipfile corrupt.\n"
        "  (please check that you have transferred or created the zipfile "
        "in the\n"
        "  appropriate BINARY mode and that you have compiled UnZip "
        "properly)\n"));
    free(b.s);
    free(two.s);
    free(z.s);
}

// Faults: a failed write asks whether to go on, and either way the file
// is not kept (50); a directory, a file, or an attribute that cannot be
// made is reported, and the rest go on; an archive that shrinks or
// fails to read once extraction began leaves no partial file
static void test_faults(os *ctx)
{
    static char big[200000];
    for (iz i = 0; i < countof(big)-1; i++) {
        big[i] = (char)('a' + (i*i/7)%26);
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

// An archive on standard input, a regular file read in place, or else
// read whole, never overwriting unless -o
static void test_stdin(os *ctx)
{
    s8 z = six_files();
    for (i32 file = 0; file < 2; file++) {
        mfs_reset(ctx);
        mfs_put(ctx, "p", FT_DIR, 0, 0, 0);
        u64 a = mfs_put(ctx, "p/a.txt", FT_FILE, "old", 3, 1)->ino;
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
    test_damaged(ctx);
    test_faults(ctx);
    test_windows(ctx);
    test_stdin(ctx);
    unzipos_free(ctx);
    puts("all unzip program tests pass");
    return 0;
}
