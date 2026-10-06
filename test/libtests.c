// Test suite for the tugz library interface (tugz.h)
// Exercises streaming with every buffer split, exact stream ends, all
// three formats, flushes, memory handling, and cross-checks with zlib.
// $ cc -g3 -fsanitize=address,undefined -o tests-lib test/libtests.c -lz
#include "../platform/libtugz.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define TEST(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
            fflush(stderr); \
            __builtin_trap(); \
        } \
    } while (0)

_Static_assert((int)FMT_RAW==TUGZ_RAW && (int)FMT_ZLIB==TUGZ_ZLIB &&
               (int)FMT_GZIP==TUGZ_GZIP, "formats");
_Static_assert((int)DEF_NONE==TUGZ_NONE && (int)DEF_SYNC==TUGZ_SYNC &&
               (int)DEF_FULL==TUGZ_FULL && (int)DEF_FINISH==TUGZ_FINISH,
               "flushes");

typedef struct {
    u8 *s;
    iz  len;
} buf;

static u8 *randbytes(iz len, u64 seed)
{
    u8 *p = malloc((uz)len + 1);
    for (iz i = 0; i < len; i++) {
        seed = seed*0x3243f6a8885a308d + 1;
        p[i] = (u8)(seed >> 56);
    }
    return p;
}

// Compressible text-like data with a skewed alphabet and repeats.
static u8 *textbytes(iz len, u64 seed)
{
    static char const words[][8] = {
        "the ", "gzip ", "tiny ", "unity ", "data ", "of ", "and ", "\n",
        "deflate", "zlib ", "stream ", "a ", "window ", "huff ", "bits ",
    };
    u8 *p = malloc((uz)len + 1);
    for (iz i = 0; i < len;) {
        seed = seed*0x3243f6a8885a308d + 1;
        char const *w = words[(seed>>59) % countof(words)];
        for (; *w && i<len; i++) {
            p[i] = (u8)*w++;
        }
    }
    return p;
}

static b32 same(buf a, u8 const *p, iz len)
{
    return a.len==len && (!len || !memcmp(a.s, p, (uz)len));
}

static void *mem_deflator(int format, int level, tugz_deflator **d)
{
    ptrdiff_t len = tugz_deflate_size(format);
    void *mem = malloc((uz)len);
    *d = tugz_deflate_init(mem, len, format, level);
    TEST(*d);
    return mem;
}

static void *mem_inflator(int format, tugz_inflator **z)
{
    ptrdiff_t len = tugz_inflate_size(format);
    void *mem = malloc((uz)len);
    *z = tugz_inflate_init(mem, len, format);
    TEST(*z);
    return mem;
}

// Compress a stream with a new or reset deflator, feeding input and
// taking output in pieces (0 for unlimited). Flushes of the given mode
// are requested at every multiple of flushat input bytes (0 for none).
static buf tcompress_with(tugz_deflator *d, u8 const *p, iz len,
                          iz inpiece, iz outpiece, int fmode, iz flushat)
{
    iz cap = len + len/4 + (1<<16) + (flushat ? 6*(len/flushat+1) : 0);
    buf r = {malloc((uz)cap), 0};

    iz off = 0;
    for (;;) {
        iz next = flushat ? MIN(len, (off/flushat + 1)*flushat) : len;
        int flush = next<len ? fmode : TUGZ_FINISH;
        int status;
        do {
            tugz_buf b = {0};
            b.in = p + off;
            b.inlen = inpiece ? MIN(inpiece, next-off) : next-off;
            b.out = r.s + r.len;
            b.outlen = outpiece ? MIN(outpiece, cap-r.len) : cap-r.len;
            iz inlen = b.inlen;
            status = tugz_deflate(d, &b, off+inlen==next ? flush : TUGZ_NONE);
            off += inlen - b.inlen;
            r.len = b.out - r.s;
            TEST(status>=0);
            TEST(r.len < cap);
        } while (off<next || status!=TUGZ_DONE);
        if (next == len) {
            break;
        }
    }

    // Finishing again is harmless; anything else is misuse
    tugz_buf b = {0};
    TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_DONE);
    TEST(tugz_deflate(d, &b, TUGZ_SYNC) == TUGZ_EUSAGE);
    b.in = p;
    b.inlen = 1;
    TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_EUSAGE);
    return r;
}

// Compress as tcompress_with, with a new deflator.
static buf tcompress(int format, int level, u8 const *p, iz len,
                    iz inpiece, iz outpiece, int fmode, iz flushat)
{
    tugz_deflator *d;
    void *mem = mem_deflator(format, level, &d);
    buf r = tcompress_with(d, p, len, inpiece, outpiece, fmode, flushat);
    free(mem);
    return r;
}

typedef struct {
    int status;
    buf out;
    iz  used;  // input bytes consumed
} result;

// Decompress one stream (or gzip member) with a new or reset inflator,
// with pieces as for compress.
static result tdecompress_with(tugz_inflator *z, u8 const *p, iz len,
                               iz inpiece, iz outpiece)
{
    iz cap = 1 << 22;
    result r = {0};
    r.out.s = malloc((uz)cap);
    for (;;) {
        tugz_buf b = {0};
        b.in = p + r.used;
        b.inlen = inpiece ? MIN(inpiece, len-r.used) : len-r.used;
        b.out = r.out.s + r.out.len;
        b.outlen = outpiece ? MIN(outpiece, cap-r.out.len) : cap-r.out.len;
        iz inlen = b.inlen;
        r.status = tugz_inflate(z, &b);
        r.used += inlen - b.inlen;
        r.out.len = b.out - r.out.s;
        TEST(r.out.len < cap);
        if (r.status==TUGZ_NEED_INPUT && r.used<len) {
            TEST(!b.inlen);
            continue;
        } else if (r.status == TUGZ_NEED_OUTPUT) {
            TEST(!b.outlen);
            continue;
        }
        break;
    }
    return r;
}

// Decompress as tdecompress_with, with a new inflator.
static result tdecompress(int format, u8 const *p, iz len, iz inpiece,
                         iz outpiece)
{
    tugz_inflator *z;
    void *mem = mem_inflator(format, &z);
    result r = tdecompress_with(z, p, len, inpiece, outpiece);
    free(mem);
    return r;
}

static buf zlib_inflate(int wbits, u8 const *p, iz len, b32 *ok)
{
    z_stream s = {0};
    TEST(inflateInit2(&s, wbits) == Z_OK);
    iz cap = 1 << 24;
    buf r = {malloc((uz)cap), 0};
    s.next_in = (u8 *)p;
    s.avail_in = (u32)len;
    s.next_out = r.s;
    s.avail_out = (u32)cap;
    int status = inflate(&s, Z_FINISH);
    *ok = status == Z_STREAM_END;
    r.len = (iz)s.total_out;
    inflateEnd(&s);
    return r;
}

static int zlib_wbits(int format)
{
    return format==TUGZ_RAW ? -15 : format==TUGZ_ZLIB ? 15 : 31;
}

// zlib's output from the first len bytes of a stream in one call, and
// whether it ended there (1), was cut short (0), or failed (-1).
static buf zlib_prefix(int format, u8 const *p, iz len, int *how)
{
    z_stream s = {0};
    TEST(inflateInit2(&s, zlib_wbits(format)) == Z_OK);
    iz cap = 1 << 22;
    buf r = {malloc((uz)cap), 0};
    s.next_in = (u8 *)p;
    s.avail_in = (u32)len;
    s.next_out = r.s;
    s.avail_out = (u32)cap;
    int status = inflate(&s, Z_FINISH);
    TEST(s.avail_out);
    *how = status==Z_STREAM_END ? 1 : status==Z_BUF_ERROR ? 0 : -1;
    r.len = (iz)s.total_out;
    inflateEnd(&s);
    return r;
}

static iz const pieces[] = {0, 1, 2, 3, 7, 16, 17, 100, 4096};

static void test_memory(void)
{
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        ptrdiff_t ilen = tugz_inflate_size(format);
        ptrdiff_t dlen = tugz_deflate_size(format);
        TEST(ilen>0 && dlen>0);
        u8 *mem = malloc((uz)MAX(ilen, dlen) + 64);
        for (iz off = 0; off < 64; off += 7) {
            TEST(!tugz_inflate_init(mem+off, ilen-1, format));
            TEST(!tugz_deflate_init(mem+off, dlen-1, format, 6));
            // Exact sizes suffice at any alignment (os_oom would trap)
            TEST(tugz_inflate_init(mem+off, ilen, format));
            TEST(tugz_deflate_init(mem+off, dlen, format, 6));
        }
        free(mem);
    }
    TEST(!tugz_inflate_size(3));
    TEST(!tugz_deflate_size(-1));
    TEST(!tugz_inflate_init(0, 1<<30, TUGZ_RAW));
}

typedef struct {
    iz    allocs;
    iz    frees;
    void *last;
    iz    size;
} allocstats;

static void *test_alloc(void *ctx, void *ptr, ptrdiff_t old, ptrdiff_t new)
{
    allocstats *st = ctx;
    if (!ptr) {
        TEST(!old && new>0);
        st->allocs++;
        st->size = new;
        return st->last = malloc((uz)new);
    }
    TEST(ptr==st->last && old==st->size && !new);
    st->frees++;
    free(ptr);
    return 0;
}

static void test_allocator(void)
{
    allocstats st = {0};
    tugz_deflator *d = tugz_deflate_new(test_alloc, &st, TUGZ_GZIP, 6);
    TEST(d && st.allocs==1 && st.size==tugz_deflate_size(TUGZ_GZIP));
    u8 out[64];
    tugz_buf b = {(u8 const *)"hello", 5, out, countof(out)};
    TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_DONE);
    iz zlen = countof(out) - b.outlen;
    tugz_deflate_free(d, test_alloc, &st);
    TEST(st.frees == 1);

    st = (allocstats){0};
    tugz_inflator *z = tugz_inflate_new(test_alloc, &st, TUGZ_GZIP);
    TEST(z && st.allocs==1 && st.size==tugz_inflate_size(TUGZ_GZIP));
    u8 back[8];
    b = (tugz_buf){out, zlen, back, countof(back)};
    TEST(tugz_inflate(z, &b) == TUGZ_DONE);
    TEST(!b.inlen && b.outlen==3 && !memcmp(back, "hello", 5));
    tugz_inflate_free(z, test_alloc, &st);
    TEST(st.frees == 1);

    TEST(!tugz_inflate_new(test_alloc, &st, 7));
    tugz_inflate_free(0, test_alloc, &st);
    TEST(st.allocs==1 && st.frees==1);
}

// Round trips in every format and level, with every piece size for both
// directions. Compressed output must not depend on piece sizes, and zlib
// must agree on decoding.
static void test_roundtrip(void)
{
    iz len = 300000;
    u8 *text = textbytes(len, 1);
    u8 *noise = randbytes(len, 2);
    static i32 const levels[] = {1, 4, 6, 9};
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        for (i32 li = 0; li < countof(levels); li++) {
            for (i32 k = 0; k < 2; k++) {
                u8 *p = k ? noise : text;
                iz n = k ? len/3 : len;
                buf ref = tcompress(format, levels[li], p, n, 0, 0, 0, 0);
                b32 ok;
                buf z = zlib_inflate(zlib_wbits(format), ref.s, ref.len, &ok);
                TEST(ok && same(z, p, n));
                free(z.s);

                for (i32 i = 1; i < countof(pieces); i++) {
                    if (pieces[i]<16 && n>20000) {
                        continue;  // too slow; covered below
                    }
                    buf c = tcompress(format, levels[li], p, n,
                                     pieces[i], pieces[countof(pieces)-i], 0, 0);
                    TEST(same(c, ref.s, ref.len));
                    free(c.s);
                }
                for (i32 i = 0; i < countof(pieces); i++) {
                    if (pieces[i]<16 && n>20000) {
                        continue;
                    }
                    result r = tdecompress(format, ref.s, ref.len, pieces[i],
                                          pieces[(i+3)%countof(pieces)]);
                    TEST(r.status==TUGZ_DONE && r.used==ref.len);
                    TEST(same(r.out, p, n));
                    free(r.out.s);
                }
                free(ref.s);
            }
        }
    }

    // Byte-at-a-time in both directions on a shorter input
    iz n = 20000;
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        buf ref = tcompress(format, 6, text, n, 0, 0, 0, 0);
        buf c = tcompress(format, 6, text, n, 1, 1, 0, 0);
        TEST(same(c, ref.s, ref.len));
        result r = tdecompress(format, ref.s, ref.len, 1, 1);
        TEST(r.status==TUGZ_DONE && same(r.out, text, n));
        free(r.out.s);
        free(c.s);
        free(ref.s);
    }
    free(noise);
    free(text);
}

// Every two-piece split of a variety of streams, including dynamic
// block headers split at every byte, must decode identically, and every
// proper prefix must ask for more input.
static void test_splits(void)
{
    iz n = 3000;
    u8 *text = textbytes(n, 3);
    u8 *noise = randbytes(n, 4);
    static int const strategies[] = {
        Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED
    };
    for (i32 si = 0; si < countof(strategies); si++) {
        for (i32 k = 0; k < 3; k++) {
            u8 *p = k==1 ? noise : text;
            z_stream s = {0};
            TEST(deflateInit2(&s, k==2 ? 0 : 6, Z_DEFLATED, -15, 8,
                              strategies[si]) == Z_OK);
            u8 z[8192];
            s.next_in = p;
            s.avail_in = (u32)n;
            s.next_out = z;
            s.avail_out = sizeof(z);
            TEST(deflate(&s, Z_FINISH) == Z_STREAM_END);
            iz zlen = (iz)s.total_out;
            deflateEnd(&s);

            tugz_inflator *zs;
            void *mem = mem_inflator(TUGZ_RAW, &zs);
            u8 *out = malloc((uz)n + 1);
            for (iz cut = 0; cut <= zlen; cut++) {
                TEST(tugz_inflate_init(mem, tugz_inflate_size(TUGZ_RAW),
                                       TUGZ_RAW) == zs);
                tugz_buf b = {z, cut, out, n};
                int status = tugz_inflate(zs, &b);
                TEST(status == (cut==zlen ? TUGZ_DONE : TUGZ_NEED_INPUT));
                TEST(!b.inlen);
                if (cut < zlen) {
                    b.in = z + cut;
                    b.inlen = zlen - cut;
                    TEST(tugz_inflate(zs, &b) == TUGZ_DONE);
                    TEST(!b.inlen);
                }
                TEST(!b.outlen && !memcmp(out, p, (uz)n));
            }
            free(out);
            free(mem);
        }
    }
    free(noise);
    free(text);
}

// The stream end is reported exactly, whatever follows it.
static void test_stream_end(void)
{
    iz n = 50000;
    u8 *text = textbytes(n, 5);
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        for (i32 level = 1; level <= 9; level += 4) {
            buf c = tcompress(format, level, text, n, 0, 0, 0, 0);
            u8 *z = malloc((uz)c.len + 100);
            memcpy(z, c.s, (uz)c.len);
            memset(z+c.len, 0x55, 100);
            for (i32 i = 0; i < countof(pieces); i++) {
                for (i32 j = 0; j < countof(pieces); j += 4) {
                    if (pieces[i] && pieces[i]<16) {
                        continue;
                    }
                    result r = tdecompress(format, z, c.len+100, pieces[i],
                                          pieces[j]);
                    TEST(r.status == TUGZ_DONE);
                    TEST(r.used == c.len);
                    TEST(same(r.out, text, n));
                    free(r.out.s);
                }
            }
            free(z);
            free(c.s);
        }
    }
    free(text);
}

static void test_members(void)
{
    buf a = tcompress(TUGZ_GZIP, 6, (u8 const *)"hello ", 6, 0, 0, 0, 0);
    buf b = tcompress(TUGZ_GZIP, 1, (u8 const *)"world", 5, 0, 0, 0, 0);
    iz len = a.len + b.len + 4;
    u8 *p = malloc((uz)len);
    memcpy(p, a.s, (uz)a.len);
    memcpy(p+a.len, b.s, (uz)b.len);
    memcpy(p+a.len+b.len, "junk", 4);

    for (i32 i = 0; i < countof(pieces); i++) {
        tugz_inflator *z;
        void *mem = mem_inflator(TUGZ_GZIP, &z);
        u8 out[64];
        tugz_buf t = {p, 0, out, countof(out)};
        iz off = 0;
        int status;
        i32 members = 0;
        for (;;) {
            t.in = p + off;
            t.inlen = pieces[i] ? MIN(pieces[i], len-off) : len-off;
            iz inlen = t.inlen;
            status = tugz_inflate(z, &t);
            off += inlen - t.inlen;
            if (status == TUGZ_DONE) {
                members++;
                TEST(off == (members==1 ? a.len : a.len+b.len));
            } else if (status != TUGZ_NEED_INPUT) {
                break;
            }
        }
        TEST(members==2 && status==TUGZ_ENOTGZ);
        TEST(tugz_inflate(z, &t) == TUGZ_ENOTGZ);  // sticky
        TEST(countof(out)-t.outlen==11 && !memcmp(out, "hello world", 11));
        free(mem);
    }
    free(p);
    free(b.s);
    free(a.s);
}

static int zlib_header(u8 const *hdr, iz len)
{
    u8 z[64];
    memcpy(z, hdr, (uz)len);
    result r = tdecompress(TUGZ_ZLIB, z, len, 0, 0);
    free(r.out.s);
    return r.status;
}

static void test_zlib_format(void)
{
    // Headers match zlib's at every level
    u8 const *msg = (u8 const *)"zlib format";
    for (i32 level = 1; level <= 9; level++) {
        buf c = tcompress(TUGZ_ZLIB, level, msg, 11, 0, 0, 0, 0);
        u8 z[64];
        uLongf zlen = sizeof(z);
        TEST(compress2(z, &zlen, msg, 11, level) == Z_OK);
        TEST(c.s[0]==z[0] && c.s[1]==z[1]);
        TEST(!memcmp(c.s+c.len-4, z+zlen-4, 4));  // Adler-32
        free(c.s);
    }

    // Adler-32 against zlib, including sums near the modulus
    u8 *ff = malloc(100000);
    memset(ff, 0xff, 100000);
    for (iz len = 0; len < 100000; len = len*3 + 1) {
        TEST(adler32_update(1, ff, len) == adler32(1, ff, (uInt)len));
    }
    free(ff);

    static u8 const empty[] = {0x78, 0x9c, 0x03, 0x00, 0, 0, 0, 1};
    TEST(zlib_header(empty, 8) == TUGZ_DONE);
    static u8 const badcheck[] = {0x78, 0x9d, 0x03, 0x00, 0, 0, 0, 1};
    TEST(zlib_header(badcheck, 8) == TUGZ_EHEADER);
    static u8 const method[] = {0x79, 0x94, 0x03, 0x00, 0, 0, 0, 1};
    TEST((method[0]<<8 | method[1]) % 31 == 0);
    TEST(zlib_header(method, 8) == TUGZ_EHEADER);
    static u8 const window[] = {0x88, 0x98, 0x03, 0x00, 0, 0, 0, 1};
    TEST((window[0]<<8 | window[1]) % 31 == 0);
    TEST(zlib_header(window, 8) == TUGZ_EHEADER);
    static u8 const small[] = {0x08, 0x1d, 0x03, 0x00, 0, 0, 0, 1};
    TEST((small[0]<<8 | small[1]) % 31 == 0);
    TEST(zlib_header(small, 8) == TUGZ_DONE);
    // A preset dictionary is unsupported, but as with zlib, which asks
    // for it (Z_NEED_DICT) once it has the ID, input ending before then
    // is truncation
    static u8 const dict[] = {0x78, 0xbb, 0, 0, 0x30, 0x39, 0x03, 0x00};
    TEST((dict[0]<<8 | dict[1]) % 31 == 0);
    for (iz len = 0; len <= 8; len++) {
        int how;
        buf z = zlib_prefix(TUGZ_ZLIB, dict, len, &how);
        TEST(!z.len && how==(len<6 ? 0 : -1));
        free(z.s);
        TEST(zlib_header(dict, len) == (len<6 ? TUGZ_NEED_INPUT
                                              : TUGZ_EHEADER));
    }
    static u8 const badsum[] = {0x78, 0x9c, 0x03, 0x00, 0, 0, 0, 2};
    TEST(zlib_header(badsum, 8) == TUGZ_ECHECK);
    TEST(zlib_header(empty, 7) == TUGZ_NEED_INPUT);
}

static void test_gzip_errors(void)
{
    buf c = tcompress(TUGZ_GZIP, 6, (u8 const *)"abc", 3, 0, 0, 0, 0);
    result r = tdecompress(TUGZ_GZIP, (u8 const *)"\x1f\x8c", 2, 0, 0);
    TEST(r.status == TUGZ_ENOTGZ);
    free(r.out.s);

    c.s[c.len-8] ^= 1;
    r = tdecompress(TUGZ_GZIP, c.s, c.len, 0, 0);
    TEST(r.status == TUGZ_ECHECK);
    free(r.out.s);
    c.s[c.len-8] ^= 1;
    c.s[c.len-4] ^= 1;
    r = tdecompress(TUGZ_GZIP, c.s, c.len, 1, 1);
    TEST(r.status == TUGZ_ELENGTH);
    free(r.out.s);
    c.s[3] = 0x20;
    r = tdecompress(TUGZ_GZIP, c.s, c.len, 0, 0);
    TEST(r.status == TUGZ_EHEADER);
    free(r.out.s);
    free(c.s);

    static u8 const bad[] = {0x07, 0x00};  // block type 3
    r = tdecompress(TUGZ_RAW, bad, 2, 0, 0);
    TEST(r.status == TUGZ_EDATA);
    free(r.out.s);
}

// Flushes: SYNC ends with an empty stored block and lets a decoder
// produce everything so far; FULL also allows decoding to start there.
static void test_flush(void)
{
    iz n = 200000;
    u8 *text = textbytes(n, 6);
    for (int fmode = TUGZ_SYNC; fmode <= TUGZ_FULL; fmode++) {
        for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
            for (iz at = 1; at < n; at = at*5 + 3) {
                buf c = tcompress(format, 6, text, n, 0, 0, fmode, at);
                b32 ok;
                buf z = zlib_inflate(zlib_wbits(format), c.s, c.len, &ok);
                TEST(ok && same(z, text, n));
                free(z.s);
                result r = tdecompress(format, c.s, c.len, 0, 1000);
                TEST(r.status==TUGZ_DONE && same(r.out, text, n));
                free(r.out.s);
                buf c2 = tcompress(format, 6, text, n, 777, 333, fmode, at);
                TEST(same(c2, c.s, c.len));
                free(c2.s);
                free(c.s);
            }
        }
    }

    // A raw stream flushed once: the prefix decodes to the first part,
    // and with FULL the suffix decodes alone
    for (int fmode = TUGZ_SYNC; fmode <= TUGZ_FULL; fmode++) {
        tugz_deflator *d;
        void *mem = mem_deflator(TUGZ_RAW, 6, &d);
        iz cap = n + (1<<16);
        u8 *z = malloc((uz)cap);
        tugz_buf b = {text, n/2, z, cap};
        TEST(tugz_deflate(d, &b, fmode) == TUGZ_DONE);
        iz mid = b.out - z;
        TEST(mid>=4 && !memcmp(z+mid-4, "\0\0\xff\xff", 4));
        b.in = text + n/2;
        b.inlen = n - n/2;
        TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_DONE);
        iz zlen = b.out - z;

        z_stream s = {0};
        TEST(inflateInit2(&s, -15) == Z_OK);
        u8 *out = malloc((uz)n);
        s.next_in = z;
        s.avail_in = (u32)mid;
        s.next_out = out;
        s.avail_out = (u32)n;
        TEST(inflate(&s, Z_SYNC_FLUSH) == Z_OK);
        TEST(s.total_out==(uLong)n/2 && !memcmp(out, text, (uz)n/2));
        inflateEnd(&s);

        result r = tdecompress(TUGZ_RAW, z+mid, zlen-mid, 0, 0);
        if (fmode == TUGZ_FULL) {
            TEST(r.status == TUGZ_DONE);
            TEST(same(r.out, text+n/2, n-n/2));
        }
        free(r.out.s);
        free(out);
        free(z);
        free(mem);
    }

    // Repeated flushes with no input in between are fine
    tugz_deflator *d;
    void *mem = mem_deflator(TUGZ_ZLIB, 6, &d);
    u8 z[256];
    tugz_buf b = {0, 0, z, countof(z)};
    for (i32 i = 0; i < 5; i++) {
        TEST(tugz_deflate(d, &b, i&1 ? TUGZ_SYNC : TUGZ_FULL) == TUGZ_DONE);
    }
    TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_DONE);
    b32 ok;
    buf o = zlib_inflate(15, z, countof(z)-b.outlen, &ok);
    TEST(ok && !o.len);
    free(o.s);
    free(mem);
    free(text);
}

// Compress p in steps, step i taking input up to ends[i] with modes[i]
// (the last TUGZ_FINISH), and output in pieces (0 for unlimited). With
// early, a step moves on once its input is consumed rather than waiting
// for its flush to return TUGZ_DONE, counting in early[1] the SYNC and
// FULL flushes left with output staged, and in early[0] those not yet
// staged at all.
static buf tcompress_steps(int format, u8 const *p, iz const *ends,
                           int const *modes, i32 nsteps, iz outpiece,
                           i32 *early)
{
    tugz_deflator *d;
    void *mem = mem_deflator(format, 6, &d);
    deflator *def = d->e->def;
    iz cap = ends[nsteps-1] + (1<<16);
    buf r = {malloc((uz)cap), 0};
    iz off = 0;
    for (i32 i = 0; i < nsteps; i++) {
        for (;;) {
            tugz_buf b = {p+off, ends[i]-off, r.s+r.len, cap-r.len};
            b.outlen = outpiece ? MIN(outpiece, b.outlen) : b.outlen;
            iz inlen = b.inlen;
            int status = tugz_deflate(d, &b, modes[i]);
            off += inlen - b.inlen;
            r.len = b.out - r.s;
            TEST(r.len < cap);
            if (status == TUGZ_NEED_OUTPUT) {
                TEST(!b.outlen);
                if (early && off==ends[i] && modes[i]!=TUGZ_FINISH) {
                    if (modes[i] != TUGZ_NONE) {
                        TEST(def->flushing == modes[i]);
                        early[def->flushed]++;
                    }
                    break;
                }
                continue;
            }
            TEST(off == ends[i]);
            TEST(status == (modes[i]==TUGZ_NONE ? TUGZ_NEED_INPUT
                                                : TUGZ_DONE));
            break;
        }
    }
    free(mem);
    return r;
}

// A SYNC or FULL flush falls due once its call has consumed all input,
// and completes whatever the mode of later calls, which then go on as
// asked: a caller may move on without waiting for TUGZ_DONE, as to
// TUGZ_FINISH at the end of input, and the stream comes out as if it had
// waited. Tiny output buffers leave the flush's output staged; larger
// earlier output fills staging so that the flush is not yet staged.
static void test_flush_switch(void)
{
    iz n = 6000;
    u8 *text = textbytes(n, 18);
    iz m = 400000;
    u8 *mixed = randbytes(m, 19);
    u8 *more = textbytes(m/4, 20);
    memcpy(mixed + m - m/4, more, (uz)(m/4));
    free(more);

    i32 early[2] = {0};
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        for (int fmode = TUGZ_SYNC; fmode <= TUGZ_FULL; fmode++) {
            for (int next = TUGZ_NONE; next <= TUGZ_FINISH; next++) {
                for (i32 k = 0; k < 4; k++) {
                    static iz const outpieces[] = {1, 7, 64, 4096};
                    u8 *p = k<3 ? text : mixed;
                    iz len = k<3 ? n : m;
                    iz ends[] = {k<3 ? len/2 : len - len/4, len, len};
                    int modes[] = {fmode, next, TUGZ_FINISH};
                    i32 nsteps = next==TUGZ_FINISH ? 2 : 3;
                    buf ref = tcompress_steps(format, p, ends, modes, nsteps,
                                              0, 0);
                    b32 ok;
                    buf z = zlib_inflate(zlib_wbits(format), ref.s, ref.len,
                                         &ok);
                    TEST(ok && same(z, p, len));
                    free(z.s);

                    buf c = tcompress_steps(format, p, ends, modes, nsteps,
                                            outpieces[k], early);
                    TEST(same(c, ref.s, ref.len));
                    result r = tdecompress(format, c.s, c.len, 0, 0);
                    TEST(r.status==TUGZ_DONE && same(r.out, p, len));
                    free(r.out.s);
                    free(c.s);
                    free(ref.s);
                }
            }
        }
    }
    TEST(early[0] && early[1]);

    // Once TUGZ_FINISH falls due, other calls are rejected and change
    // nothing
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        buf ref = tcompress(format, 6, text, n, 0, 0, 0, 0);
        tugz_deflator *d;
        void *mem = mem_deflator(format, 6, &d);
        iz cap = ref.len + 100;
        u8 *z = malloc((uz)cap);
        tugz_buf b = {text, n, z, 5};
        TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_NEED_OUTPUT);
        TEST(!b.inlen && b.out==z+5);
        for (int mode = TUGZ_NONE; mode <= TUGZ_FULL; mode++) {
            b.outlen = 5;
            TEST(tugz_deflate(d, &b, mode) == TUGZ_EUSAGE);
        }
        b.in = text;
        b.inlen = 1;
        TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_EUSAGE);
        TEST(b.inlen==1 && b.out==z+5);
        b.inlen = 0;
        int status;
        do {
            b.outlen = MIN(5, cap - (b.out - z));
            TEST(b.outlen);
            status = tugz_deflate(d, &b, TUGZ_FINISH);
        } while (status == TUGZ_NEED_OUTPUT);
        TEST(status == TUGZ_DONE);
        TEST(b.out-z==ref.len && !memcmp(z, ref.s, (uz)ref.len));
        free(z);
        free(mem);
        free(ref.s);
    }
    free(mixed);
    free(text);
}

// Staging is bounded, so TUGZ_NONE may stop short of consuming all input
// and return TUGZ_NEED_OUTPUT with the output buffer full. A caller that
// supplies output space and calls again with the rest gets the stream of
// one call with ample output.
static void test_none_staging(void)
{
    iz n = (iz)2 << 20;
    u8 *noise = randbytes(n, 21);
    buf ref = tcompress(TUGZ_GZIP, 1, noise, n, 0, 0, 0, 0);
    tugz_deflator *d;
    void *mem = mem_deflator(TUGZ_GZIP, 1, &d);
    iz cap = ref.len + 100;
    u8 *z = malloc((uz)cap);
    tugz_buf b = {noise, n, z, 0};
    TEST(tugz_deflate(d, &b, TUGZ_NONE) == TUGZ_NEED_OUTPUT);
    TEST(b.inlen>0 && b.out==z);
    for (int flush = TUGZ_NONE;; flush = TUGZ_FINISH) {
        int status;
        do {
            b.outlen = MIN(1<<16, cap - (b.out - z));
            TEST(b.outlen);
            status = tugz_deflate(d, &b, flush);
            TEST(status==TUGZ_NEED_OUTPUT ? !b.outlen : !b.inlen);
        } while (status == TUGZ_NEED_OUTPUT);
        if (flush == TUGZ_FINISH) {
            TEST(status == TUGZ_DONE);
            break;
        }
        TEST(status == TUGZ_NEED_INPUT);
    }
    TEST(b.out-z==ref.len && !memcmp(z, ref.s, (uz)ref.len));
    free(z);
    free(mem);
    free(ref.s);
    free(noise);
}

// Staged output moves only to make room for an emission step, so a
// caller taking output a byte at a time does not pay on each call to
// move all that remains. Each step (a block of at least MIN_BLOCK bytes
// of input but for the last, or the finish) moves staging at most twice:
// once to compact, and once when drained to empty.
static void test_staging_moves(void)
{
    iz n = 300000;
    u8 *noise = randbytes(n, 22);
    tugz_deflator *d;
    void *mem = mem_deflator(TUGZ_RAW, 1, &d);
    deflator *def = d->e->def;
    tugz_buf b = {noise, n, 0, 0};
    u8 out;
    i32 calls = 0;
    i32 moves = 0;
    int status;
    do {
        iz ooff = def->ooff;
        b.out = &out;
        b.outlen = 1;
        status = tugz_deflate(d, &b, TUGZ_FINISH);
        moves += def->ooff != ooff+1;
        calls++;
    } while (status == TUGZ_NEED_OUTPUT);
    TEST(status==TUGZ_DONE && calls>n);
    TEST(moves <= 2*(n/MIN_BLOCK + 2));
    free(mem);
    free(noise);
}

// Large inputs cross window slides and many blocks in both directions,
// and deflate's staged output never exceeds its bound.
static void test_large(void)
{
    iz n = (iz)5 << 20;
    u8 *p = textbytes(n, 7);
    u8 *noise = randbytes(1<<20, 8);
    memcpy(p + n/2, noise, 1<<20);
    free(noise);
    buf c = tcompress(TUGZ_GZIP, 1, p, n, 1<<16, 1<<16, 0, 0);
    buf c2 = tcompress(TUGZ_GZIP, 1, p, n, 0, 3000, 0, 0);
    TEST(same(c2, c.s, c.len));
    b32 ok;
    buf z = zlib_inflate(31, c.s, c.len, &ok);
    TEST(ok && same(z, p, n));
    free(z.s);

    tugz_inflator *zs;
    void *mem = mem_inflator(TUGZ_GZIP, &zs);
    u8 *out = malloc((uz)n);
    tugz_buf b = {c.s, c.len, out, n};
    TEST(tugz_inflate(zs, &b) == TUGZ_DONE);
    TEST(!b.inlen && !b.outlen && !memcmp(out, p, (uz)n));
    free(out);
    free(mem);
    free(c2.s);
    free(c.s);
    free(p);
}

enum { PREV_FINISH, PREV_SYNC, PREV_FULL, PREV_NONE, PREV_STALLED, NPREV };

// Compress a stream that ends as given, unfinished but for PREV_FINISH:
// flushed with more input pending, input consumed but not yet compressed
// (past the window, compressed and slid in part), or output left staged.
static void prev_stream(tugz_deflator *d, u8 const *p, iz len, int how)
{
    iz cap = len + len/4 + (1<<16);
    u8 *z = malloc((uz)cap);
    tugz_buf b = {p, len, z, cap};
    switch (how) {
    case PREV_FINISH:
        TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_DONE);
        break;
    case PREV_SYNC:
    case PREV_FULL:
        TEST(tugz_deflate(d, &b, how==PREV_SYNC ? TUGZ_SYNC : TUGZ_FULL)
             == TUGZ_DONE);
        b.in = p;
        b.inlen = len/2;
        TEST(tugz_deflate(d, &b, TUGZ_NONE) == TUGZ_NEED_INPUT);
        break;
    case PREV_NONE:
        TEST(tugz_deflate(d, &b, TUGZ_NONE) == TUGZ_NEED_INPUT);
        break;
    case PREV_STALLED:
        b.outlen = 1;
        TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_NEED_OUTPUT);
        break;
    }
    free(z);
}

// A reset deflator compresses exactly as a new one, at any level and in
// any format, whatever its previous stream and however it ended: short
// or long (the reset clears history in two ways), compressible or not
// (with and without 3-byte matching, which a stream too short to sample
// never uses).
static void test_reset(void)
{
    iz n = (iz)1300000;  // past the window
    u8 *text = textbytes(n, 9);
    u8 *noise = randbytes(n, 10);
    iz m = 30000;
    u8 *next = textbytes(m, 11);
    for (iz i = m/3; i < m; i++) {
        next[i] = (u8)('a' + (noise[i] & 15));  // for 3-byte matching
    }
    u8 const *tiny = (u8 const *)"abc1abc2abc3abc4abc5abc6abc7abc8abc9";
    iz tinylen = 36;
    iz const prevlens[] = {0, 300, 20000, n};

    // Check that the inputs cover what they are meant to
    tugz_deflator *d;
    void *mem = mem_deflator(TUGZ_RAW, 6, &d);
    free(tcompress_with(d, next, m, 0, 0, 0, 0).s);
    TEST(d->e->def->use3);
    tugz_deflate_reset(d, 6);
    free(tcompress_with(d, text, 20000, 0, 0, 0, 0).s);
    TEST(!d->e->def->use3);
    tugz_deflate_reset(d, 6);
    free(tcompress_with(d, noise, 300, 0, 0, 0, 0).s);
    TEST(d->e->def->use3);
    free(mem);

    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        for (int level = 1; level <= 9; level++) {
            buf tref = tcompress(format, level, tiny, tinylen, 0, 0, 0, 0);
            buf ref = tcompress(format, level, next, m, 0, 0, 0, 0);
            buf sync = tcompress(format, level, next, m, 0, 0, TUGZ_SYNC,
                                 7000);
            buf full = tcompress(format, level, next, m, 0, 0, TUGZ_FULL,
                                 5000);
            mem = mem_deflator(format, 10-level, &d);
            for (int how = 0; how < NPREV; how++) {
                for (i32 k = 0; k < 2*countof(prevlens); k++) {
                    u8 *p = k&1 ? noise : text;
                    iz len = prevlens[k>>1];
                    if (len==n && (format!=TUGZ_RAW || level%4!=1)) {
                        continue;  // slow, and raw at 1, 5, 9 suffices
                    }
                    tugz_deflate_reset(d, k&2 ? level : 10-level);
                    prev_stream(d, p, len, how);
                    tugz_deflate_reset(d, level);
                    buf c;
                    switch ((how + k) % 4) {
                    case 0:
                        c = tcompress_with(d, next, m, 777, 333, 0, 0);
                        TEST(same(c, ref.s, ref.len));
                        break;
                    case 1:
                        c = tcompress_with(d, next, m, 0, 100, TUGZ_SYNC,
                                           7000);
                        TEST(same(c, sync.s, sync.len));
                        break;
                    case 2:
                        c = tcompress_with(d, next, m, 1000, 0, TUGZ_FULL,
                                           5000);
                        TEST(same(c, full.s, full.len));
                        break;
                    default:
                        c = tcompress_with(d, tiny, tinylen, 1, 1, 0, 0);
                        TEST(same(c, tref.s, tref.len));
                    }
                    free(c.s);
                }
            }

            // Reset with no stream in between, and back to back
            tugz_deflate_reset(d, level);
            tugz_deflate_reset(d, level);
            buf c = tcompress_with(d, next, m, 0, 0, 0, 0);
            TEST(same(c, ref.s, ref.len));
            free(c.s);
            tugz_deflate_reset(d, level);
            c = tcompress_with(d, next, m, 0, 0, 0, 0);
            TEST(same(c, ref.s, ref.len));
            free(c.s);

            free(mem);
            free(full.s);
            free(sync.s);
            free(ref.s);
            free(tref.s);
        }
    }
    tugz_deflate_reset(0, 6);  // ignored
    free(next);
    free(noise);
    free(text);
}

// Forgetting more than a short history advances a stamp on the hash
// table entries rather than clearing them, until the stamps run out after
// 2,047 advances and the heads are cleared. Across that, streams compress
// as new ones: after a dense history (which, were sampling to see its
// 3-byte heads, would flip the next stream's choice of 3-byte matching),
// across the clearing, and long enough to slide the window past older
// entries.
static void test_reset_stamps(void)
{
    iz n = (iz)1100000;  // past the window
    u8 *rnd = randbytes(n, 15);
    u8 *text = textbytes(n, 16);
    u8 *abc = randbytes(n, 17);
    for (iz i = 0; i < n; i++) {
        abc[i] = (u8)('a' + abc[i]%11);  // 3-byte matching, barely
    }
    u8 *zeros = calloc(2000, 1);
    tugz_deflator *d;
    void *mem = mem_deflator(TUGZ_RAW, 1, &d);
    deflator *def = d->e->def;

    // Stamps whose many bits stand out in the 3-byte heads
    free(tcompress_with(d, rnd, 30000, 0, 0, 0, 0).s);  // stamp zero
    TEST(!def->stamp && def->use3);
    while (def->stamp < 1023*STAMP_STEP) {
        tugz_deflate_reset(d, 1);
        free(tcompress_with(d, zeros, 2000, 0, 0, 0, 0).s);
    }
    tugz_deflate_reset(d, 1);
    free(tcompress_with(d, rnd, 1000000, 0, 0, 0, 0).s);
    TEST(def->use3);
    tugz_deflate_reset(d, 1);
    buf ref = tcompress(TUGZ_RAW, 1, abc, 40000, 0, 0, 0, 0);
    buf c = tcompress_with(d, abc, 40000, 0, 0, 0, 0);
    TEST(def->use3);
    TEST(same(c, ref.s, ref.len));
    free(c.s);
    free(ref.s);

    // Run the stamps out
    while (def->stamp != STAMP_LAST) {
        tugz_deflate_reset(d, 1);
        free(tcompress_with(d, zeros, 2000, 0, 0, 0, 0).s);
    }
    tugz_deflate_reset(d, 9);
    TEST(!def->stamp);
    for (i32 i = 0; i < HASH_SIZE; i++) {
        TEST(!def->head[i] && !def->head3[i]);
    }
    ref = tcompress(TUGZ_RAW, 9, rnd+40000, 30000, 0, 0, 0, 0);
    c = tcompress_with(d, rnd+40000, 30000, 0, 0, 0, 0);
    TEST(same(c, ref.s, ref.len));
    free(c.s);
    free(ref.s);

    // Long streams, with and without 3-byte matching
    for (i32 k = 0; k < 2; k++) {
        u8 *p = k ? abc : text;
        ref = tcompress(TUGZ_RAW, 1, p, n, 0, 0, 0, 0);
        tugz_deflate_reset(d, 1);
        TEST(def->stamp);
        c = tcompress_with(d, p, n, 0, 0, 0, 0);
        TEST(same(c, ref.s, ref.len));
        free(c.s);
        free(ref.s);
    }

    free(mem);
    free(zeros);
    free(abc);
    free(text);
    free(rnd);
}

// A reset inflator decodes as a new one, after its previous stream ended,
// failed, or was abandoned at any point: in a header, block, trailer,
// or the stash, or with output pending.
static void test_inflate_reset(void)
{
    iz n = 30000;
    u8 *text = textbytes(n, 12);
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        buf c = tcompress(format, 6, text, n, 0, 0, 0, 0);
        // Corrupt in the middle, and in the header (raw: block type 3)
        buf bad[2];
        for (i32 i = 0; i < 2; i++) {
            bad[i].s = malloc((uz)c.len);
            bad[i].len = c.len;
            memcpy(bad[i].s, c.s, (uz)c.len);
        }
        bad[0].s[c.len/2] ^= 0x10;
        bad[1].s[0] ^= format==TUGZ_RAW ? (~c.s[0] & 6) : 0x40;
        tugz_inflator *z;
        void *mem = mem_inflator(format, &z);
        u8 *out = malloc((uz)n);
        for (iz cut = 0; cut <= c.len; cut += cut<40 ? 1 : 97) {
            for (i32 k = 0; k < 4; k++) {
                buf prev = k<2 ? c : bad[k-2];
                tugz_buf b = {prev.s, cut, out, k==1 ? 100 : n};
                tugz_inflate_reset(z);
                int status = tugz_inflate(z, &b);
                if (k == 0) {
                    TEST(status == (cut==c.len ? TUGZ_DONE : TUGZ_NEED_INPUT));
                } else if (k == 1) {
                    TEST(status==TUGZ_NEED_INPUT || status==TUGZ_NEED_OUTPUT);
                } else if (k==3 && cut>=2) {
                    TEST(status < 0);
                }
                tugz_inflate_reset(z);
                result r = tdecompress_with(z, c.s, c.len, cut%5 ? 0 : 7,
                                            cut%3 ? 0 : 50);
                TEST(r.status==TUGZ_DONE && r.used==c.len);
                TEST(same(r.out, text, n));
                free(r.out.s);
            }
        }
        // A finished stream starts over, rather than staying done (or for
        // gzip, going on to the next member)
        tugz_buf b = {c.s, c.len, out, n};
        TEST(tugz_inflate(z, &b) == TUGZ_DONE);
        tugz_inflate_reset(z);
        b = (tugz_buf){c.s, c.len, out, n};
        TEST(tugz_inflate(z, &b) == TUGZ_DONE);
        TEST(!b.inlen && !b.outlen && !memcmp(out, text, (uz)n));
        free(out);
        free(mem);
        free(bad[1].s);
        free(bad[0].s);
        free(c.s);
    }
    tugz_inflate_reset(0);  // ignored
    free(text);
}

// Init leaves most of an inflator uncleared, so it must decode alike
// from memory holding anything: stored, fixed, and dynamic blocks, in
// one piece and byte by byte (through the stash), and after a reset.
static void test_inflate_init(void)
{
    iz n = 20000;
    u8 *text = textbytes(n, 13);
    u8 *noise = randbytes(n, 14);
    s8 const inputs[] = {
        {text, n}, {noise, n}, {(u8 *)"hello, hello", 12}, {text, 0},
    };
    static int const types[] = {2, 0, 1, 1};  // dynamic, stored, fixed
    static u8 const fills[] = {0, 0xff, 0xa5};
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        ptrdiff_t len = tugz_inflate_size(format);
        u8 *mem = malloc((uz)len);
        for (i32 i = 0; i < countof(inputs); i++) {
            s8 in = inputs[i];
            buf c = tcompress(format, 6, in.s, in.len, 0, 0, 0, 0);
            if (format == TUGZ_RAW) {
                TEST((c.s[0]>>1 & 3) == types[i]);  // first block's type
            }
            for (i32 fill = 0; fill <= countof(fills); fill++) {
                for (i32 pass = 0; pass < 3; pass++) {
                    if (fill < countof(fills)) {
                        memset(mem, fills[fill], (uz)len);
                    } else {
                        u8 *junk = randbytes(len, (u64)(i*3 + pass));
                        memcpy(mem, junk, (uz)len);
                        free(junk);
                    }
                    tugz_inflator *z = tugz_inflate_init(mem, len, format);
                    TEST(z);
                    if (pass == 2) {
                        // Leave a stream half done, then start over
                        u8 out[100];
                        tugz_buf b = {c.s, c.len/2, out, countof(out)};
                        TEST(tugz_inflate(z, &b) >= 0);
                        tugz_inflate_reset(z);
                    }
                    result r = tdecompress_with(z, c.s, c.len, pass==1, 0);
                    TEST(r.status==TUGZ_DONE && r.used==c.len);
                    TEST(same(r.out, in.s, in.len));
                    free(r.out.s);
                }
            }
            free(c.s);
        }
        free(mem);
    }
    free(noise);
    free(text);
}

// Init leaves a deflator's chain links and tokens uncleared, so it must
// compress alike from memory holding anything, though slides rebase
// links never written. In runs of zeros, level 1 inserts only the start
// of each 258-byte match, so only even chain slots are written, both
// with 3-byte matching (chosen by sampling noise and zeros) and without
// (all zeros). The last pass uses memory as malloc returns it, so that
// MemorySanitizer can check that unwritten links decide nothing.
static void test_deflate_init(void)
{
    iz n = 1300000;  // past the window
    u8 *p[2] = {randbytes(n, 23), calloc((uz)n, 1)};
    memset(p[0]+16384, 0, (uz)(n-16384));
    memset(p[1]+n/2, 'z', 1000);
    ptrdiff_t len = tugz_deflate_size(TUGZ_RAW);
    static u8 const fills[] = {0, 0xff, 0xa5};
    for (i32 k = 0; k < 2; k++) {
        buf ref = {0};
        for (i32 fill = 0; fill <= countof(fills)+1; fill++) {
            u8 *mem;
            if (fill < countof(fills)) {
                mem = malloc((uz)len);
                memset(mem, fills[fill], (uz)len);
            } else if (fill == countof(fills)) {
                mem = randbytes(len, (u64)(24 + k));
            } else {
                mem = malloc((uz)len);
            }
            tugz_deflator *d = tugz_deflate_init(mem, len, TUGZ_RAW, 1);
            TEST(d);
            buf c = tcompress_with(d, p[k], n, 0, 0, 0, 0);
            TEST(d->e->def->use3 == !k);
            free(mem);
            if (!fill) {
                ref = c;
                continue;
            }
            TEST(same(c, ref.s, ref.len));
            free(c.s);
        }
        free(ref.s);
        free(p[k]);
    }
}

// Inflate decodes ahead of the caller's output buffer into its window,
// but holds nothing back at TUGZ_NEED_INPUT or an error. So a caller who
// calls again only for TUGZ_NEED_OUTPUT (and for more input while there
// is some), as tugz.h says, gets from any prefix of a stream exactly
// what zlib does, whatever the buffer sizes: everything decoded before
// the input ran out, or before the error.
static void test_held_output(void)
{
    // An interactive message, SYNC-flushed: one call delivers what fits,
    // and the rest follows without more input
    iz n = 10000;
    u8 *msg = malloc((uz)n);
    memset(msg, 'x', (uz)n);
    tugz_deflator *d;
    void *mem = mem_deflator(TUGZ_RAW, 6, &d);
    u8 z[256];
    tugz_buf b = {msg, n, z, countof(z)};
    TEST(tugz_deflate(d, &b, TUGZ_SYNC) == TUGZ_DONE);
    iz zlen = countof(z) - b.outlen;
    free(mem);
    tugz_inflator *zs;
    mem = mem_inflator(TUGZ_RAW, &zs);
    u8 out[1024];
    b = (tugz_buf){z, zlen, 0, 0};
    iz got = 0;
    int status;
    do {
        b.out = out;
        b.outlen = countof(out);
        status = tugz_inflate(zs, &b);
        got += countof(out) - b.outlen;
    } while (status == TUGZ_NEED_OUTPUT);
    TEST(status==TUGZ_NEED_INPUT && got==n && !b.inlen);
    free(mem);
    free(msg);

    // Prefixes of streams, and of the same streams broken after a SYNC
    // flush by an invalid block (type 3), the input running out or the
    // error coming with the window holding over 100 KiB
    iz len = 150000;
    u8 *text = textbytes(len, 15);
    memset(text+20000, 0, 100000);
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        buf c[2] = {tcompress(format, 6, text, len, 0, 0, 0, 0)};
        mem = mem_deflator(format, 6, &d);
        c[1].s = malloc((uz)len);
        b = (tugz_buf){text, len, c[1].s, len};
        TEST(tugz_deflate(d, &b, TUGZ_SYNC) == TUGZ_DONE);
        c[1].len = b.out - c[1].s;
        c[1].s[c[1].len++] = 0x07;
        free(mem);

        for (i32 k = 0; k < 2; k++) {
            for (iz cut = 0; cut <= c[k].len; cut += 1 + cut/8) {
                int how;
                buf want = zlib_prefix(format, c[k].s, cut, &how);
                TEST(k==1 && cut==c[k].len ? how<0 : how>=0);
                static iz const outs[] = {7, 1000, 4096, 1<<16};
                for (i32 i = 0; i < countof(outs); i++) {
                    result r = tdecompress(format, c[k].s, cut, i&1 ? 3 : 0,
                                          outs[i]);
                    TEST(how ? how>0 ? r.status==TUGZ_DONE : r.status<0
                             : r.status==TUGZ_NEED_INPUT);
                    TEST(same(r.out, want.s, want.len));
                    free(r.out.s);
                }
                free(want.s);
            }
        }

        // The error stays, after the output before it
        tugz_inflator *zi;
        mem = mem_inflator(format, &zi);
        u8 *o = malloc((uz)len);
        b = (tugz_buf){c[1].s, c[1].len, o, 1000};
        TEST(tugz_inflate(zi, &b) == TUGZ_NEED_OUTPUT);
        b.outlen = len - (b.out - o);
        TEST(tugz_inflate(zi, &b) == TUGZ_EDATA);
        TEST(b.out-o==len && !memcmp(o, text, (uz)len));
        TEST(tugz_inflate(zi, &b) == TUGZ_EDATA);
        TEST(b.out-o == len);
        free(o);
        free(mem);
        free(c[1].s);
        free(c[0].s);
    }
    free(text);
}

static void test_usage(void)
{
    tugz_inflator *z;
    void *mem = mem_inflator(TUGZ_RAW, &z);
    tugz_buf b = {0, -1, 0, 0};
    TEST(tugz_inflate(z, &b) == TUGZ_EUSAGE);
    b = (tugz_buf){0, 1, 0, 0};
    TEST(tugz_inflate(z, &b) == TUGZ_EUSAGE);
    TEST(tugz_inflate(0, &b) == TUGZ_EUSAGE);
    b = (tugz_buf){0};
    TEST(tugz_inflate(z, &b) == TUGZ_NEED_INPUT);
    b = (tugz_buf){(u8 const *)"\x02", 1, 0, 0};  // a stash, then nulls
    TEST(tugz_inflate(z, &b) == TUGZ_NEED_INPUT);
    b = (tugz_buf){0};
    TEST(tugz_inflate(z, &b) == TUGZ_NEED_INPUT);
    free(mem);

    tugz_deflator *d;
    mem = mem_deflator(TUGZ_RAW, 6, &d);
    TEST(tugz_deflate(d, &b, 4) == TUGZ_EUSAGE);
    TEST(tugz_deflate(d, &b, TUGZ_NONE) == TUGZ_NEED_INPUT);
    free(mem);
}

int main(void)
{
    test_memory();
    test_allocator();
    test_usage();
    test_zlib_format();
    test_gzip_errors();
    test_members();
    test_stream_end();
    test_splits();
    test_flush();
    test_flush_switch();
    test_none_staging();
    test_staging_moves();
    test_roundtrip();
    test_large();
    test_reset();
    test_reset_stamps();
    test_inflate_reset();
    test_inflate_init();
    test_held_output();
    test_deflate_init();
    puts("all library tests pass");
    return 0;
}
