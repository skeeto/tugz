// libFuzzer harness: arbitrary bytes as an existing archive
// Parses end records and the central directory as the zip program does
// before merging. Whatever parses is then rewritten as zip does, copying
// each entry's data, and the result must parse back to the same entries.
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined test/fuzz_zipread.c
#include "../src/base.c"
#include "../src/zip.c"

#include <stdint.h>
#include <stdlib.h>

#define CHECK(c)  do { if (!(c)) __builtin_trap(); } while (0)

struct os { int unused; };

static void os_oom(os *ctx)
{
    (void)ctx;
    __builtin_trap();  // bounded inputs never exhaust the arena
}

static b32 same(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !__builtin_memcmp(a.s, b.s, (uz)a.len));
}

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    static byte *mem;
    iz cap = (iz)1 << 26;
    if (!mem) {
        mem = malloc((uz)cap);
    }
    (void)bytemove;
    arena a = {mem, mem+cap, 0};

    iz  len = (iz)size;
    u8 *in  = newbytes(&a, len);
    bytecopy(in, data, len);

    zend end = {0};
    i32 r = zip_find_end(in, len, len, &end);
    if (r==ZIP_OK && end.end64>=0) {
        CHECK(end.end64+ZIP_END64_LEN <= end.endpos);
        r = zip_parse_end64(in+end.end64, &end);
    }
    if (r != ZIP_OK) {
        return 0;
    }
    CHECK(end.cdoff>=0 && end.cdsize>=0 && end.cdoff+end.cdsize<=len);
    zentry *e = zip_parse_central(in+end.cdoff, (iz)end.cdsize, end.count,
                                  end.cdoff, &a);
    if (!e) {
        return 0;
    }

    // Rewrite, as zip copies entries, skipping bad local headers
    iz      n   = (iz)end.count;
    zentry *out = new(&a, n, zentry);
    u8     *buf = newbytes(&a, 2*len + 1024 + n*64);
    u8     *p   = buf;
    iz      m   = 0;
    for (iz i = 0; i < n; i++) {
        CHECK(e[i].offset>=0 && e[i].offset+ZIP_LOCAL_LEN<=end.cdoff);
        CHECK(e[i].csize>=0 && e[i].offset+e[i].csize<=end.cdoff);
        u8 *h = in + e[i].offset;
        iz  v = zip_local_varlen(h);
        i64 dataoff = e[i].offset + ZIP_LOCAL_LEN + v;
        if (v<0 || dataoff>end.cdoff || e[i].csize>end.cdoff-dataoff) {
            continue;
        }
        zentry *o = out + m++;
        *o = e[i];
        iz nlen = get16(h+26);
        s8 lx = {h+ZIP_LOCAL_LEN+nlen, v-nlen};
        o->lextra = zip_filter_extra(&a, lx, 0);
        o->offset = p - buf;
        o->zip64  = o->usize>=ZIP_MAX32 || o->csize>=ZIP_MAX32;
        o->flags &= ~ZIP_FLAG_DESCRIPTOR;
        p = zip_local(p, o);
        bytecopy(p, in+dataoff, (iz)o->csize);
        p += o->csize;
    }
    i64 cdoff = p - buf;
    for (iz i = 0; i < m; i++) {
        p = zip_central(p, out+i);
    }
    i64 cdsize = p - buf - cdoff;
    p = zip_end(p, m, cdsize, cdoff, end.comment);
    iz total = p - buf;

    zend end2 = {0};
    r = zip_find_end(buf, total, total, &end2);
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
        CHECK(zip_local_varlen(buf+back[i].offset) >= 0);
    }
    return 0;
}
