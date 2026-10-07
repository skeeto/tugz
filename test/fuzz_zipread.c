// libFuzzer harness: arbitrary bytes as an existing archive
// Parses end records and the central directory with src/zip.c's parsers,
// whose header parser the zip program shares, though it reads a header
// at a time (src/zipcli.c's read_archive, not fuzzed here). Whatever
// parses is then rewritten much as zip merges, copying each entry's data,
// but skipping entries with bad local headers (zip fails on them) and
// clearing every descriptor flag (zip keeps an encrypted entry's), and
// the result must parse back to the same entries. Entries may share data,
// so a rewrite can far exceed its input.
// Each parser is given exactly the bytes it is to read, in an allocation
// of their size, so that AddressSanitizer sees any read past them.
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined test/fuzz_zipread.c
// $ ./a.out -max_len=8192 corpus/
#include "../src/base.c"
#include "../src/zip.c"

#include <stdint.h>
#include <stdlib.h>

#define CHECK(c)  do { if (!(c)) __builtin_trap(); } while (0)

// Inputs and rewrites beyond these are skipped. Within them the arena
// needs under 3*MAXOUT + 16*MAXIN bytes, so it is never exhausted.
#define MAXIN   ((iz)1 << 20)
#define MAXOUT  ((iz)1 << 23)

struct os { int unused; };

static void os_oom(os *ctx)
{
    (void)ctx;
    __builtin_trap();  // should never happen, as above
}

static void os_extend(os *ctx, arena *a, iz need)
{
    (void)a;
    (void)need;
    os_oom(ctx);
}

static b32 same(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !__builtin_memcmp(a.s, b.s, (uz)a.len));
}

// A copy of exactly len bytes, rather than one within an arena, whose
// following bytes would hide reads past its end.
static u8 *exact(u8 const *p, iz len)
{
    u8 *r = malloc((uz)len);
    CHECK(r || !len);
    return bytecopy(r, p, len);
}

static void rewrite(u8 *in, zend end, zentry *e, arena a)
{
    // Rewrite, as zip copies entries, skipping bad local headers. Many
    // entries may share the same data, so first bound the output size,
    // counting extra fields before filtering, and skip huge rewrites.
    iz      n   = (iz)end.count;
    zentry *out = new(&a, n, zentry);
    u8    **src = new(&a, n, u8 *);
    iz      m   = 0;
    i64     max = 0;
    for (iz i = 0; i < n; i++) {
        CHECK(e[i].offset>=0 && e[i].offset+ZIP_LOCAL_LEN<=end.cdoff);
        CHECK(e[i].csize>=0 && e[i].offset+e[i].csize<=end.cdoff);
        u8 *h = in + e[i].offset;
        iz  v = zip_local_varlen(h);
        i64 dataoff = e[i].offset + ZIP_LOCAL_LEN + v;
        if (v<0 || dataoff>end.cdoff || e[i].csize>end.cdoff-dataoff) {
            continue;
        }
        zentry *o = out + m;
        src[m++]  = in + dataoff;
        *o = e[i];
        iz nlen = get16(h+26);
        o->lextra = (s8){h+ZIP_LOCAL_LEN+nlen, v-nlen};  // filtered below
        o->zip64  = o->usize>=ZIP_MAX32 || o->csize>=ZIP_MAX32;
        o->flags &= ~ZIP_FLAG_DESCRIPTOR;
        max += zip_local_len(o) + o->csize + zip_central_len(o);
    }
    if (max > MAXOUT) {
        return;
    }

    // Lay out the archive exactly, then write it to that layout
    i64 cdoff = 0;
    for (iz i = 0; i < m; i++) {
        out[i].lextra = zip_filter_extra(&a, out[i].lextra);
        out[i].offset = cdoff;
        if (!zip_fits(out+i)) {
            return;  // zip refuses to copy it
        }
        cdoff += zip_local_len(out+i) + out[i].csize;
    }
    i64 cdsize = 0;
    for (iz i = 0; i < m; i++) {
        cdsize += zip_central_len(out+i);
    }
    iz  total = (iz)(cdoff+cdsize) +
                zip_end_len(m, cdsize, cdoff, end.comment);
    u8 *buf   = newbytes(&a, total);
    u8 *p     = buf;
    for (iz i = 0; i < m; i++) {
        CHECK(p-buf == out[i].offset);
        p = zip_local(p, out+i);
        bytecopy(p, src[i], (iz)out[i].csize);
        p += out[i].csize;
    }
    for (iz i = 0; i < m; i++) {
        p = zip_central(p, out+i);
    }
    CHECK(p-buf == cdoff+cdsize);
    p = zip_end(p, m, cdsize, cdoff, end.comment, 0x031e);
    CHECK(p-buf == total);
    buf = exact(buf, total);

    zend end2 = {0};
    i32 r = zip_find_end(buf, total, total, &end2);
    if (r==ZIP_OK && end2.end64>=0) {
        r = zip_parse_end64(buf+end2.end64, &end2);
    }
    CHECK(r == ZIP_OK);
    CHECK(end2.count==m && end2.cdoff==cdoff && end2.cdsize==cdsize);
    zentry *back = zip_parse_central(buf+cdoff, (iz)cdsize, m, cdoff, &a);
    CHECK(back);
    for (iz i = 0; i < m; i++) {
        CHECK(same(back[i].name, out[i].name));
        CHECK(same(back[i].cextra, out[i].cextra));
        CHECK(same(back[i].comment, out[i].comment));
        CHECK(back[i].usize==out[i].usize && back[i].csize==out[i].csize);
        CHECK(back[i].offset==out[i].offset && back[i].crc==out[i].crc);
        CHECK(back[i].flags==out[i].flags && back[i].method==out[i].method);
        CHECK(back[i].dostime==out[i].dostime);
        iz v = zip_local_varlen(buf+back[i].offset);
        CHECK(v == zip_local_len(out+i)-ZIP_LOCAL_LEN);
    }
    free(buf);
}

// Parse as the zip program reads an archive: the end records among its
// final bytes, then the Zip64 end record and the central directory, each
// parser given exactly the bytes it is to read.
static void parse(u8 *in, iz len, arena a)
{
    iz   n    = (iz)MIN(len, ZIP_END_LEN + ZIP_MAX16 + ZIP_LOC64_LEN);
    u8  *tail = exact(in+len-n, n);  // holds the comment until the rewrite
    zend end  = {0};
    i32  r    = zip_find_end(tail, n, len, &end);
    if (r==ZIP_OK && end.end64>=0) {
        CHECK(end.end64+ZIP_END64_LEN <= end.endpos);
        u8 *rec = exact(in+end.end64, ZIP_END64_LEN);
        r = zip_parse_end64(rec, &end);
        free(rec);
    }
    if (r == ZIP_OK) {
        CHECK(end.cdoff>=0 && end.cdsize>=0 && end.cdoff+end.cdsize<=len);
        u8     *cd = exact(in+end.cdoff, (iz)end.cdsize);
        zentry *e  = zip_parse_central(cd, (iz)end.cdsize, end.count,
                                       end.cdoff, &a);
        if (e) {
            rewrite(in, end, e, a);
        }
        free(cd);
    }
    free(tail);
}

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    if (size > MAXIN) {
        return 0;
    }
    static byte *mem;
    iz cap = (iz)1 << 26;
    if (!mem) {
        mem = malloc((uz)cap);
    }
    (void)bytemove;
    arena a  = {mem, mem+cap, 0, 0};
    u8   *in = exact(data, (iz)size);
    parse(in, (iz)size, a);
    free(in);
    return 0;
}
