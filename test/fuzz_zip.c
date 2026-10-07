// libFuzzer harness: the zip program with arbitrary bytes as its archive
// Runs zip_main in memory (test/zipos.c), so through the program's own
// reader (src/zipin.c's zar_read, its read-ahead window, zar_uname,
// zar_local, and src/zipcli.c's copy_entry), with the input after a
// two-byte header as a.zip beside a few files. The first byte chooses
// the arguments, POSIX or Windows conventions, and a fault, and the
// second where the fault strikes: the archive shrinking, or its reads
// failing, from when it is opened or from when the temporary file is
// created, or the temporary file's writes failing. Whatever zip does
// must leave the archive as it was, or else, having succeeded, replace
// it with one that src/zip.c parses, whose entries are the old ones in
// order, each copied as it was or replaced by a file's, and then files'
// (some modes delete entries), and from which zip reads every entry
// back. No temporary file may remain, and zip may claim no memory once
// it has created one (but after a fault).
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined test/fuzz_zip.c
// $ ./a.out -max_len=8192 corpus/
#include "zipos.c"

#include <stdint.h>

#define MAXIN ((iz)1 << 20)

// The arguments, chosen by the first byte's low three bits
static char *modes[8][5] = {
    {"a.zip", "f", "d/g"},
    {"-r", "a.zip", "d"},
    {"-u", "a.zip"},
    {"-f", "a.zip", "*"},
    {"-FS", "a.zip", "f", "d/g"},
    {"-d", "a.zip", "*a*", "f"},
    {"-X0ry", "a.zip", "d", "f"},  // links stored as links (POSIX)
    {"-uj", "a.zip", "d/g", "x*"},
};
enum { MODE_LINKS = 6 };
static char *windows_links[5] = {"-X0r", "a.zip", "d", "f"};
static b32 deletes(i32 mode)
{
    return mode==4 || mode==5;
}

static b32 same_bytes(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !memcmp(a.s, b.s, (uz)a.len));
}

typedef struct {
    zentry *e;
    s8     *data;  // null where the local header or data is bad
    iz      n;
} zparsed;

// Parse an archive with src/zip.c. Returns false if its end records or
// central directory do not parse, or, if strict, a local header, or it
// names its entry otherwise.
static b32 parse_zip(s8 z, zparsed *r, b32 strict, arena *a)
{
    zend end = {0};
    iz   n   = (iz)MIN(z.len, ZIP_END_LEN + ZIP_MAX16 + ZIP_LOC64_LEN);
    i32  got = zip_find_end(z.s+z.len-n, n, z.len, &end);
    if (got==ZIP_OK && end.end64>=0) {
        CHECK(end.end64+ZIP_END64_LEN <= end.endpos);
        got = zip_parse_end64(z.s+end.end64, &end);
    }
    if (got != ZIP_OK) {
        return 0;
    }
    r->n = (iz)end.count;
    r->e = zip_parse_central(z.s+end.cdoff, (iz)end.cdsize, end.count,
                             end.cdoff, a);
    if (!r->e) {
        return 0;
    }
    r->data = new(a, r->n, s8);
    for (iz i = 0; i < r->n; i++) {
        zentry *e = r->e + i;
        u8     *h = z.s + e->offset;
        iz      v = zip_local_varlen(h);
        i64     d = e->offset + ZIP_LOCAL_LEN + v;
        if (v>=0 && d<=end.cdoff && e->csize<=end.cdoff-d) {
            r->data[i] = (s8){z.s+d, (iz)e->csize};
        }
        s8 lname = {h+ZIP_LOCAL_LEN, (iz)get16(h+26)};
        if (strict && (!r->data[i].s || !same_bytes(lname, e->name))) {
            return 0;  // as zip writes a local header from the central
        }
    }
    return 1;
}

// Whether a new entry is an old one copied, as zip copies it, dropping a
// data descriptor unless encrypted.
static b32 is_copy(zparsed *o, iz i, zparsed *n, iz j)
{
    zentry *a = o->e + i;
    zentry *b = n->e + j;
    u16     d = a->flags&ZIP_FLAG_ENCRYPTED ? 0 : ZIP_FLAG_DESCRIPTOR;
    return o->data[i].s && same_bytes(o->data[i], n->data[j]) &&
           same_bytes(a->name, b->name) &&
           same_bytes(a->comment, b->comment) &&
           same_bytes(a->cextra, b->cextra) && (a->flags & ~d)==b->flags &&
           a->crc==b->crc && a->usize==b->usize && a->method==b->method &&
           a->dostime==b->dostime && a->extattr==b->extattr &&
           a->intattr==b->intattr;
}

// The file that an entry name gives: without "." components or doubled
// slashes, and on Windows, as it ignores case, in lower case.
static s8 file_of(s8 name, b32 windows, u8 *buf, iz cap)
{
    s8 r = {buf, 0};
    for (iz i = 0; i < name.len;) {
        iz j = i;
        for (; j<name.len && name.s[j]!='/'; j++) {}
        s8 part = {name.s+i, j-i};
        if (part.len && !same_bytes(part, S("."))) {
            if (r.len+part.len+1 > cap) {
                return (s8){0};
            }
            if (r.len) {
                r.s[r.len++] = '/';
            }
            for (iz k = 0; k < part.len; k++) {
                u8 c = part.s[k];
                r.s[r.len++] = windows && c>='A' && c<='Z' ? c|0x20 : c;
            }
        }
        i = j + 1;
    }
    return r;
}

// Whether a new entry is one written for a file, by its contents (or, as
// a link, its target).
static b32 is_written(os *ctx, zparsed *n, iz j, b32 windows, b32 links)
{
    zentry *e = n->e + j;
    u8      buf[64];
    s8      name = file_of(e->name, windows, buf, countof(buf));
    b32     dir  = e->name.len && e->name.s[e->name.len-1]=='/';
    if (same_bytes(name, S("d"))) {
        return dir && !e->usize;
    }
    name = same_bytes(name, S("g")) ? S("d/g") : name;  // junked (-j)
    mfile *f = mfs_find(ctx, name);
    f = f && !links ? mfs_follow(ctx, f) : f;
    return f && !dir && e->usize==f->len &&
           e->crc==crc32_update(0, f->data, f->len, &(i32){0});
}

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    static os *ctx;
    static u8 *mem;
    iz cap = (iz)1 << 26;
    if (!ctx) {
        ctx = zipos_new((iz)1 << 27);
        mem = malloc((uz)cap);
    }
    if (size<2 || (iz)size-2>MAXIN) {
        return 0;
    }
    u8  how     = data[0];
    i64 at      = data[1];
    i32 mode    = how & 7;
    b32 windows = how>>3 & 1;
    s8  in      = {(u8 *)data+2, (iz)size-2};

    mfs_reset(ctx);
    u8 text[600];
    for (i32 i = 0; i < countof(text); i++) {
        text[i] = (u8)"zip me, zip me\n"[i%15];
    }
    mfs_put(ctx, "a.zip", FT_FILE, in.s, in.len, 1600000000);
    mfs_put(ctx, "f", FT_FILE, text, countof(text), 1700000000);
    mfs_put(ctx, "d", FT_DIR, 0, 0, 1700000000);
    mfs_put(ctx, "d/g", FT_FILE, "g\n", 2, 1700000000);
    mfs_put(ctx, "d/h", FT_FILE, "", 0, 1700000000);
    mfs_put(ctx, "d/l", FT_LINK, "g", 1, 1700000000);
    switch (how>>4 & 7) {
    case 1: case 2:
        ctx->when     = how>>4 & 1 ? FAULT_OPEN : FAULT_TEMP;
        ctx->shrinkto = in.len * at / 256;
        break;
    case 3: case 4:
        ctx->when       = how>>4 & 1 ? FAULT_OPEN : FAULT_TEMP;
        ctx->failreadat = in.len * at / 256;
        break;
    case 5:
        ctx->failwriteat = at * 64;
        break;
    }

    char **argv = windows && mode==MODE_LINKS ? windows_links : modes[mode];
    b32    links = !windows && mode==MODE_LINKS;
    i32    argc = 0;
    for (; argc<countof(modes[0]) && argv[argc]; argc++) {}
    i32 status = zipos_run(ctx, argv, argc, windows);
    CHECK(status==0  || status==2  || status==3  || status==11 ||
          status==12 || status==14 || status==18);

    // No temporary file is left
    for (i32 i = 0; i < MAX_FILES; i++) {
        mfile *f = ctx->files + i;
        CHECK(!f->live || f->name.len<2 || memcmp(f->name.s, "zi", 2));
    }

    // The archive is as it was (or as it was cut short), or else replaced
    // after a success, which a failed write would have prevented
    s8 now = mfs_get(ctx, "a.zip");
    CHECK(now.s);
    iz was = ctx->armed && ctx->shrinkto>=0 ? MIN(in.len, ctx->shrinkto)
                                            : in.len;
    if (same_bytes(now, (s8){in.s, was})) {
        return 0;
    }
    CHECK(status==0 || status==ZE_OPEN);
    CHECK(ctx->failwriteat<0 || !ctx->faulted);

    // Its entries are the old ones in order, each copied or replaced in
    // place by a file's, followed by files' entries, unless deleted
    arena   a   = {(byte *)mem, (byte *)mem+cap, ctx, 0};
    zparsed old = {0};
    zparsed new = {0};
    CHECK(parse_zip(in, &old, 0, &a));
    CHECK(parse_zip(now, &new, 1, &a));
    iz i = 0;
    for (iz j = 0; j < new.n; j++) {
        iz k = i;
        for (; k<old.n && !is_copy(&old, k, &new, j); k++) {
            if (!deletes(mode)) {
                k = old.n;  // only the next may be copied
                break;
            }
        }
        if (k < old.n) {
            i = k + 1;
        } else {
            CHECK(is_written(ctx, &new, j, windows, links));
            i += i < old.n;  // replaced, or else added
        }
    }
    CHECK(i==old.n || deletes(mode));

    // Which zip reads back, every entry, finding none to delete (unless
    // one is named as such)
    char *again[] = {"-d", "a.zip", "no.such.entry"};
    for (iz j = 0; j < new.n; j++) {
        u8 buf[64];
        s8 name = file_of(new.e[j].name, 1, buf, countof(buf));
        if (same_bytes(name, cstrs8(again[2]))) {
            return 0;
        }
    }
    ctx->when        = 0;
    ctx->failwriteat = -1;
    CHECK(zipos_run(ctx, again, countof(again), windows) == ZE_NONE);
    CHECK(same_bytes(mfs_get(ctx, "a.zip"), now));
    return 0;
}
