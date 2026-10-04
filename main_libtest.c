// Test suite for the tugz library interface (tugz.h)
// Exercises streaming with every buffer split, exact stream ends, all
// three formats, flushes, memory handling, and cross-checks with zlib.
// $ cc -g3 -fsanitize=address,undefined -o tests-lib main_libtest.c -lz
#include "libtugz.c"

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
    ptrdiff_t len = tugz_deflate_size(format, level);
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

// Compress through the library, feeding input and taking output in
// pieces (0 for unlimited). Flushes of the given mode are requested at
// every multiple of flushat input bytes (0 for none).
static buf tcompress(int format, int level, u8 const *p, iz len,
                    iz inpiece, iz outpiece, int fmode, iz flushat)
{
    tugz_deflator *d;
    void *mem = mem_deflator(format, level, &d);
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
    free(mem);
    return r;
}

typedef struct {
    int status;
    buf out;
    iz  used;  // input bytes consumed
} result;

// Decompress one stream (or gzip member) with pieces as for compress.
static result tdecompress(int format, u8 const *p, iz len, iz inpiece,
                         iz outpiece)
{
    tugz_inflator *z;
    void *mem = mem_inflator(format, &z);
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

static iz const pieces[] = {0, 1, 2, 3, 7, 16, 17, 100, 4096};

static void test_memory(void)
{
    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        ptrdiff_t ilen = tugz_inflate_size(format);
        ptrdiff_t dlen = tugz_deflate_size(format, 6);
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
    TEST(!tugz_deflate_size(-1, 6));
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
    TEST(d && st.allocs==1 && st.size==tugz_deflate_size(TUGZ_GZIP, 6));
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
    static u8 const dict[] = {0x78, 0xbb, 0, 0, 0, 1, 0x03, 0x00};
    TEST((dict[0]<<8 | dict[1]) % 31 == 0);
    TEST(zlib_header(dict, 8) == TUGZ_EHEADER);
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
    test_roundtrip();
    test_large();
    puts("all library tests pass");
    return 0;
}
