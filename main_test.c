// Test suite for gzip
// On success prints "all tests pass" and exits with status zero. A
// failure traps, so run under a debugger to examine it.
// $ cc -g3 -fsanitize=address,undefined -o tests main_test.c -lz -ldeflate
#include "src/base.c"
#include "src/crc32.c"
#include "src/inflate.c"
#include "src/deflate.c"
#include "src/gzip.c"
#include "src/cli.c"

#include <libdeflate.h>
#include <setjmp.h>
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

// In-memory file system

typedef struct {
    s8  name;
    u8 *data;
    iz  len;
    iz  cap;
    b32 live;
    b32 isdir;
    b32 issymlink;  // followed unless OS_NOFOLLOW
    b32 isspecial;  // e.g. FIFO or device
    i32 nlinks;     // extra hard links
    u32 mode;       // metadata copied by os_copymeta
    i64 mtime;
} mfile;

enum { MAX_FILES = 64, MAX_FDS = 16 };

struct os {
    jmp_buf fail;
    mfile   files[MAX_FILES];
    struct {
        i32 file;
        iz  off;
        b32 open;
        b32 created;  // discarded on close unless kept
        b32 keep;
    } fds[MAX_FDS];
    iz  readlimit;   // max bytes per read, to exercise short reads
    b32 failread;    // reads of non-standard descriptors fail
    b32 failwrite;   // writes to descriptors other than stderr fail
    b32 failclose;   // closing created files fails
    b32 tty[3];      // standard descriptors attached to a terminal
};

static s8 cstrs8(char *z)
{
    return (s8){(u8 *)z, (iz)strlen(z)};
}

static s8 dup8(s8 s)
{
    s8 r = {malloc((uz)s.len + 1), s.len};
    if (s.len) {
        memcpy(r.s, s.s, (uz)s.len);
    }
    return r;
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

static mfile *mfs_create(os *ctx, s8 name)
{
    mfile *f = mfs_find(ctx, name);
    if (!f) {
        for (i32 i = 0; i < MAX_FILES; i++) {
            if (!ctx->files[i].live) {
                f = ctx->files + i;
                free(f->name.s);
                free(f->data);
                *f = (mfile){0};
                f->name = dup8(name);
                f->live = 1;
                break;
            }
        }
    }
    TEST(f);
    f->len = 0;
    f->isdir = f->issymlink = f->isspecial = 0;
    f->nlinks = 0;
    f->mode = 0600;  // as created by a platform layer
    f->mtime = 0;
    return f;
}

static void mfs_append(mfile *f, u8 const *p, iz len)
{
    if (f->len+len > f->cap) {
        f->cap = MAX(f->cap*2, f->len+len);
        f->data = realloc(f->data, (uz)f->cap);
        TEST(f->data);
    }
    if (len) {
        memcpy(f->data+f->len, p, (uz)len);
    }
    f->len += len;
}

static void mfs_put(os *ctx, char *name, u8 const *p, iz len)
{
    mfile *f = mfs_create(ctx, cstrs8(name));
    mfs_append(f, p, len);
}

// Returns the contents of a file, or a null string if missing.
static s8 mfs_get(os *ctx, char *name)
{
    mfile *f = mfs_find(ctx, cstrs8(name));
    s8 r = {0};
    if (f) {
        r.s = f->data ? f->data : (u8 *)"";
        r.len = f->len;
    }
    return r;
}

static void mfs_reset(os *ctx)
{
    for (i32 i = 0; i < MAX_FILES; i++) {
        ctx->files[i].live = 0;
    }
    for (i32 i = 0; i < MAX_FDS; i++) {
        ctx->fds[i].open = 0;
    }
    ctx->readlimit = 0;
    ctx->failread = ctx->failwrite = ctx->failclose = 0;
    ctx->tty[0] = ctx->tty[1] = ctx->tty[2] = 0;
    static char *std[] = {"<stdin>", "<stdout>", "<stderr>"};
    for (i32 i = 0; i < 3; i++) {
        ctx->fds[i].file = (i32)(mfs_create(ctx, cstrs8(std[i])) - ctx->files);
        ctx->fds[i].off = 0;
        ctx->fds[i].open = 1;
    }
}

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    (void)scratch;
    i32 fd = 3;
    for (; fd<MAX_FDS && ctx->fds[fd].open; fd++) {}
    TEST(fd < MAX_FDS);

    mfile *f = mfs_find(ctx, path);
    b32 created = 0;
    if (mode & OS_CREATE) {
        if (f) {
            return OS_EEXIST;
        }
        f = mfs_create(ctx, path);
        created = 1;
    } else if (mode & OS_FORCE) {
        f = mfs_create(ctx, path);
        created = 1;
    } else if (!f) {
        return OS_ERR;
    } else if (f->isdir) {
        return OS_EISDIR;
    } else if ((mode & OS_NOFOLLOW) && f->issymlink) {
        return OS_ESYMLINK;
    } else if ((mode & OS_REGULAR) && f->isspecial) {
        return OS_ENOTREG;
    } else if ((mode & OS_ONELINK) && f->nlinks) {
        return OS_ELINKS;
    }
    ctx->fds[fd].file = (i32)(f - ctx->files);
    ctx->fds[fd].off = 0;
    ctx->fds[fd].open = 1;
    ctx->fds[fd].created = created;
    ctx->fds[fd].keep = 0;
    return fd;
}

static b32 os_close(os *ctx, i32 fd)
{
    TEST(fd>2 && fd<MAX_FDS && ctx->fds[fd].open);
    ctx->fds[fd].open = 0;
    if (ctx->fds[fd].created) {
        if (!ctx->fds[fd].keep) {
            ctx->files[ctx->fds[fd].file].live = 0;
        }
        return !ctx->failclose;
    }
    return 1;
}

static void os_keep(os *ctx, i32 fd)
{
    TEST(fd>2 && fd<MAX_FDS && ctx->fds[fd].open && ctx->fds[fd].created);
    ctx->fds[fd].keep = 1;
}

static b32 os_isatty(os *ctx, i32 fd)
{
    TEST(fd>=0 && fd<3);
    return ctx->tty[fd];
}

static void os_copymeta(os *ctx, i32 from, i32 to)
{
    TEST(ctx->fds[from].open && ctx->fds[to].open && ctx->fds[to].created);
    mfile *src = ctx->files + ctx->fds[from].file;
    mfile *dst = ctx->files + ctx->fds[to].file;
    dst->mode  = src->mode;
    dst->mtime = src->mtime;
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    TEST(fd>=0 && fd<MAX_FDS && ctx->fds[fd].open);
    TEST(cap > 0);
    if (ctx->failread && fd>2) {
        return -1;
    }
    mfile *f = ctx->files + ctx->fds[fd].file;
    iz n = MIN(cap, f->len - ctx->fds[fd].off);
    if (ctx->readlimit) {
        n = MIN(n, ctx->readlimit);
    }
    if (n) {
        memcpy(buf, f->data + ctx->fds[fd].off, (uz)n);
    }
    ctx->fds[fd].off += n;
    return n;
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    TEST(fd>=0 && fd<MAX_FDS && ctx->fds[fd].open);
    TEST(fd != 0);
    if (ctx->failwrite && fd!=2) {
        return 0;
    }
    mfs_append(ctx->files + ctx->fds[fd].file, buf, len);
    return 1;
}

static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)scratch;
    mfile *f = mfs_find(ctx, path);
    if (f) {
        f->live = 0;
    }
    return !!f;
}

static void os_fail(os *ctx)
{
    longjmp(ctx->fail, 1);
}

// Test helpers

static u64 rng_state = 1;

static u32 rand32(void)
{
    rng_state = rng_state*0x3243f6a8885a308d + 1;
    return (u32)(rng_state >> 32);
}

static u8 *randbytes(iz len, i32 kind)
{
    u8 *p = malloc((uz)len + 1);
    static char const *words[] = {
        "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ",
        "dog ", "compression ", "deflate ", "inflate ", "\n", "gzip ",
        "huffman ", "window ", "a ", "of ", "and ", "1234 ", "{}; ",
    };
    switch (kind) {
    case 0:  // zeros
        memset(p, 0, (uz)len);
        break;
    case 1:  // random
        for (iz i = 0; i < len; i++) {
            p[i] = (u8)rand32();
        }
        break;
    case 2:  // text
        for (iz i = 0; i < len;) {
            char const *w = words[rand32() % countof(words)];
            for (; *w && i<len; w++) {
                p[i++] = (u8)*w;
            }
        }
        break;
    case 3:  // short-period repetition with occasional noise
        {
            iz period = 1 + rand32()%300;
            for (iz i = 0; i < len; i++) {
                p[i] = i<period ? (u8)rand32() : p[i-period];
                if (rand32()%1000 == 0) {
                    p[i] = (u8)rand32();
                }
            }
        }
        break;
    case 4:  // geometric symbol distribution, forcing long codes
        for (iz i = 0; i < len; i++) {
            u32 r = rand32();
            p[i] = (u8)(r ? __builtin_ctz(r) : 32);
        }
        break;
    case 5:  // small alphabet
        for (iz i = 0; i < len; i++) {
            p[i] = (u8)("ab"[rand32()%2]);
        }
        break;
    }
    return p;
}

static void set_stdin(os *ctx, u8 const *p, iz len)
{
    mfs_put(ctx, "<stdin>", p, len);
    mfs_put(ctx, "<stdout>", 0, 0);
    mfs_put(ctx, "<stderr>", 0, 0);
    ctx->fds[0].off = 0;
}

static s8 get_stdout(os *ctx)
{
    return dup8(mfs_get(ctx, "<stdout>"));
}

static i32 do_gzip(os *ctx, arena a, u8 const *p, iz len, i32 level, s8 *out)
{
    set_stdin(ctx, p, len);
    i32 status = gzip_compress(0, 1, level, a);
    *out = get_stdout(ctx);
    return status;
}

static i32 do_gunzip(os *ctx, arena a, u8 const *p, iz len, s8 *out)
{
    set_stdin(ctx, p, len);
    i32 status = gzip_decompress(0, 1, a);
    *out = get_stdout(ctx);
    return status;
}

static i32 do_inflate(os *ctx, arena a, u8 const *p, iz len, s8 *out)
{
    set_stdin(ctx, p, len);
    reader  *r = newreader(&a, 0, 1<<16);
    writer  *w = newwriter(&a, 1, 1<<16);
    inflator *s = inflate_new(&a, r, w);
    i32 status = inflate_run(s);
    writer_flush(w);
    *out = get_stdout(ctx);
    return status;
}

// Deflate in pieces of the given size (0 for one piece).
static s8 do_deflate(os *ctx, arena a, u8 const *p, iz len, i32 level,
                     iz piece, deflator **dp)
{
    set_stdin(ctx, 0, 0);
    writer  *w = newwriter(&a, 1, 1<<16);
    deflator *d = deflate_new(&a, level, w);
    if (dp) {
        *dp = d;
    }
    for (iz off = 0; off < len;) {
        iz n = piece ? MIN(piece, len-off) : len-off;
        deflate_push(d, p+off, n);
        off += n;
    }
    deflate_finish(d);
    writer_flush(w);
    TEST(!w->err);
    return get_stdout(ctx);
}

static b32 equals(s8 a, u8 const *p, iz len)
{
    return a.len==len && (!len || !memcmp(a.s, p, (uz)len));
}

// zlib decompression: windowBits 31 for gzip, -15 for raw
static b32 zlib_inflate(u8 const *p, iz len, i32 wbits, s8 *out)
{
    z_stream z = {0};
    TEST(inflateInit2(&z, wbits) == Z_OK);
    iz cap = 1 << 16;
    out->s = malloc((uz)cap);
    out->len = 0;
    z.next_in = (u8 *)p;
    z.avail_in = (u32)len;
    i32 r;
    do {
        if (out->len == cap) {
            cap *= 2;
            out->s = realloc(out->s, (uz)cap);
        }
        z.next_out = out->s + out->len;
        z.avail_out = (u32)(cap - out->len);
        r = inflate(&z, Z_NO_FLUSH);
        out->len = cap - z.avail_out;
    } while (r == Z_OK);
    inflateEnd(&z);
    return r == Z_STREAM_END;
}

static s8 zlib_deflate(u8 const *p, iz len, i32 level, i32 wbits,
                       i32 strategy)
{
    z_stream z = {0};
    TEST(deflateInit2(&z, level, Z_DEFLATED, wbits, 8, strategy) == Z_OK);
    s8 r = {0};
    r.len = (iz)deflateBound(&z, (uLong)len);
    r.s = malloc((uz)r.len);
    z.next_in = (u8 *)p;
    z.avail_in = (u32)len;
    z.next_out = r.s;
    z.avail_out = (u32)r.len;
    TEST(deflate(&z, Z_FINISH) == Z_STREAM_END);
    r.len = (iz)z.total_out;
    deflateEnd(&z);
    return r;
}

// Bit-level stream builder for hand-crafted DEFLATE streams

typedef struct {
    u8  buf[1<<12];
    iz  len;
    i32 nbits;
} bits;

static void bput(bits *b, u32 v, i32 n)
{
    for (i32 i = 0; i < n; i++, b->nbits++) {
        if (!(b->nbits & 7)) {
            b->buf[b->len++] = 0;
        }
        b->buf[b->len-1] |= (u8)((v>>i & 1) << (b->nbits & 7));
    }
}

// Write a Huffman code most-significant bit first.
static void bcode(bits *b, u32 code, i32 len)
{
    for (i32 i = len-1; i >= 0; i--) {
        bput(b, code>>i & 1, 1);
    }
}

static void canonical(u8 const *lens, i32 n, u32 *codes)
{
    u32 count[16] = {0};
    for (i32 i = 0; i < n; i++) {
        count[lens[i]]++;
    }
    count[0] = 0;
    u32 next[16] = {0};
    for (i32 b = 1, code = 0; b < 16; b++) {
        code = (code + (i32)count[b-1]) << 1;
        next[b] = (u32)code;
    }
    for (i32 i = 0; i < n; i++) {
        codes[i] = lens[i] ? next[lens[i]]++ : 0;
    }
}

static void bfixed(bits *b, i32 sym)
{
    if (sym < 144) {
        bcode(b, 0x30 + (u32)sym, 8);
    } else if (sym < 256) {
        bcode(b, 0x190 + (u32)sym - 144, 9);
    } else if (sym < 280) {
        bcode(b, (u32)sym - 256, 7);
    } else {
        bcode(b, 0xc0 + (u32)sym - 280, 8);
    }
}

// Dynamic block header with code length code {0..15: length 4}.
static void bdynamic(bits *b, b32 final, i32 hlit, i32 hdist, u8 const *lens)
{
    bput(b, (u32)final, 1);
    bput(b, 2, 2);
    bput(b, (u32)(hlit - 257), 5);
    bput(b, (u32)(hdist - 1), 5);
    bput(b, 19 - 4, 4);
    for (i32 i = 0; i < 19; i++) {
        bput(b, inf_cl_order[i]<16 ? 4 : 0, 3);
    }
    for (i32 i = 0; i < hlit+hdist; i++) {
        bcode(b, lens[i], 4);
    }
}

typedef struct {
    i32 hlit, hdist;
    u8  lens[286+30];
    u32 lcodes[286];
    u32 dcodes[30];
} dyncode;

static void dyn_begin(bits *b, dyncode *c)
{
    bdynamic(b, 1, c->hlit, c->hdist, c->lens);
    canonical(c->lens, c->hlit, c->lcodes);
    canonical(c->lens+c->hlit, c->hdist, c->dcodes);
}

static void dyn_lit(bits *b, dyncode *c, i32 sym)
{
    TEST(c->lens[sym]);
    bcode(b, c->lcodes[sym], c->lens[sym]);
}

static void dyn_dist(bits *b, dyncode *c, i32 sym)
{
    TEST(c->lens[c->hlit+sym]);
    bcode(b, c->dcodes[sym], c->lens[c->hlit+sym]);
}

static i32 inflate_bits(os *ctx, arena a, bits *b, s8 *out)
{
    return do_inflate(ctx, a, b->buf, b->len, out);
}

// Tests

static void test_tables(void)
{
    u32 table[256];
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (i32 k = 0; k < 8; k++) {
            c = c&1 ? 0xedb88320u ^ c>>1 : c>>1;
        }
        table[n] = c;
    }
    TEST(!memcmp(table, crc32_table[0], sizeof(table)));
    for (i32 k = 1; k < 8; k++) {
        for (i32 n = 0; n < 256; n++) {
            u32 want = crc32_table[k-1][n]>>8 ^ table[crc32_table[k-1][n]&0xff];
            TEST(crc32_table[k][n] == want);
        }
    }
    for (i32 len = 0; len < 64; len++) {
        u8 buf[64];
        for (i32 i = 0; i < len; i++) {
            buf[i] = (u8)(i*37 + len);
        }
        TEST(crc32_update(0, buf, len) == (u32)crc32(0, buf, (uInt)len));
        TEST(crc32_slice8(0, buf, len) == (u32)crc32(0, buf, (uInt)len));
    }
    TEST(crc32_update(0, (u8 *)"123456789", 9) == 0xcbf43926);
    TEST(crc32_update(0, 0, 0) == 0);

    u8 *p = randbytes(100000, 1);
    u32 whole = crc32_update(0, p, 100000);
    TEST(whole == (u32)crc32(0, p, 100000));
    TEST(whole == crc32_slice8(0, p, 100000));
    u32 parts = crc32_update(crc32_update(0, p, 33333), p+33333, 66667);
    TEST(whole == parts);
    free(p);

    for (i32 len = 3; len <= 258; len++) {
        i32 c = length_code(len);
        TEST(c>=0 && c<29);
        if (len == 258) {
            TEST(c == 28);
        } else {
            TEST(len >= def_len_base[c]);
            TEST(len < def_len_base[c] + (1<<def_len_extra[c]));
        }
    }
    for (i32 dist = 1; dist <= 32768; dist++) {
        i32 c = dist_code(dist);
        TEST(c>=0 && c<30);
        TEST(dist >= def_dist_base[c]);
        TEST(dist < def_dist_base[c] + (1<<def_dist_extra[c]));
    }
}

// Reference unlimited Huffman cost via repeated minimum extraction.
static u64 huffman_cost(u32 const *freq, i32 n)
{
    u64 w[600];
    i32 m = 0;
    for (i32 i = 0; i < n; i++) {
        if (freq[i]) {
            w[m++] = freq[i];
        }
    }
    u64 cost = 0;
    while (m > 1) {
        for (i32 k = 0; k < 2; k++) {
            i32 lo = k;
            for (i32 i = k; i < m; i++) {
                lo = w[i]<w[lo] ? i : lo;
            }
            u64 t = w[k]; w[k] = w[lo]; w[lo] = t;
        }
        u64 sum = w[0] + w[1];
        cost += sum;
        w[0] = sum;
        w[1] = w[--m];
    }
    return cost;
}

static void check_tree(htree *t, u32 const *freq, i32 n, i32 maxlen)
{
    u64 kraft = 0;
    i32 used = 0;
    for (i32 i = 0; i < n; i++) {
        TEST(t->len[i] <= maxlen);
        TEST(!freq[i] || t->len[i]);
        if (t->len[i]) {
            kraft += (u64)1 << (maxlen - t->len[i]);
            used++;
        }
    }
    TEST(used >= 2);
    TEST(kraft == (u64)1<<maxlen);

    // Codes must be prefix-free and canonical: verify via the inflater's
    // table builder, which enforces zlib's rules.
    u16 lens[288];
    for (i32 i = 0; i < n; i++) {
        lens[i] = t->len[i];
    }
    htable h;
    static u32 entries[1<<15];
    TEST(htable_build(&h, entries, countof(entries), lens, n, HUFF_CODELEN,
                      LIT_ROOT));
}

static void test_huffman(void)
{
    // Edge cases: no symbols, one symbol
    for (i32 n = 2; n <= 288; n += 286) {
        u32 freq[288] = {0};
        htree t;
        huff_build(&t, freq, n, 15);
        check_tree(&t, freq, n, 15);
        freq[n-1] = 1000;
        huff_build(&t, freq, n, 15);
        check_tree(&t, freq, n, 15);
    }

    // Fibonacci weights force maximal depth
    for (i32 maxlen = 7; maxlen <= 15; maxlen += 8) {
        i32 n = maxlen==7 ? 19 : 286;
        u32 freq[288] = {0};
        u32 a = 1, b = 1;
        for (i32 i = 0; i < n && i < 40; i++) {
            freq[i] = a;
            u32 t = a + b;
            a = b;
            b = t;
        }
        htree t;
        huff_build(&t, freq, n, maxlen);
        check_tree(&t, freq, n, maxlen);
    }

    // Random weights: complete, limited, and optimal when unconstrained
    for (i32 trial = 0; trial < 2000; trial++) {
        i32 n = 2 + (i32)(rand32() % 287);
        i32 maxlen = trial&1 ? 15 : 7;
        if (maxlen == 7) {
            n = MIN(n, 19);
        }
        u32 freq[288] = {0};
        i32 shape = (i32)(rand32() % 3);
        for (i32 i = 0; i < n; i++) {
            u32 r = rand32();
            switch (shape) {
            case 0: freq[i] = r % 100; break;
            case 1: freq[i] = r % 4 ? 0 : 1 + r%100000; break;
            case 2: freq[i] = 1u << (r % 20); break;
            }
        }
        htree t;
        huff_build(&t, freq, n, maxlen);
        check_tree(&t, freq, n, maxlen);

        i32 nz = 0, deepest = 0;
        u64 cost = 0;
        for (i32 i = 0; i < n; i++) {
            nz += freq[i] != 0;
            cost += (u64)freq[i] * t.len[i];
            deepest = MAX(deepest, t.len[i]);
        }
        if (nz>=2 && deepest<maxlen) {
            TEST(cost == huffman_cost(freq, n));
        }
    }
}

static void test_inflate_vectors(os *ctx, arena a)
{
    s8 out;
    bits b;

    // Stored block
    static u8 const stored[] = {1, 3, 0, 0xfc, 0xff, 'a', 'b', 'c'};
    TEST(do_inflate(ctx, a, stored, countof(stored), &out) == GZ_OK);
    TEST(equals(out, (u8 *)"abc", 3));
    free(out.s);

    // Empty stored block
    static u8 const empty[] = {1, 0, 0, 0xff, 0xff};
    TEST(do_inflate(ctx, a, empty, countof(empty), &out) == GZ_OK);
    TEST(out.len == 0);
    free(out.s);

    // NLEN mismatch
    static u8 const badlen[] = {1, 3, 0, 0xfc, 0xfe, 'a', 'b', 'c'};
    TEST(do_inflate(ctx, a, badlen, countof(badlen), &out) == GZ_EDATA);
    free(out.s);

    // Truncated stored data
    TEST(do_inflate(ctx, a, stored, countof(stored)-1, &out) == GZ_ETRUNC);
    free(out.s);

    // Reserved block type
    static u8 const type3[] = {7};
    TEST(do_inflate(ctx, a, type3, 1, &out) == GZ_EDATA);
    free(out.s);

    // Empty input
    TEST(do_inflate(ctx, a, 0, 0, &out) == GZ_ETRUNC);
    free(out.s);

    // Fixed: "a" plus a match of length 3 at distance 1
    b = (bits){0};
    bput(&b, 1, 1);
    bput(&b, 1, 2);
    bfixed(&b, 'a');
    bfixed(&b, 257);
    bcode(&b, 0, 5);
    bfixed(&b, 256);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_OK);
    TEST(equals(out, (u8 *)"aaaa", 4));
    free(out.s);

    // Fixed: run of a high byte (regression: signed overflow in the
    // distance-1 fill)
    b = (bits){0};
    bput(&b, 1, 1);
    bput(&b, 1, 2);
    bfixed(&b, 0xff);
    bfixed(&b, 265);  // length 11..12
    bput(&b, 1, 1);
    bcode(&b, 0, 5);
    bfixed(&b, 256);
    b.len += 32;  // trailing padding, ignored, to engage the fast path
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_OK);
    TEST(out.len == 13);
    for (iz i = 0; i < out.len; i++) {
        TEST(out.s[i] == 0xff);
    }
    free(out.s);

    // Fixed: distance too far back
    b = (bits){0};
    bput(&b, 1, 1);
    bput(&b, 1, 2);
    bfixed(&b, 'a');
    bfixed(&b, 257);
    bcode(&b, 1, 5);
    bfixed(&b, 256);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // Fixed: invalid literal/length symbols 286 and 287
    for (i32 sym = 286; sym <= 287; sym++) {
        b = (bits){0};
        bput(&b, 1, 1);
        bput(&b, 1, 2);
        bfixed(&b, 'a');
        bfixed(&b, sym);
        bcode(&b, 0, 5);
        bfixed(&b, 256);
        TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
        free(out.s);
    }

    // Fixed: invalid distance symbols 30 and 31
    for (u32 sym = 30; sym <= 31; sym++) {
        b = (bits){0};
        bput(&b, 1, 1);
        bput(&b, 1, 2);
        bfixed(&b, 'a');
        bfixed(&b, 257);
        bcode(&b, sym, 5);
        bfixed(&b, 256);
        TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
        free(out.s);
    }

    // Fixed: length 258 via symbol 285 and via 284 + 31 (zlib allows)
    for (i32 v = 0; v < 2; v++) {
        b = (bits){0};
        bput(&b, 1, 1);
        bput(&b, 1, 2);
        bfixed(&b, 'z');
        if (v) {
            bfixed(&b, 284);
            bput(&b, 31, 5);
        } else {
            bfixed(&b, 285);
        }
        bcode(&b, 0, 5);
        bfixed(&b, 256);
        TEST(inflate_bits(ctx, a, &b, &out) == GZ_OK);
        TEST(out.len == 259);
        free(out.s);
    }

    // Fixed: maximum distance 32768
    {
        b = (bits){0};
        bput(&b, 0, 1);
        bput(&b, 0, 2);
        static u8 data[32768];
        for (i32 i = 0; i < 32768; i++) {
            data[i] = (u8)(i * 7);
        }
        // Stored block with 32768 bytes needs no alignment padding here
        // since the header is at a byte boundary.
        b.buf[0] = 0;
        b.len = 1;
        b.nbits = 8;
        bput(&b, 32768, 16);
        bput(&b, 32767, 16);
        bits big = b;
        u8 *stream = malloc(40000);
        memcpy(stream, big.buf, (uz)big.len);
        memcpy(stream+big.len, data, 32768);
        iz len = big.len + 32768;
        b = (bits){0};
        bput(&b, 1, 1);
        bput(&b, 1, 2);
        bfixed(&b, 257);
        bcode(&b, 29, 5);
        bput(&b, 8191, 13);
        bfixed(&b, 256);
        memcpy(stream+len, b.buf, (uz)b.len);
        len += b.len;
        TEST(do_inflate(ctx, a, stream, len, &out) == GZ_OK);
        TEST(out.len == 32771);
        TEST(!memcmp(out.s, data, 32768));
        TEST(!memcmp(out.s+32768, data, 3));
        free(out.s);

        // One byte short of history: distance too far
        stream[1] = 0xff; stream[2] = 0x7f;  // LEN 32767
        stream[3] = 0x00; stream[4] = 0x80;
        memmove(stream+5+32767, stream+5+32768, (uz)b.len);
        TEST(do_inflate(ctx, a, stream, len-1, &out) == GZ_EDATA);
        free(out.s);
        free(stream);
    }

    // Dynamic block, valid
    dyncode c = {0};
    c.hlit = 258;
    c.hdist = 1;
    c.lens['a'] = 1;
    c.lens[256] = 2;
    c.lens[257] = 2;
    c.lens[c.hlit+0] = 1;  // lone 1-bit distance code is allowed
    b = (bits){0};
    dyn_begin(&b, &c);
    dyn_lit(&b, &c, 'a');
    dyn_lit(&b, &c, 257);
    dyn_dist(&b, &c, 0);
    dyn_lit(&b, &c, 256);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_OK);
    TEST(equals(out, (u8 *)"aaaa", 4));
    free(out.s);

    // Lone 1-bit distance code, unused codeword decoded
    b = (bits){0};
    dyn_begin(&b, &c);
    dyn_lit(&b, &c, 'a');
    dyn_lit(&b, &c, 257);
    bcode(&b, 1, 1);
    dyn_lit(&b, &c, 256);
    TEST(inflate_bits(ctx, a, &b, &out) != GZ_OK);
    free(out.s);

    // Empty distance code is fine when unused
    c.lens[c.hlit+0] = 0;
    b = (bits){0};
    dyn_begin(&b, &c);
    dyn_lit(&b, &c, 'a');
    dyn_lit(&b, &c, 256);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_OK);
    TEST(equals(out, (u8 *)"a", 1));
    free(out.s);

    // ... but not when used
    b = (bits){0};
    dyn_begin(&b, &c);
    dyn_lit(&b, &c, 'a');
    dyn_lit(&b, &c, 257);
    bput(&b, 0, 16);
    TEST(inflate_bits(ctx, a, &b, &out) != GZ_OK);
    free(out.s);

    // Lone 1-bit literal/length code (end-of-block only)
    dyncode e = {0};
    e.hlit = 257;
    e.hdist = 1;
    e.lens[256] = 1;
    b = (bits){0};
    dyn_begin(&b, &e);
    dyn_lit(&b, &e, 256);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_OK);
    TEST(out.len == 0);
    free(out.s);

    // Incomplete literal/length code
    e.lens['a'] = 2;
    b = (bits){0};
    dyn_begin(&b, &e);
    bput(&b, 0, 16);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // Over-subscribed literal/length code
    e.lens['a'] = 1;
    e.lens['b'] = 1;
    b = (bits){0};
    dyn_begin(&b, &e);
    bput(&b, 0, 16);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // Missing end-of-block code
    dyncode m = {0};
    m.hlit = 257;
    m.hdist = 1;
    m.lens['a'] = 1;
    m.lens['b'] = 1;
    b = (bits){0};
    dyn_begin(&b, &m);
    bput(&b, 0, 16);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // Incomplete distance code with more than one codeword
    c.lens[c.hlit+0] = 2;
    c.lens[c.hlit+1] = 2;
    c.hdist = 2;
    b = (bits){0};
    dyn_begin(&b, &c);
    bput(&b, 0, 16);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // HLIT > 286 and HDIST > 30
    for (i32 v = 0; v < 2; v++) {
        b = (bits){0};
        bput(&b, 1, 1);
        bput(&b, 2, 2);
        bput(&b, v ? 0 : 30, 5);
        bput(&b, v ? 30 : 0, 5);
        bput(&b, 0, 32);
        TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
        free(out.s);
    }

    // HDIST = 30 is the maximum allowed
    dyncode h = {0};
    h.hlit = 286;
    h.hdist = 30;
    h.lens['x'] = 1;
    h.lens[256] = 1;
    h.lens[286+29] = 1;
    b = (bits){0};
    dyn_begin(&b, &h);
    dyn_lit(&b, &h, 'x');
    dyn_lit(&b, &h, 256);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_OK);
    TEST(equals(out, (u8 *)"x", 1));
    free(out.s);

    // Code length code: incomplete, over-subscribed, empty
    static u8 const clcases[][4] = {
        {0, 0, 0, 1},  // only symbol 0 with length 1: incomplete
        {1, 1, 1, 0},  // three 1-bit codes: over-subscribed
        {0, 0, 0, 0},  // empty
    };
    for (i32 i = 0; i < countof(clcases); i++) {
        b = (bits){0};
        bput(&b, 1, 1);
        bput(&b, 2, 2);
        bput(&b, 0, 5);
        bput(&b, 0, 5);
        bput(&b, 0, 4);
        for (i32 j = 0; j < 4; j++) {
            bput(&b, clcases[i][j], 3);
        }
        bput(&b, 0, 32);
        TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
        free(out.s);
    }

}

typedef struct {
    i32 sym;
    u32 extra;
} clop;

// Dynamic block header with arbitrary code length code lengths (indexed
// by symbol) and an explicit sequence of code length symbols.
static void bheader(bits *b, i32 hlit, i32 hdist, u8 const *cl,
                    clop const *ops, i32 nops)
{
    bput(b, 1, 1);
    bput(b, 2, 2);
    bput(b, (u32)(hlit - 257), 5);
    bput(b, (u32)(hdist - 1), 5);
    bput(b, 19 - 4, 4);
    for (i32 i = 0; i < 19; i++) {
        bput(b, cl[inf_cl_order[i]], 3);
    }
    u32 codes[19];
    canonical(cl, 19, codes);
    for (i32 i = 0; i < nops; i++) {
        i32 sym = ops[i].sym;
        bcode(b, codes[sym], cl[sym]);
        bput(b, ops[i].extra, cl_extra_bits(sym));
    }
}

static void test_inflate_repeats(os *ctx, arena a)
{
    bits b;
    s8 out;

    // Code length code: 1 -> "0", 17 -> "10", 18 -> "11"
    u8 cl[19] = {0};
    cl[1]  = 1;
    cl[17] = 2;
    cl[18] = 2;

    // A zero run spanning the literal/length and distance lengths:
    // hlit 258, hdist 3, 'a' = 1, 256 = 1, {257, d0, d1} = 0, d2 = 1
    clop const span[] = {
        {18, 97-11}, {1, 0}, {18, 138-11}, {18, 20-11}, {1, 0},
        {17, 0}, {1, 0},
    };
    b = (bits){0};
    bheader(&b, 258, 3, cl, span, countof(span));
    bcode(&b, 0, 1);  // 'a'
    bcode(&b, 0, 1);  // 'a'
    bcode(&b, 1, 1);  // end of block
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_OK);
    TEST(equals(out, (u8 *)"aa", 2));
    free(out.s);

    // The same, but the final run overruns the lengths
    clop const over[] = {
        {18, 97-11}, {1, 0}, {18, 138-11}, {18, 20-11}, {1, 0},
        {17, 0}, {17, 0},
    };
    b = (bits){0};
    bheader(&b, 258, 3, cl, over, countof(over));
    bput(&b, 0, 16);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // A maximal run overrunning the lengths
    clop const over18[] = {{18, 127}, {18, 127}};
    b = (bits){0};
    bheader(&b, 257, 1, cl, over18, countof(over18));
    bput(&b, 0, 16);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // Repeat-previous (16) with no previous length
    u8 cl16[19] = {0};
    cl16[0]  = 1;
    cl16[16] = 1;
    clop const first16[] = {{16, 0}};
    b = (bits){0};
    bheader(&b, 257, 1, cl16, first16, countof(first16));
    bput(&b, 0, 32);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // Code length code: {0, 1}: 3 bits, {2, 16, 18}: 2 bits
    u8 clr[19] = {0};
    clr[0]  = 3;
    clr[1]  = 3;
    clr[2]  = 2;
    clr[16] = 2;
    clr[18] = 2;
    // Literal/length code {'a': 1, 255: 2, 256: 2} is complete. A
    // repeat-previous (16) carries 256's length into the distances.
    clop const rep[] = {
        {18, 97-11}, {1, 0}, {18, 138-11}, {18, 19-11}, {2, 0},
        {16, 0}, {2, 0}, {0, 0},
    };
    // d0..d2 = 2, d3 = 0: incomplete distance code
    b = (bits){0};
    bheader(&b, 257, 4, clr, rep, countof(rep));
    bput(&b, 0, 16);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // d0..d6 = 2: over-subscribed distance code
    clop const rep2[] = {
        {18, 97-11}, {1, 0}, {18, 138-11}, {18, 19-11}, {2, 0},
        {16, 0}, {16, 1}, {2, 0},
    };
    b = (bits){0};
    bheader(&b, 257, 7, clr, rep2, countof(rep2));
    bput(&b, 0, 16);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
    free(out.s);

    // d0..d3 = 2: complete, valid
    clop const rep3[] = {
        {18, 97-11}, {1, 0}, {18, 138-11}, {18, 19-11}, {2, 0},
        {16, 0}, {2, 0}, {2, 0},
    };
    b = (bits){0};
    bheader(&b, 257, 4, clr, rep3, countof(rep3));
    bcode(&b, 2, 2);  // 255 is "10"
    bcode(&b, 0, 1);  // 'a' is "0"
    bcode(&b, 3, 2);  // 256 is "11"
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_OK);
    TEST(equals(out, (u8 *)"\xff" "a", 2));
    free(out.s);
}

static void test_inflate_zlib(os *ctx, arena a)
{
    static i32 const strategies[] = {
        Z_DEFAULT_STRATEGY, Z_FILTERED, Z_HUFFMAN_ONLY, Z_RLE, Z_FIXED,
    };
    static iz const sizes[] = {0, 1, 100, 5000, 70000, 300000};
    for (i32 kind = 0; kind < 6; kind++) {
        for (i32 si = 0; si < countof(sizes); si++) {
            iz len = sizes[si];
            u8 *p = randbytes(len, kind);
            for (i32 level = 0; level <= 9; level += 3) {
                for (i32 st = 0; st < countof(strategies); st++) {
                    s8 z = zlib_deflate(p, len, level, -15, strategies[st]);
                    s8 out;
                    TEST(do_inflate(ctx, a, z.s, z.len, &out) == GZ_OK);
                    TEST(equals(out, p, len));
                    free(out.s);

                    // Every proper prefix must fail
                    if (len <= 5000) {
                        for (iz n = 0; n < z.len; n++) {
                            TEST(do_inflate(ctx, a, z.s, n, &out) != GZ_OK);
                            free(out.s);
                        }
                    }
                    free(z.s);

                    z = zlib_deflate(p, len, level, 31, strategies[st]);
                    TEST(do_gunzip(ctx, a, z.s, z.len, &out) == GZ_OK);
                    TEST(equals(out, p, len));
                    free(out.s);
                    free(z.s);
                }
            }

            // libdeflate levels 0..12
            for (i32 level = 0; level <= 12; level++) {
                struct libdeflate_compressor *c =
                    libdeflate_alloc_compressor(level);
                uz cap = libdeflate_gzip_compress_bound(c, (uz)len);
                u8 *z = malloc(cap);
                uz zlen = libdeflate_gzip_compress(c, p, (uz)len, z, cap);
                TEST(zlen);
                s8 out;
                TEST(do_gunzip(ctx, a, z, (iz)zlen, &out) == GZ_OK);
                TEST(equals(out, p, len));
                free(out.s);
                free(z);
                libdeflate_free_compressor(c);
            }
            free(p);
        }
    }
}

static void check_roundtrip(os *ctx, arena a, u8 *p, iz len, i32 level)
{
    s8 gz, out;
    TEST(do_gzip(ctx, a, p, len, level, &gz) == GZ_OK);

    TEST(do_gunzip(ctx, a, gz.s, gz.len, &out) == GZ_OK);
    TEST(equals(out, p, len));
    free(out.s);

    TEST(zlib_inflate(gz.s, gz.len, 31, &out));
    TEST(equals(out, p, len));
    free(out.s);

    struct libdeflate_decompressor *dd = libdeflate_alloc_decompressor();
    u8 *buf = malloc((uz)len + 1);
    uz actual = 0;
    TEST(libdeflate_gzip_decompress(dd, gz.s, (uz)gz.len, buf, (uz)len,
                                    &actual) == LIBDEFLATE_SUCCESS);
    TEST(actual==(uz)len && (!len || !memcmp(buf, p, (uz)len)));
    free(buf);
    libdeflate_free_decompressor(dd);
    free(gz.s);
}

static void test_roundtrip(os *ctx, arena a)
{
    static iz const sizes[] = {
        0, 1, 2, 3, 4, 5, 257, 258, 259, 32767, 32768, 32769, 65534,
        65535, 65536, 65537, 131070, 131071,
        WIN_CAP - LOOKAHEAD - 1, WIN_CAP - LOOKAHEAD, WIN_CAP - 1, WIN_CAP,
        WIN_CAP + 1, 2*WIN_CAP + 12345,
    };
    for (i32 kind = 0; kind < 6; kind++) {
        for (i32 si = 0; si < countof(sizes); si++) {
            iz len = sizes[si];
            u8 *p = randbytes(len, kind);
            for (i32 level = 1; level <= 9; level++) {
                if (len>200000 && level%4!=1) {
                    continue;  // keep the suite fast
                }
                check_roundtrip(ctx, a, p, len, level);
            }
            free(p);
        }
    }

    // Exactly 65535*k incompressible bytes must still end with a final
    // block (stored chunks of exactly 65535 bytes).
    for (iz k = 1; k <= 3; k++) {
        iz len = 65535 * k;
        u8 *p = randbytes(len, 1);
        check_roundtrip(ctx, a, p, len, 6);
        free(p);
    }

    // Short reads must not change output
    {
        iz len = 300000;
        u8 *p = randbytes(len, 2);
        s8 x, y;
        TEST(do_gzip(ctx, a, p, len, 6, &x) == GZ_OK);
        ctx->readlimit = 777;
        TEST(do_gzip(ctx, a, p, len, 6, &y) == GZ_OK);
        TEST(s8equals(x, y));
        s8 out;
        TEST(do_gunzip(ctx, a, x.s, x.len, &out) == GZ_OK);
        TEST(equals(out, p, len));
        ctx->readlimit = 0;
        free(x.s);
        free(y.s);
        free(out.s);
        free(p);
    }
}

static void test_push_invariance(os *ctx, arena a)
{
    iz len = 3*WIN_CAP + 999;
    u8 *p = randbytes(len, 3);
    s8 ref = do_deflate(ctx, a, p, len, 6, 0, 0);
    static iz const pieces[] = {1, 7, 4096, 65536, WIN_CAP-1, WIN_CAP+1};
    for (i32 i = 0; i < countof(pieces); i++) {
        if (pieces[i] < 100) {
            // Byte-at-a-time pushes over a smaller prefix
            iz n = 200000;
            s8 x = do_deflate(ctx, a, p, n, 6, 0, 0);
            s8 y = do_deflate(ctx, a, p, n, 6, pieces[i], 0);
            TEST(s8equals(x, y));
            free(x.s);
            free(y.s);
            continue;
        }
        s8 out = do_deflate(ctx, a, p, len, 6, pieces[i], 0);
        TEST(s8equals(ref, out));
        free(out.s);
    }
    s8 out;
    TEST(zlib_inflate(ref.s, ref.len, -15, &out));
    TEST(equals(out, p, len));
    free(out.s);
    free(ref.s);
    free(p);
}

// Stream offsets beyond 4 GiB: start the deflater with a large base.
static void test_large_offset(os *ctx, arena a)
{
    iz len = 3*WIN_CAP;
    u8 *p = randbytes(len, 3);
    s8 ref = do_deflate(ctx, a, p, len, 6, 0, 0);

    set_stdin(ctx, 0, 0);
    writer  *w = newwriter(&a, 1, 1<<16);
    deflator *d = deflate_new(&a, 6, w);
    d->base = 0xffffffffu - 1000000;
    deflate_push(d, p, len);
    deflate_finish(d);
    writer_flush(w);
    s8 out = get_stdout(ctx);
    TEST(s8equals(ref, out));
    free(out.s);
    free(ref.s);
    free(p);
}

static s8 gzbytes(u8 const *p, iz len)
{
    s8 r = {malloc((uz)len), len};
    memcpy(r.s, p, (uz)len);
    return r;
}

static s8 cat(s8 a, u8 const *p, iz len)
{
    a.s = realloc(a.s, (uz)(a.len + len));
    memcpy(a.s+a.len, p, (uz)len);
    a.len += len;
    return a;
}

static void test_container(os *ctx, arena a)
{
    s8 out;
    u8 const payload[] = {'h', 'e', 'l', 'l', 'o'};
    s8 member;
    TEST(do_gzip(ctx, a, payload, 5, 6, &member) == GZ_OK);
    TEST(member.s[0]==0x1f && member.s[1]==0x8b && member.s[2]==8);

    // Multiple members concatenate
    s8 two = cat(gzbytes(member.s, member.len), member.s, member.len);
    TEST(do_gunzip(ctx, a, two.s, two.len, &out) == GZ_OK);
    TEST(equals(out, (u8 *)"hellohello", 10));
    free(out.s);

    // Empty member followed by a member
    s8 none;
    TEST(do_gzip(ctx, a, 0, 0, 6, &none) == GZ_OK);
    s8 mix = cat(gzbytes(none.s, none.len), member.s, member.len);
    TEST(do_gunzip(ctx, a, mix.s, mix.len, &out) == GZ_OK);
    TEST(equals(out, payload, 5));
    free(out.s);
    free(mix.s);
    free(none.s);

    // Trailing data
    static struct {
        u8  tail[4];
        i32 len;
        i32 want;
    } const tails[] = {
        {{0, 0, 0, 0},       4, GZ_TRAILING},
        {{'x', 'y'},         2, GZ_TRAILING},
        {{0x1f},             1, GZ_TRAILING},
        {{0x1f, 0x8b},       2, GZ_ETRUNC},
        {{0x1f, 0x8b, 8, 0}, 4, GZ_ETRUNC},
        {{0x1f, 0x8c, 8, 0}, 4, GZ_TRAILING},
    };
    for (i32 i = 0; i < countof(tails); i++) {
        s8 t = cat(gzbytes(member.s, member.len), tails[i].tail, tails[i].len);
        TEST(do_gunzip(ctx, a, t.s, t.len, &out) == tails[i].want);
        if (tails[i].want == GZ_TRAILING) {
            TEST(equals(out, payload, 5));
        }
        free(out.s);
        free(t.s);
    }

    // Not gzip
    TEST(do_gunzip(ctx, a, (u8 *)"hello", 5, &out) == GZ_ENOTGZ);
    free(out.s);
    TEST(do_gunzip(ctx, a, (u8 *)"\x1f", 1, &out) == GZ_ENOTGZ);
    free(out.s);
    TEST(do_gunzip(ctx, a, 0, 0, &out) == GZ_ETRUNC);
    free(out.s);

    // Every prefix of a valid member fails
    for (iz n = 1; n < member.len; n++) {
        TEST(do_gunzip(ctx, a, member.s, n, &out) != GZ_OK);
        free(out.s);
    }

    // Bad method, reserved flags
    s8 bad = gzbytes(member.s, member.len);
    bad.s[2] = 7;
    TEST(do_gunzip(ctx, a, bad.s, bad.len, &out) == GZ_EMETHOD);
    free(out.s);
    for (i32 bit = 5; bit < 8; bit++) {
        memcpy(bad.s, member.s, (uz)member.len);
        bad.s[3] = (u8)(1 << bit);
        TEST(do_gunzip(ctx, a, bad.s, bad.len, &out) == GZ_EFLAGS);
        free(out.s);
    }

    // Corrupt CRC and ISIZE
    memcpy(bad.s, member.s, (uz)member.len);
    bad.s[bad.len-8] ^= 1;
    TEST(do_gunzip(ctx, a, bad.s, bad.len, &out) == GZ_ECRC);
    free(out.s);
    memcpy(bad.s, member.s, (uz)member.len);
    bad.s[bad.len-4] ^= 1;
    TEST(do_gunzip(ctx, a, bad.s, bad.len, &out) == GZ_ELEN);
    free(out.s);
    free(bad.s);

    // Optional header fields, with and without a correct header CRC
    s8 body = {member.s+10, member.len-10};
    for (i32 flags = 0; flags < 32; flags++) {
        u8 hdr[64];
        iz n = 0;
        static u8 const fixed[10] = {0x1f, 0x8b, 8, 0, 1, 2, 3, 4, 0, 3};
        memcpy(hdr, fixed, 10);
        hdr[3] = (u8)flags;
        n = 10;
        if (flags & FEXTRA) {
            hdr[n++] = 4;
            hdr[n++] = 0;
            hdr[n++] = 'A';
            hdr[n++] = 'B';
            hdr[n++] = 0;
            hdr[n++] = 0;
        }
        if (flags & FNAME) {
            memcpy(hdr+n, "name", 5);
            n += 5;
        }
        if (flags & FCOMMENT) {
            memcpy(hdr+n, "comment", 8);
            n += 8;
        }
        if (flags & FHCRC) {
            u32 crc = crc32_update(0, hdr, n);
            hdr[n++] = (u8)crc;
            hdr[n++] = (u8)(crc >> 8);
        }
        s8 g = cat(gzbytes(hdr, n), body.s, body.len);
        TEST(do_gunzip(ctx, a, g.s, g.len, &out) == GZ_OK);
        TEST(equals(out, payload, 5));
        free(out.s);
        TEST(zlib_inflate(g.s, g.len, 31, &out));
        free(out.s);

        // Every proper prefix fails
        for (iz k = 0; k < g.len; k++) {
            TEST(do_gunzip(ctx, a, g.s, k, &out) != GZ_OK);
            free(out.s);
        }

        if (flags & FHCRC) {
            g.s[n-1] ^= 0x40;
            TEST(do_gunzip(ctx, a, g.s, g.len, &out) == GZ_EHCRC);
            free(out.s);
            TEST(!zlib_inflate(g.s, g.len, 31, &out));
            free(out.s);
        }
        free(g.s);
    }

    // ISIZE is modulo 2^32: covered by the slow CLI test.
    free(two.s);
    free(member.s);
}

static void test_io_errors(os *ctx, arena a)
{
    s8 gz, out;
    u8 *p = randbytes(100000, 2);
    TEST(do_gzip(ctx, a, p, 100000, 6, &gz) == GZ_OK);

    ctx->failwrite = 1;
    TEST(do_gzip(ctx, a, p, 100000, 6, &out) == GZ_EWRITE);
    free(out.s);
    TEST(do_gunzip(ctx, a, gz.s, gz.len, &out) == GZ_EWRITE);
    free(out.s);
    ctx->failwrite = 0;

    // Read failures on files via the CLI
    mfs_put(ctx, "in", p, 100000);
    ctx->failread = 1;
    config conf = {0};
    s8 args[] = {S("-c"), S("in")};
    conf.perm = a;
    conf.args = args;
    conf.nargs = countof(args);
    TEST(gzip_main(&conf) == EXIT_ERR);
    ctx->failread = 0;

    free(gz.s);
    free(p);
}

// Run the command line with a space-separated argument string.
static i32 run(os *ctx, arena a, char *cmdline)
{
    s8 args[32];
    i32 nargs = 0;
    for (char *p = cmdline; *p;) {
        for (; *p == ' '; p++) {}
        if (!*p) {
            break;
        }
        char *beg = p;
        for (; *p && *p!=' '; p++) {}
        args[nargs++] = (s8){(u8 *)beg, p - beg};
    }
    mfs_put(ctx, "<stdout>", 0, 0);
    mfs_put(ctx, "<stderr>", 0, 0);
    ctx->fds[0].off = 0;
    config conf = {0};
    conf.perm = a;
    conf.args = args;
    conf.nargs = nargs;
    return gzip_main(&conf);
}

static b32 has(os *ctx, char *name)
{
    return !!mfs_get(ctx, name).s;
}

static b32 stderr_has(os *ctx, char *needle)
{
    s8 e = mfs_get(ctx, "<stderr>");
    iz n = (iz)strlen(needle);
    for (iz i = 0; i+n <= e.len; i++) {
        if (!memcmp(e.s+i, needle, (uz)n)) {
            return 1;
        }
    }
    return 0;
}

static void test_cli(os *ctx, arena a)
{
    u8 *text = randbytes(50000, 2);
    s8 gz, out;
    TEST(do_gzip(ctx, a, text, 50000, 6, &gz) == GZ_OK);

    // Compress in place
    mfs_reset(ctx);
    mfs_put(ctx, "f", text, 50000);
    TEST(run(ctx, a, "f") == EXIT_OK);
    TEST(!has(ctx, "f"));
    TEST(has(ctx, "f.gz"));
    s8 fgz = mfs_get(ctx, "f.gz");
    TEST(do_gunzip(ctx, a, fgz.s, fgz.len, &out) == GZ_OK);
    TEST(equals(out, text, 50000));
    free(out.s);

    // Decompress in place
    TEST(run(ctx, a, "-d f.gz") == EXIT_OK);
    TEST(!has(ctx, "f.gz"));
    TEST(equals(mfs_get(ctx, "f"), text, 50000));

    // Keep, level, combined flags
    TEST(run(ctx, a, "-k9 f") == EXIT_OK);
    TEST(has(ctx, "f") && has(ctx, "f.gz"));

    // Refuse to overwrite, then force
    TEST(run(ctx, a, "f") == EXIT_WARN);
    TEST(stderr_has(ctx, "already exists"));
    TEST(has(ctx, "f"));
    mfs_put(ctx, "f.gz", (u8 *)"junk", 4);
    TEST(run(ctx, a, "-f f") == EXIT_OK);
    TEST(!has(ctx, "f"));
    fgz = mfs_get(ctx, "f.gz");
    TEST(do_gunzip(ctx, a, fgz.s, fgz.len, &out) == GZ_OK);
    TEST(equals(out, text, 50000));
    free(out.s);

    // Already has suffix; unknown suffix
    TEST(run(ctx, a, "f.gz") == EXIT_WARN);
    TEST(stderr_has(ctx, "already has .gz suffix"));
    mfs_put(ctx, "g", text, 100);
    TEST(run(ctx, a, "-d g") == EXIT_WARN);
    TEST(stderr_has(ctx, "unknown suffix"));
    TEST(has(ctx, "g"));

    // Upper case suffix and .tgz
    mfs_put(ctx, "U.GZ", gz.s, gz.len);
    TEST(run(ctx, a, "-d U.GZ") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "U"), text, 50000));
    mfs_put(ctx, "t.tgz", gz.s, gz.len);
    TEST(run(ctx, a, "-d t.tgz") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "t.tar"), text, 50000));
    TEST(!has(ctx, "t.tgz"));

    // A bare ".gz" has no stem
    mfs_put(ctx, ".gz", gz.s, gz.len);
    TEST(run(ctx, a, "-d .gz") == EXIT_WARN);

    // Corrupt input: error, output removed, input kept
    s8 bad = gzbytes(gz.s, gz.len);
    bad.s[bad.len/2] ^= 0x55;
    mfs_put(ctx, "bad.gz", bad.s, bad.len);
    TEST(run(ctx, a, "-d bad.gz") == EXIT_ERR);
    TEST(has(ctx, "bad.gz"));
    TEST(!has(ctx, "bad"));
    free(bad.s);

    // Trailing garbage: warning, success otherwise
    s8 trail = cat(gzbytes(gz.s, gz.len), (u8 *)"junk", 4);
    mfs_put(ctx, "tr.gz", trail.s, trail.len);
    TEST(run(ctx, a, "-d tr.gz") == EXIT_WARN);
    TEST(stderr_has(ctx, "trailing garbage"));
    TEST(equals(mfs_get(ctx, "tr"), text, 50000));
    TEST(!has(ctx, "tr.gz"));
    mfs_put(ctx, "tr.gz", trail.s, trail.len);
    TEST(run(ctx, a, "-dqf tr.gz") == EXIT_WARN);
    TEST(!mfs_get(ctx, "<stderr>").len);
    free(trail.s);

    // Test mode
    mfs_put(ctx, "ok.gz", gz.s, gz.len);
    mfs_put(ctx, "bad.gz", (u8 *)"\x1f\x8b\x08\x00", 4);
    TEST(run(ctx, a, "-t ok.gz") == EXIT_OK);
    TEST(run(ctx, a, "-t ok.gz bad.gz ok.gz") == EXIT_ERR);
    TEST(has(ctx, "ok.gz") && has(ctx, "bad.gz"));
    TEST(!mfs_get(ctx, "<stdout>").len);

    // Standard output mode keeps inputs
    TEST(run(ctx, a, "-dc ok.gz ok.gz") == EXIT_OK);
    s8 o = mfs_get(ctx, "<stdout>");
    TEST(o.len == 100000);
    TEST(!memcmp(o.s, text, 50000) && !memcmp(o.s+50000, text, 50000));
    TEST(has(ctx, "ok.gz"));

    // Standard input
    set_stdin(ctx, text, 50000);
    TEST(run(ctx, a, "") == EXIT_OK);
    o = mfs_get(ctx, "<stdout>");
    TEST(do_gunzip(ctx, a, o.s, o.len, &out) == GZ_OK);
    TEST(equals(out, text, 50000));
    free(out.s);
    set_stdin(ctx, gz.s, gz.len);
    TEST(run(ctx, a, "-d -") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "<stdout>"), text, 50000));
    set_stdin(ctx, (u8 *)"nope", 4);
    TEST(run(ctx, a, "-d") == EXIT_ERR);
    TEST(stderr_has(ctx, "stdin: not in gzip format"));

    // End of options
    mfs_put(ctx, "-k", text, 10);
    TEST(run(ctx, a, "-- -k") == EXIT_OK);
    TEST(has(ctx, "-k.gz") && !has(ctx, "-k"));

    // Missing file, directory, unknown option, help, version
    TEST(run(ctx, a, "missing") == EXIT_ERR);
    TEST(stderr_has(ctx, "missing: cannot open"));
    mfs_create(ctx, S("dir"))->isdir = 1;
    TEST(run(ctx, a, "dir") == EXIT_WARN);
    TEST(stderr_has(ctx, "is a directory"));
    TEST(run(ctx, a, "-x f") == EXIT_ERR);
    TEST(run(ctx, a, "--bogus") == EXIT_ERR);
    TEST(run(ctx, a, "-h") == EXIT_OK);
    TEST(mfs_get(ctx, "<stdout>").len > 0);
    TEST(run(ctx, a, "--version") == EXIT_OK);

    // Error wins over warning in the exit status
    mfs_put(ctx, "w", text, 10);
    mfs_put(ctx, "w.gz", text, 10);
    TEST(run(ctx, a, "w missing") == EXIT_ERR);
    TEST(run(ctx, a, "w") == EXIT_WARN);

    // Long options
    mfs_put(ctx, "l", text, 1000);
    TEST(run(ctx, a, "--best --keep --force l") == EXIT_OK);
    TEST(has(ctx, "l") && has(ctx, "l.gz"));
    TEST(run(ctx, a, "--decompress --stdout l.gz") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "<stdout>"), text, 1000));

    free(gz.s);
    free(text);
}

static void test_cli_safety(os *ctx, arena a)
{
    u8 *text = randbytes(20000, 2);
    s8 out;

    // Metadata follows the data in both directions
    mfs_reset(ctx);
    mfile *f = mfs_create(ctx, S("m"));
    mfs_append(f, text, 20000);
    f->mode = 0640;
    f->mtime = 1234567890;
    TEST(run(ctx, a, "m") == EXIT_OK);
    f = mfs_find(ctx, S("m.gz"));
    TEST(f && f->mode==0640 && f->mtime==1234567890);
    f->mode = 0604;
    f->mtime = 42;
    TEST(run(ctx, a, "-d m.gz") == EXIT_OK);
    f = mfs_find(ctx, S("m"));
    TEST(f && f->mode==0604 && f->mtime==42);
    TEST(equals(mfs_get(ctx, "m"), text, 20000));

    // Symbolic links are skipped in place unless forced
    f = mfs_create(ctx, S("lnk"));
    mfs_append(f, text, 100);
    f->issymlink = 1;
    TEST(run(ctx, a, "lnk") == EXIT_WARN);
    TEST(stderr_has(ctx, "is a symbolic link"));
    TEST(has(ctx, "lnk") && !has(ctx, "lnk.gz"));
    TEST(run(ctx, a, "-c lnk") == EXIT_OK);
    TEST(mfs_get(ctx, "<stdout>").len > 0);
    TEST(run(ctx, a, "-f lnk") == EXIT_OK);
    TEST(!has(ctx, "lnk") && has(ctx, "lnk.gz"));

    // Hard-linked files likewise
    f = mfs_create(ctx, S("hard"));
    mfs_append(f, text, 100);
    f->nlinks = 1;
    TEST(run(ctx, a, "hard") == EXIT_WARN);
    TEST(stderr_has(ctx, "has other links"));
    TEST(has(ctx, "hard") && !has(ctx, "hard.gz"));
    TEST(run(ctx, a, "-f hard") == EXIT_OK);
    TEST(has(ctx, "hard.gz"));

    // Special files are never replaced, but may be read
    f = mfs_create(ctx, S("fifo"));
    mfs_append(f, text, 100);
    f->isspecial = 1;
    TEST(run(ctx, a, "fifo") == EXIT_WARN);
    TEST(stderr_has(ctx, "not a directory or a regular file"));
    TEST(run(ctx, a, "-f fifo") == EXIT_WARN);
    TEST(has(ctx, "fifo") && !has(ctx, "fifo.gz"));
    TEST(run(ctx, a, "-c fifo") == EXIT_OK);
    s8 o = mfs_get(ctx, "<stdout>");
    TEST(do_gunzip(ctx, a, o.s, o.len, &out) == GZ_OK);
    TEST(equals(out, text, 100));
    free(out.s);

    // Failure to close the output keeps the input and removes the output
    mfs_put(ctx, "c", text, 20000);
    ctx->failclose = 1;
    TEST(run(ctx, a, "c") == EXIT_ERR);
    TEST(stderr_has(ctx, "write error"));
    ctx->failclose = 0;
    TEST(equals(mfs_get(ctx, "c"), text, 20000));
    TEST(!has(ctx, "c.gz"));

    // Write failure mid-stream: nothing partial left behind
    ctx->failwrite = 1;
    TEST(run(ctx, a, "c") == EXIT_ERR);
    ctx->failwrite = 0;
    TEST(has(ctx, "c") && !has(ctx, "c.gz"));

    // Terminals: no compressed data written to or read from one
    mfs_put(ctx, "t", text, 1000);
    ctx->tty[1] = 1;
    set_stdin(ctx, text, 1000);
    TEST(run(ctx, a, "") == EXIT_ERR);
    TEST(stderr_has(ctx, "not written to a terminal"));
    TEST(!mfs_get(ctx, "<stdout>").len);
    TEST(run(ctx, a, "-c t") == EXIT_ERR);
    TEST(!mfs_get(ctx, "<stdout>").len);
    TEST(run(ctx, a, "-cf t") == EXIT_OK);
    TEST(mfs_get(ctx, "<stdout>").len > 0);
    TEST(run(ctx, a, "-k t") == EXIT_OK);  // in place: terminal irrelevant
    s8 tgz = dup8(mfs_get(ctx, "t.gz"));
    TEST(run(ctx, a, "-dc t.gz") == EXIT_OK);  // plain output is fine
    TEST(equals(mfs_get(ctx, "<stdout>"), text, 1000));
    ctx->tty[1] = 0;

    ctx->tty[0] = 1;
    set_stdin(ctx, tgz.s, tgz.len);
    TEST(run(ctx, a, "-d") == EXIT_ERR);
    TEST(stderr_has(ctx, "not read from a terminal"));
    TEST(run(ctx, a, "-t") == EXIT_ERR);
    TEST(run(ctx, a, "-t t.gz") == EXIT_OK);  // files are fine
    set_stdin(ctx, tgz.s, tgz.len);
    TEST(run(ctx, a, "-df") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "<stdout>"), text, 1000));
    set_stdin(ctx, text, 1000);
    TEST(run(ctx, a, "") == EXIT_OK);  // compressing from a terminal is fine
    ctx->tty[0] = 0;

    free(tgz.s);
    free(text);
}

static void test_oom(os *ctx, arena a)
{
    // Every truncation of the arena must fail cleanly via os_fail
    u8 *text = randbytes(1000, 2);
    s8 gz;
    TEST(do_gzip(ctx, a, text, 1000, 6, &gz) == GZ_OK);
    for (iz cap = 0; cap < 4<<20; cap = cap*2 + 1000) {
        arena t = a;
        t.end = t.beg + cap;
        jmp_buf save;
        memcpy(save, ctx->fail, sizeof(save));
        if (!setjmp(ctx->fail)) {
            s8 out;
            do_gunzip(ctx, t, gz.s, gz.len, &out);
            free(out.s);
        }
        if (!setjmp(ctx->fail)) {
            s8 out;
            do_gzip(ctx, t, text, 1000, 6, &out);
            free(out.s);
        }
        memcpy(ctx->fail, save, sizeof(save));
    }
    free(gz.s);
    free(text);
}

int main(void)
{
    static os ctx;
    iz cap = (iz)1 << 28;
    arena a = {0};
    a.beg = malloc((uz)cap);
    a.end = a.beg + cap;
    a.ctx = &ctx;
    mfs_reset(&ctx);

    if (setjmp(ctx.fail)) {
        fprintf(stderr, "unexpected os_fail\n");
        __builtin_trap();
    }

    test_tables();
    test_huffman();
    test_inflate_vectors(&ctx, a);
    test_inflate_repeats(&ctx, a);
    test_container(&ctx, a);
    test_io_errors(&ctx, a);
    test_cli(&ctx, a);
    test_cli_safety(&ctx, a);
    test_oom(&ctx, a);
    test_push_invariance(&ctx, a);
    test_large_offset(&ctx, a);
    test_inflate_zlib(&ctx, a);
    test_roundtrip(&ctx, a);

    puts("all tests pass");
    return 0;
}
