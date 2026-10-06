// Test suite for tugz
// On success prints "all tests pass" and exits with status zero. A
// failure traps, so run under a debugger to examine it.
// $ cc -g3 -fsanitize=address,undefined -o tests test/tests.c -lz -ldeflate
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/adler32.c"
#include "../src/inflate.c"
#include "../src/deflate.c"
#include "../src/gzip.c"
#include "../src/io.c"
#include "../src/gzipio.c"
#include "../src/cli.c"

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
    u32 mode;       // metadata copied by os_getmeta and os_setmeta
    i64 mtime;
    i64 atime;      // set to ATIME_NOW by reading
} mfile;

enum { ATIME_NOW = 1000000 };

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
    b32 failread;    // reads fail at an offset of failreadat or beyond
    iz  failreadat;
    b32 failwrite;   // writes to descriptors other than stderr fail
    b32 brokenpipe;  // ...as to a pipe without a reader
    iz  reads;       // calls to os_read
    iz  writes;      // calls to os_write, other than for stderr
    b32 failcreate;  // creating files fails
    b32 failstat;    // getting metadata fails
    b32 failmeta;    // setting metadata fails
    b32 failclose;   // closing created files fails
    b32 failremove;  // removing files fails
    b32 tty[3];      // standard descriptors attached to a terminal
    b32 missing;     // the last os_open found no such file
    char *error;     // why the last failing call failed, for os_error
    b32 pipeclosed;  // the last failing write found no reader
    b32 noreason;    // os_error knows no reasons
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
    f->mtime = f->atime = 0;
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
    ctx->readlimit = ctx->failreadat = 0;
    ctx->failread = ctx->failwrite = ctx->failclose = ctx->failremove = 0;
    ctx->brokenpipe = ctx->failcreate = ctx->noreason = 0;
    ctx->failstat = ctx->failmeta = 0;
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
    ctx->missing = !f;
    if ((mode & (OS_CREATE|OS_FORCE)) && ctx->failcreate) {
        ctx->error = "Permission denied";
        return OS_ERR;
    } else if (mode & OS_CREATE) {
        if (f) {
            ctx->error = "File exists";
            return OS_EEXIST;
        }
        f = mfs_create(ctx, path);
        created = 1;
    } else if (mode & OS_FORCE) {
        f = mfs_create(ctx, path);
        created = 1;
    } else if (!f) {
        ctx->error = "No such file or directory";
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
        if (ctx->failclose) {
            ctx->error = "Disk quota exceeded";
            return 0;
        }
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

static b32 os_missing(os *ctx)
{
    return ctx->missing;
}

static s8 os_error(os *ctx)
{
    return ctx->error && !ctx->noreason ? cstrs8(ctx->error) : S("");
}

static b32 os_pipeclosed(os *ctx)
{
    return ctx->pipeclosed;
}

struct osmeta {
    u32 mode;
    i64 mtime;
    i64 atime;
};

static osmeta *os_getmeta(os *ctx, i32 fd, arena *a)
{
    TEST(fd>2 && fd<MAX_FDS && ctx->fds[fd].open);
    if (ctx->failstat) {
        ctx->error = "Input/output error";
        return 0;
    }
    mfile  *f = ctx->files + ctx->fds[fd].file;
    osmeta *m = new(a, 1, osmeta);
    m->mode  = f->mode;
    m->mtime = f->mtime;
    m->atime = f->atime;
    return m;
}

static b32 os_setmeta(os *ctx, i32 fd, osmeta *m)
{
    TEST(fd>2 && fd<MAX_FDS && ctx->fds[fd].open && ctx->fds[fd].created);
    if (ctx->failmeta) {
        ctx->error = "Operation not permitted";
        return 0;
    }
    mfile *f = ctx->files + ctx->fds[fd].file;
    f->mode  = m->mode;
    f->mtime = m->mtime;
    f->atime = m->atime;
    return 1;
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    TEST(fd>=0 && fd<MAX_FDS && ctx->fds[fd].open);
    TEST(cap > 0);
    ctx->reads++;
    iz off = ctx->fds[fd].off;
    if (ctx->failread && off>=ctx->failreadat) {
        ctx->error = "Input/output error";
        return -1;
    }
    mfile *f = ctx->files + ctx->fds[fd].file;
    iz n = MIN(cap, f->len - off);
    if (ctx->readlimit) {
        n = MIN(n, ctx->readlimit);
    }
    if (ctx->failread) {
        n = MIN(n, ctx->failreadat - off);
    }
    if (n) {
        memcpy(buf, f->data + ctx->fds[fd].off, (uz)n);
    }
    ctx->fds[fd].off += n;
    f->atime = ATIME_NOW;
    return n;
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    TEST(fd>=0 && fd<MAX_FDS && ctx->fds[fd].open);
    TEST(fd != 0);
    ctx->writes += fd != 2;
    if ((ctx->failwrite || ctx->brokenpipe) && fd!=2) {
        ctx->pipeclosed = ctx->brokenpipe;
        ctx->error = ctx->brokenpipe ? "Broken pipe"
                                     : "No space left on device";
        return 0;
    }
    mfs_append(ctx->files + ctx->fds[fd].file, buf, len);
    return 1;
}

static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)scratch;
    mfile *f = mfs_find(ctx, path);
    if (ctx->failremove) {
        ctx->error = "Operation not permitted";
        return 0;
    } else if (!f) {
        ctx->error = "No such file or directory";
        return 0;
    }
    f->live = 0;
    return 1;
}

static void os_exit(os *ctx, i32 status)
{
    (void)status;
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
    encoder *e = gzip_encoder(&a, level);
    i32 status = gzip_compress(e, 0, 1, level, a);
    *out = get_stdout(ctx);
    return status;
}

static i32 do_gunzip(os *ctx, arena a, u8 const *p, iz len, s8 *out)
{
    set_stdin(ctx, p, len);
    i32 status = stream_decompress(stream_decoder(&a, FMT_GZIP), 0, 1, 0, a);
    *out = get_stdout(ctx);
    return status;
}

static i32 do_inflate(os *ctx, arena a, u8 const *p, iz len, s8 *out)
{
    set_stdin(ctx, p, len);
    i32 status = stream_decompress(stream_decoder(&a, FMT_RAW), 0, 1, 0, a);
    *out = get_stdout(ctx);
    return status;
}

// Compress in memory, feeding input and taking output in pieces of the
// given sizes (0 for unlimited), with the deflater at a stream base.
static s8 zcompress(arena a, i32 format, u8 const *p, iz len, i32 level,
                    iz inpiece, iz outpiece, u64 base)
{
    encoder *e = encoder_new(&a, format, level);
    e->def->base = base;
    iz cap = len + len/4 + (1<<16);
    s8 r = {malloc((uz)cap), 0};
    zbuf b = {p, 0, 0, 0};
    for (;;) {
        iz inleft = p + len - b.in;
        b.inlen = inpiece ? MIN(inpiece, inleft) : inleft;
        iz inlen = b.inlen;
        b.out = r.s + r.len;
        b.outlen = outpiece ? MIN(outpiece, cap-r.len) : cap-r.len;
        u8 *out = b.out;
        i32 flush = inlen==inleft ? DEF_FINISH : DEF_NONE;
        i32 status = encoder_run(e, &b, flush);
        r.len += b.out - out;
        TEST(b.inlen==0 || status==GZ_NEEDOUT);
        TEST(status==GZ_OK || status==GZ_NEEDIN || status==GZ_NEEDOUT);
        TEST(r.len < cap);
        if (status == GZ_OK) {
            TEST(flush == DEF_FINISH);
            break;
        }
    }
    return r;
}

// Raw deflate in pieces of the given size (0 for one piece).
static s8 do_deflate(os *ctx, arena a, u8 const *p, iz len, i32 level,
                     iz piece, deflator **dp)
{
    (void)ctx;
    (void)dp;
    return zcompress(a, FMT_RAW, p, len, level, piece, 0, 0);
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

    // The fixed codes' constant decoding tables are htable_build's
    u16 lens[288];
    for (i32 i = 0; i < 288; i++) {
        lens[i] = (u16)(i<144 ? 8 : i<256 ? 9 : i<280 ? 7 : 8);
    }
    u32 fixed[512];
    htable h;
    TEST(htable_build(&h, fixed, countof(fixed), lens, 288, HUFF_LITLEN,
                      LIT_ROOT));
    TEST(h.mask == countof(inf_fixlit)-1);
    TEST(!memcmp(fixed, inf_fixlit, sizeof(inf_fixlit)));
    for (i32 i = 0; i < 32; i++) {
        lens[i] = 5;
    }
    TEST(htable_build(&h, fixed, 32, lens, 32, HUFF_DIST, DIST_ROOT));
    TEST(h.mask == countof(inf_fixdist)-1);
    TEST(!memcmp(fixed, inf_fixdist, sizeof(inf_fixdist)));
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

// Bit reader over a raw DEFLATE stream, for inspecting block headers.
typedef struct {
    u8 const *p;
    iz        len;
    iz        bit;
} bitreader;

static u32 getbits(bitreader *r, i32 n)
{
    u32 v = 0;
    for (i32 i = 0; i < n; i++, r->bit++) {
        TEST(r->bit>>3 < r->len);
        v |= (u32)(r->p[r->bit>>3] >> (r->bit&7) & 1) << i;
    }
    return v;
}

// Kraft sum of code lengths in units of 2^-15: 1<<15 when complete.
static u32 kraft(u8 const *lens, i32 n)
{
    u32 sum = 0;
    for (i32 i = 0; i < n; i++) {
        sum += lens[i] ? 1u << (15 - lens[i]) : 0;
    }
    return sum;
}

// The stream must be one final dynamic block whose codes are all
// complete. Some decoders, notably Windows' zip folder, reject the
// incomplete codes that DEFLATE permits (libdeflate issue #323), such as
// a lone distance code when a block has at most one distinct distance.
// Returns whether distance symbol dsym has a code, or true if dsym < 0.
static b32 check_complete_codes(s8 z, i32 dsym)
{
    static u8 const order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    bitreader r = {z.s, z.len, 0};
    TEST(getbits(&r, 1) == 1);  // BFINAL: the input was not split
    TEST(getbits(&r, 2) == 2);
    i32 hlit  = (i32)getbits(&r, 5) + 257;
    i32 hdist = (i32)getbits(&r, 5) + 1;
    i32 hclen = (i32)getbits(&r, 4) + 4;
    u8 cl[19] = {0};
    for (i32 i = 0; i < hclen; i++) {
        cl[order[i]] = (u8)getbits(&r, 3);
    }
    TEST(kraft(cl, 19) == 1u<<15);

    // Canonical decoding of the code length code, one bit at a time
    u16 code[19];
    u16 next[8] = {0};
    u16 count[8] = {0};
    for (i32 i = 0; i < 19; i++) {
        count[cl[i]]++;
    }
    count[0] = 0;
    for (i32 b = 1, c = 0; b < 8; b++) {
        c = (c + count[b-1]) << 1;
        next[b] = (u16)c;
    }
    for (i32 i = 0; i < 19; i++) {
        code[i] = cl[i] ? next[cl[i]]++ : 0;
    }

    u8 lens[286+30] = {0};
    for (i32 n = 0; n < hlit+hdist;) {
        i32 sym = -1;
        u32 c = 0;
        for (i32 len = 1; sym<0 && len<=7; len++) {
            c = c<<1 | getbits(&r, 1);
            for (i32 i = 0; i < 19; i++) {
                if (cl[i]==len && code[i]==c) {
                    sym = i;
                }
            }
        }
        TEST(sym >= 0);
        if (sym < 16) {
            lens[n++] = (u8)sym;
        } else {
            i32 rep = sym==16 ? 3+(i32)getbits(&r, 2) :
                      sym==17 ? 3+(i32)getbits(&r, 3) : 11+(i32)getbits(&r, 7);
            u8 v = sym==16 ? lens[n-1] : 0;
            for (; rep--; n++) {
                lens[n] = v;
            }
        }
    }
    TEST(kraft(lens, hlit) == 1u<<15);
    TEST(kraft(lens+hlit, hdist) == 1u<<15);
    return dsym<0 || (dsym<hdist && lens[hlit+dsym]);
}

// de Bruijn sequence over 26 letters of order 3: no three-byte string
// repeats, so a compressor finds no matches, though literals compress.
static iz debruijn(u8 *out, iz len, u8 *a, i32 t, i32 p)
{
    if (t > 3) {
        for (i32 j = 1; 3%p==0 && j<=p; j++) {
            out[len++] = (u8)('a' + a[j]);
        }
        return len;
    }
    a[t] = a[t-p];
    len = debruijn(out, len, a, t+1, p);
    for (i32 j = a[t-p]+1; j < 26; j++) {
        a[t] = (u8)j;
        len = debruijn(out, len, a, t+1, t);
    }
    return len;
}

static void test_complete_codes(os *ctx, arena a)
{
    u8  *p = new(&a, 17576+64, u8);
    u8   state[4] = {0};
    iz   len = debruijn(p, 0, state, 1, 1);
    TEST(len == 17576);

    for (i32 level = 1; level <= 9; level++) {
        // Literals only: no distance codes used at all
        TEST(check_complete_codes(do_deflate(ctx, a, p, len, level, 0, 0),
                                  -1));

        // One match, at distance 17576 (symbol 28): a single distance
        // code used, which must appear in the checked block. Where the
        // code's other symbol goes is up to the encoder.
        bytecopy(p+len, p, 40);
        s8 z = do_deflate(ctx, a, p, len+40, level, 0, 0);
        TEST(check_complete_codes(z, 28));
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
        {0, 0, 0, 0},  // empty: like zlib, fails at missing end-of-block
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
        for (i32 j = 0; j < 10; j++) {
            bput(&b, 0, 32);  // enough 1-bit lengths for the empty case
        }
        TEST(inflate_bits(ctx, a, &b, &out) == GZ_EDATA);
        free(out.s);
    }

    // Fuzz regression: an empty code length code is truncation, not an
    // error, while input runs out before the missing end-of-block
    b = (bits){0};
    bput(&b, 1, 1);
    bput(&b, 2, 2);
    bput(&b, 0, 14);
    bput(&b, 0, 12);
    bput(&b, 0, 32);
    TEST(inflate_bits(ctx, a, &b, &out) == GZ_ETRUNC);
    free(out.s);
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

// zlib's verdict on the first len bytes of a raw DEFLATE stream decoded
// in one call, as the status our decoder should return: GZ_OK at the end
// of the stream, GZ_NEEDIN if truncated, or GZ_EDATA.
static i32 zlib_prefix(u8 const *p, iz len, u8 *out, iz cap, iz *outlen,
                       iz *used)
{
    z_stream z = {0};
    TEST(inflateInit2(&z, -15) == Z_OK);
    z.next_in = (u8 *)p;
    z.avail_in = (u32)len;
    z.next_out = out;
    z.avail_out = (u32)cap;
    i32 r = inflate(&z, Z_FINISH);
    TEST(z.avail_out);
    *outlen = cap - z.avail_out;
    *used = len - z.avail_in;
    inflateEnd(&z);
    TEST(r==Z_STREAM_END || r==Z_BUF_ERROR || r==Z_DATA_ERROR);
    return r==Z_STREAM_END ? GZ_OK : r==Z_BUF_ERROR ? GZ_NEEDIN : GZ_EDATA;
}

// The streaming decoder against zlib at every input length of a raw
// DEFLATE stream followed by junk: each prefix in one piece, the whole
// split into two pieces at every byte, and one byte at a time. They must
// agree on success, on truncation versus error, on output, and on
// exactly where the stream ends.
static void check_splits(arena a, u8 const *p, iz len)
{
    enum { CAP = 1<<16, JUNK = 3 };
    iz   total = len + JUNK;
    u8  *in    = malloc((uz)total);
    u8  *zout  = malloc(CAP);  // zlib's output from the whole input
    u8  *tmp   = malloc(CAP);
    u8  *out   = malloc(CAP);
    i32 *want  = malloc(sizeof(*want) * (uz)(total+1));
    iz  *wlen  = malloc(sizeof(*wlen) * (uz)(total+1));
    memcpy(in, p, (uz)len);
    memset(in+len, 0x5a, JUNK);

    iz end = -1;  // where the stream ends, if it does
    for (iz n = total; n >= 0; n--) {
        iz used;
        want[n] = zlib_prefix(in, n, n==total ? zout : tmp, CAP, wlen+n,
                              &used);
        end = n==total && want[n]==GZ_OK ? used : end;
        TEST(n==total || !memcmp(tmp, zout, (uz)wlen[n]));
    }

    for (iz n = 0; n <= total; n++) {
        arena t = a;
        decoder *z = decoder_new(&t, FMT_RAW);
        zbuf b = {in, n, out, CAP};
        i32 r = decoder_run(z, &b);
        TEST(r == want[n]);
        TEST(CAP-b.outlen==wlen[n] && !memcmp(out, zout, (uz)wlen[n]));
        TEST(r!=GZ_OK || b.in-in==end);
        TEST(r!=GZ_NEEDIN || !b.inlen);
    }

    for (iz cut = 0; cut <= total; cut++) {
        arena t = a;
        decoder *z = decoder_new(&t, FMT_RAW);
        zbuf b = {in, cut, out, CAP};
        i32 r = decoder_run(z, &b);
        TEST(r == want[cut]);
        if (r == GZ_NEEDIN) {
            TEST(!b.inlen);
            b.inlen = total - cut;
            r = decoder_run(z, &b);
        }
        TEST(r == want[total]);
        TEST(CAP-b.outlen==wlen[total] && !memcmp(out, zout, (uz)wlen[total]));
        TEST(r!=GZ_OK || b.in-in==end);
    }

    arena t = a;
    decoder *z = decoder_new(&t, FMT_RAW);
    zbuf b = {in, 0, out, CAP};
    i32 r = GZ_NEEDIN;
    for (iz n = 1; r==GZ_NEEDIN && n<=total; n++) {
        b.inlen = 1;
        r = decoder_run(z, &b);
        TEST(r == want[n]);
        TEST(CAP-b.outlen==wlen[n] && !memcmp(out, zout, (uz)wlen[n]));
        TEST(r!=GZ_OK || (n==end && !b.inlen));
    }
    TEST(r == want[total]);

    free(wlen);
    free(want);
    free(out);
    free(tmp);
    free(zout);
    free(in);
}

// A lone 1-bit or empty distance code is decoded with a 1-bit table
// whose other entries are invalid. Input running out inside a length's
// extra bits must still be truncation, not those entries' error, which
// once rejected valid streams depending on how input was split. Go's
// compress/flate writes lone distance codes at levels 5 to 9.
static void test_inflate_splits(arena a)
{
    static i32 const lensyms[] = {269, 273, 277, 281};  // 2 to 5 extra bits
    static u8 const lits[] = "abaabbab";
    bits b;

    // 'a' and 'b': 3 bits, end of block: 2 bits, lengths: 3 bits
    dyncode c = {0};
    c.hlit = 282;
    c.hdist = 6;
    c.lens['a'] = 3;
    c.lens['b'] = 3;
    c.lens[256] = 2;
    for (i32 i = 0; i < countof(lensyms); i++) {
        c.lens[lensyms[i]] = 3;
    }

    // Lone 1-bit distance code, for distance 1 or for 7..8 (1 extra bit):
    // every length symbol with 2 to 5 extra bits, with every extra value
    for (i32 dsym = 0; dsym <= 5; dsym += 5) {
        c.lens[c.hlit+0] = c.lens[c.hlit+5] = 0;
        c.lens[c.hlit+dsym] = 1;
        b = (bits){0};
        dyn_begin(&b, &c);
        for (i32 i = 0; i < countof(lits)-1; i++) {
            dyn_lit(&b, &c, lits[i]);
        }
        for (i32 i = 0; i < countof(lensyms); i++) {
            i32 extra = inf_len_extra[lensyms[i]-257];
            for (u32 v = 0; v < 1u<<extra; v++) {
                dyn_lit(&b, &c, lensyms[i]);
                bput(&b, v, extra);
                dyn_dist(&b, &c, dsym);
                bput(&b, v, inf_dist_extra[dsym]);
                dyn_lit(&b, &c, lits[v%8]);
            }
        }
        dyn_lit(&b, &c, 256);
        check_splits(a, b.buf, b.len);

        // The lone code's unused codeword after a length: an error only
        // once every extra bit and that codeword's bit are present
        for (i32 i = 0; i < countof(lensyms); i++) {
            i32 extra = inf_len_extra[lensyms[i]-257];
            for (u32 v = 0; v < 1u<<extra; v += 3) {
                b = (bits){0};
                dyn_begin(&b, &c);
                for (i32 j = 0; j < countof(lits)-1; j++) {
                    dyn_lit(&b, &c, lits[j]);
                }
                dyn_lit(&b, &c, lensyms[i]);
                bput(&b, v, extra);
                bput(&b, 1, 1);
                bput(&b, 0, 16);
                check_splits(a, b.buf, b.len);
            }
        }
    }

    // Empty distance code: valid while no length is decoded
    c.lens[c.hlit+0] = c.lens[c.hlit+5] = 0;
    b = (bits){0};
    dyn_begin(&b, &c);
    for (i32 i = 0; i < countof(lits)-1; i++) {
        dyn_lit(&b, &c, lits[i]);
    }
    dyn_lit(&b, &c, 256);
    check_splits(a, b.buf, b.len);

    // ... and after a length, an error only once its extra bits and one
    // more bit are present, where zlib stops needing input
    for (i32 i = 0; i < countof(lensyms); i++) {
        i32 extra = inf_len_extra[lensyms[i]-257];
        for (u32 v = 0; v < 1u<<extra; v++) {
            b = (bits){0};
            dyn_begin(&b, &c);
            for (i32 j = 0; j < countof(lits)-1; j++) {
                dyn_lit(&b, &c, lits[j]);
            }
            dyn_lit(&b, &c, lensyms[i]);
            bput(&b, v, extra);
            bput(&b, v, 16);
            check_splits(a, b.buf, b.len);
        }
    }

    // For contrast, fixed codes, whose invalid distance codes are 5 bits:
    // every length symbol with extra bits, with its least and greatest
    // extra values, and distances with extra bits
    b = (bits){0};
    bput(&b, 1, 1);
    bput(&b, 1, 2);
    static u8 const text[] = "fixed Huffman codes, for contrast";
    for (i32 i = 0; i < countof(text)-1; i++) {
        bfixed(&b, text[i]);
    }
    for (i32 sym = 265; sym <= 284; sym++) {
        i32 extra = inf_len_extra[sym-257];
        for (u32 v = 0; v < 2; v++) {
            bfixed(&b, sym);
            bput(&b, v ? (1u<<extra)-1 : 0, extra);
            u32 dsym = (u32)(sym+(i32)v) % 10;  // at most 32 back
            bcode(&b, dsym, 5);
            bput(&b, v ? ~0u : 0, inf_dist_extra[dsym]);
        }
    }
    bfixed(&b, 256);
    check_splits(a, b.buf, b.len);

    // ... and their invalid distance code 30 after a length
    for (i32 sym = 265; sym <= 284; sym += 4) {
        b = (bits){0};
        bput(&b, 1, 1);
        bput(&b, 1, 2);
        bfixed(&b, 'a');
        bfixed(&b, sym);
        bput(&b, ~0u, inf_len_extra[sym-257]);
        bcode(&b, 30, 5);
        bput(&b, 0, 16);
        check_splits(a, b.buf, b.len);
    }
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

    s8 out = zcompress(a, FMT_RAW, p, len, 6, 0, 0, 0xffffffffu - 1000000);
    TEST(s8equals(ref, out));
    free(out.s);
    free(ref.s);
    free(p);
}

// Inputs from a fixed generator, apart from the other tests' random
// state: text of words and numbers, or 16 letters at random, favoring
// 3-byte matches.
static u8 *golden_input(iz len, b32 letters)
{
    static char const words[][9] = {
        "the ", "of ", "deflate ", "window ", "and ", "a ", "huffman\n",
        "zlib ",
    };
    u8 *p = malloc((uz)len);
    u64 s = 1 + letters;
    for (iz i = 0; i < len;) {
        s = s*0x3243f6a8885a308d + 1;
        if (letters) {
            p[i++] = (u8)('a' + (s >> 60));
        } else if (s>>58 & 3) {
            for (char const *w = words[s>>61]; *w && i<len; w++) {
                p[i++] = (u8)*w;
            }
        } else {
            for (u32 n = (u32)(s >> 32) % 1000; i < len; n /= 10) {
                p[i++] = (u8)('0' + n%10);
                if (n < 10) {
                    break;
                }
            }
            if (i < len) {
                p[i++] = ' ';
            }
        }
    }
    return p;
}

// Compressed output is pinned, so that no change to it passes unnoticed,
// even one that still decodes: CRC-32s of the gzip format at each level,
// recorded from commit eb73afb, of text long enough to slide the window
// and of data favoring 3-byte matches.
static void test_golden(arena a)
{
    static iz const lens[] = {1100000, 200000};
    static u32 const crcs[2][9] = {
        {0xcf800ee7, 0x9aa4a7b6, 0xf61fdcd7, 0x12b51908,
         0x62d88a64, 0x401c9543, 0x12d61d4f, 0x2c3f4802, 0x7765c8a5},
        {0x1325006c, 0x61c7c7d0, 0x36266ef9, 0xe63b2e04,
         0x9456a39d, 0x9456a39d, 0x112f0260, 0x112f0260, 0x112f0260},
    };
    for (i32 k = 0; k < 2; k++) {
        u8 *p = golden_input(lens[k], k);
        for (i32 level = 1; level <= 9; level++) {
            s8 z = zcompress(a, FMT_GZIP, p, lens[k], level, 0, 0, 0);
            TEST(crc32_update(0, z.s, z.len) == crcs[k][level-1]);
            free(z.s);
        }
        free(p);
    }
}

static s8 gzbytes(u8 const *p, iz len)
{
    s8 r = {malloc((uz)len), len};
    memcpy(r.s, p, (uz)len);
    return r;
}

static s8 cat(s8 a, u8 const *p, iz len)
{
    if (!len) {
        return a;  // realloc to zero may free, or be undefined
    }
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

    // Trailing data, as GNU gzip takes it: zero bytes are padding, and a
    // lone byte may begin the magic of a truncated member
    static struct {
        u8  tail[4];
        i32 len;
        i32 want;
    } const tails[] = {
        {{0, 0, 0, 0},       4, GZ_OK},
        {{0},                1, GZ_OK},
        {{0, 0, 'x'},        3, GZ_TRAILING},
        {{0, 0x1f, 0x8b},    3, GZ_TRAILING},
        {{'x', 0, 0},        3, GZ_TRAILING},
        {{'x', 'y'},         2, GZ_TRAILING},
        {{'x'},              1, GZ_ETRUNC},
        {{0x1f},             1, GZ_ETRUNC},
        {{0x1f, 0x8b},       2, GZ_ETRUNC},
        {{0x1f, 0x8b, 8, 0}, 4, GZ_ETRUNC},
        {{0x1f, 0x8c, 8, 0}, 4, GZ_TRAILING},
    };
    for (i32 i = 0; i < countof(tails); i++) {
        s8 t = cat(gzbytes(member.s, member.len), tails[i].tail, tails[i].len);
        TEST(do_gunzip(ctx, a, t.s, t.len, &out) == tails[i].want);
        if (tails[i].want==GZ_OK || tails[i].want==GZ_TRAILING) {
            TEST(equals(out, payload, 5));
        }
        free(out.s);
        free(t.s);
    }

    // Padding longer than a read, and padding then garbage past a read
    iz padlen = IO_RDBUF + 100;
    s8 pad = gzbytes(member.s, member.len);
    pad.s = realloc(pad.s, (uz)(pad.len + padlen + 1));
    memset(pad.s+pad.len, 0, (uz)padlen+1);
    TEST(do_gunzip(ctx, a, pad.s, pad.len+padlen, &out) == GZ_OK);
    TEST(equals(out, payload, 5));
    free(out.s);
    pad.s[pad.len+padlen] = 'x';
    TEST(do_gunzip(ctx, a, pad.s, pad.len+padlen+1, &out) == GZ_TRAILING);
    free(out.s);
    free(pad.s);

    // Not gzip, or truncated where the magic may begin
    TEST(do_gunzip(ctx, a, (u8 *)"hello", 5, &out) == GZ_ENOTGZ);
    free(out.s);
    TEST(do_gunzip(ctx, a, (u8 *)"\0\0", 2, &out) == GZ_ENOTGZ);
    free(out.s);
    TEST(do_gunzip(ctx, a, (u8 *)"\0", 1, &out) == GZ_ENOTGZ);
    free(out.s);
    TEST(do_gunzip(ctx, a, (u8 *)"\x1f", 1, &out) == GZ_ETRUNC);
    free(out.s);
    TEST(do_gunzip(ctx, a, (u8 *)"h", 1, &out) == GZ_ETRUNC);
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

// Run the command line with a space-separated argument string, under
// the given program name.
static i32 run_as(os *ctx, arena a, char *name, char *cmdline)
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
    ctx->reads = ctx->writes = 0;
    config conf = {0};
    conf.perm = a;
    conf.name = cstrs8(name);
    conf.args = args;
    conf.nargs = nargs;
    return gzip_main(&conf);
}

static i32 run(os *ctx, arena a, char *cmdline)
{
    return run_as(ctx, a, "gzip", cmdline);
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

    // A small member goes out in one write, its trailer with the rest
    mfs_put(ctx, "e", text, 0);
    TEST(run(ctx, a, "-c f e f") == EXIT_OK);
    TEST(ctx->writes == 3);
    TEST(run(ctx, a, "-kf f") == EXIT_OK);
    TEST(ctx->writes == 1);

    // Files in one run share an encoder, reset for each, yet compress
    // exactly as each does alone, in place and to standard output
    u8 *noise = randbytes(20000, 1);
    s8 const files[] = {
        {text, 40000}, {text, 0}, {noise, 20000}, S8("abc1abc2abc3abc4"),
        {text+3000, 9000}, {text+4000, 300}, {text+5000, 1}, {text, 40000},
    };
    for (i32 level = 1; level <= 9; level += 4) {
        s8 all = {0};
        for (i32 i = 0; i < countof(files); i++) {
            char name[] = {'m', (char)('0'+i), 0};
            mfs_put(ctx, name, files[i].s, files[i].len);
        }
        char inplace[] = "-kf? m0 m1 m2 m3 m4 m5 m6 m7";
        inplace[3] = (char)('0' + level);
        TEST(run(ctx, a, inplace) == EXIT_OK);
        for (i32 i = 0; i < countof(files); i++) {
            char name[] = {'m', (char)('0'+i), '.', 'g', 'z', 0};
            s8 ref;
            TEST(do_gzip(ctx, a, files[i].s, files[i].len, level, &ref)
                 == GZ_OK);
            TEST(equals(mfs_get(ctx, name), ref.s, ref.len));
            all = cat(all, ref.s, ref.len);
            free(ref.s);
        }
        char tostdout[] = "-c? m0 m1 m2 m3 m4 m5 m6 m7";
        tostdout[2] = (char)('0' + level);
        TEST(run(ctx, a, tostdout) == EXIT_OK);
        TEST(equals(mfs_get(ctx, "<stdout>"), all.s, all.len));
        free(all.s);
    }
    free(noise);

    // Files in one run share a decoder, reset for each, yet decompress
    // or test exactly as each does alone, whatever the previous file left
    // in it: a stream ended, cut off in its header, body (with a unit
    // pending), or trailer, or corrupt, more members, trailing garbage,
    // or bytes that are not gzip at all
    {
        u8 *rnd = randbytes(20000, 1);
        s8 hello, empty, stored;
        TEST(do_gzip(ctx, a, (u8 *)"hello, hello", 12, 6, &hello) == GZ_OK);
        TEST(do_gzip(ctx, a, text, 0, 6, &empty) == GZ_OK);
        TEST(do_gzip(ctx, a, rnd, 20000, 6, &stored) == GZ_OK);
        s8 corrupt = gzbytes(gz.s, gz.len);
        corrupt.s[corrupt.len/2] ^= 0x55;
        s8 members = cat(gzbytes(hello.s, hello.len), gz.s, gz.len);
        s8 trailing = cat(gzbytes(gz.s, gz.len), (u8 *)"junk", 4);
        s8 const zs[] = {
            gz, {gz.s, gz.len/2}, hello, corrupt, empty, {gz.s, 3}, members,
            trailing, S8("not gzip"), stored, {gz.s, gz.len-3}, hello, gz,
        };
        i32 const nz = countof(zs);
        char names[countof(zs)][8];
        for (i32 i = 0; i < nz; i++) {
            snprintf(names[i], sizeof(names[i]), "z%c.gz", 'a'+i);
            mfs_put(ctx, names[i], zs[i].s, zs[i].len);
        }
        static char const *const modes[] = {
            "-dc", "-t", "-dkf", "-dcf", "-tf",
        };
        for (i32 m = 0; m < countof(modes); m++) {
            char cmd[256];
            iz len = snprintf(cmd, sizeof(cmd), "%s", modes[m]);
            s8 out = {0}, err = {0}, plain[countof(zs)];
            i32 code = EXIT_OK;
            for (i32 i = 0; i < nz; i++) {
                char alone[32];
                snprintf(alone, sizeof(alone), "%s %s", modes[m], names[i]);
                code = exit_combine(code, run(ctx, a, alone));
                s8 o = mfs_get(ctx, "<stdout>");
                s8 e = mfs_get(ctx, "<stderr>");
                out = cat(out, o.s, o.len);
                err = cat(err, e.s, e.len);
                names[i][2] = 0;  // in place: the output, gone if failed
                plain[i] = mfs_get(ctx, names[i]);
                plain[i] = plain[i].s ? dup8(plain[i]) : plain[i];
                os_remove(ctx, cstrs8(names[i]), a);
                names[i][2] = '.';
                len += snprintf(cmd+len, sizeof(cmd)-(uz)len, " %s", names[i]);
            }
            TEST(len < countof(cmd));
            TEST(run(ctx, a, cmd) == code);
            TEST(equals(mfs_get(ctx, "<stdout>"), out.s, out.len));
            TEST(equals(mfs_get(ctx, "<stderr>"), err.s, err.len));
            for (i32 i = 0; i < nz; i++) {
                names[i][2] = 0;
                s8 p = mfs_get(ctx, names[i]);
                TEST(!p.s == !plain[i].s);
                TEST(!p.s || equals(p, plain[i].s, plain[i].len));
                os_remove(ctx, cstrs8(names[i]), a);
                names[i][2] = '.';
                free(plain[i].s);
            }
            free(err.s);
            free(out.s);
        }
        for (i32 i = 0; i < nz; i++) {
            os_remove(ctx, cstrs8(names[i]), a);
        }
        free(trailing.s);
        free(members.s);
        free(corrupt.s);
        free(stored.s);
        free(empty.s);
        free(hello.s);
        free(rnd);
    }

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

    // Already has suffix, which as in GNU gzip is no failure, unless the
    // file is missing; unknown suffix
    TEST(run(ctx, a, "f.gz") == EXIT_OK);
    TEST(stderr_has(ctx, "gzip: f.gz already has .gz suffix -- unchanged"));
    TEST(run(ctx, a, "-q f.gz") == EXIT_OK);
    TEST(!mfs_get(ctx, "<stderr>").len);
    TEST(run(ctx, a, "missing.gz") == EXIT_ERR);
    TEST(stderr_has(ctx, "missing.gz: No such file or directory"));
    mfs_put(ctx, "g", text, 100);
    TEST(run(ctx, a, "-d g") == EXIT_WARN);
    TEST(stderr_has(ctx, "gzip: g: unknown suffix -- ignored\n"));
    TEST(has(ctx, "g"));
    TEST(run(ctx, a, "-dq g") == EXIT_OK);  // as GNU gzip has it
    TEST(!mfs_get(ctx, "<stderr>").len);

    // An existing output is no error, and as in GNU gzip, said even -q
    mfs_put(ctx, "g.gz", text, 100);
    TEST(run(ctx, a, "-q g") == EXIT_WARN);
    TEST(stderr_has(ctx, "gzip: g.gz already exists;\tnot overwritten\n"));
    os_remove(ctx, S("g.gz"), a);

    // Bad headers, described from their bytes as GNU gzip does
    static struct {
        char *data;
        iz    len;
        char *what;
    } const headers[] = {
        {"\x1f\x8b\x07\0\0\0\0\0\0\x03", 10,
         "gzip: h.gz: unknown method 7 -- not supported\n"},
        {"\x1f\x8b\x00\0\0\0\0\0\0\x03", 10,
         "gzip: h.gz: unknown method 0 -- not supported\n"},
        {"\x1f\x8b\x08\x20\0\0\0\0\0\x03", 10,
         "gzip: h.gz is encrypted -- not supported\n"},
        {"\x1f\x8b\x08\xe1\0\0\0\0\0\x03", 10,
         "gzip: h.gz is encrypted -- not supported\n"},
        {"\x1f\x8b\x08\x40\0\0\0\0\0\x03", 10,
         "gzip: h.gz has flags 0x40 -- not supported\n"},
        {"\x1f\x8b\x08\xc1\0\0\0\0\0\x03", 10,
         "gzip: h.gz has flags 0xc1 -- not supported\n"},
        {"\x1f\x8b\x08\x02\0\0\0\0\0\x03\xff\xff", 12,
         "gzip: h.gz: header checksum 0xffff != computed checksum 0x77a7\n"},
        {"\x1f\x8b\x08\x02\0\0\0\0\0\x03\x01\0", 12,
         "gzip: h.gz: header checksum 0x0001 != computed checksum 0x77a7\n"},
    };
    for (i32 i = 0; i < countof(headers); i++) {
        mfs_put(ctx, "h.gz", (u8 *)headers[i].data, headers[i].len);
        TEST(run(ctx, a, "-tq h.gz") == EXIT_ERR);
        TEST(equals(mfs_get(ctx, "<stderr>"), (u8 *)headers[i].what,
                    (iz)strlen(headers[i].what)));
    }
    os_remove(ctx, S("h.gz"), a);

    // GNU gzip's suffixes, in any case: .tgz and .taz stand for .tar
    static struct {
        char *name;
        char *plain;
    } const suffixed[] = {
        {"s.gz", "s"}, {"U.GZ", "U"}, {"s.z", "s"}, {"s.Z", "s"},
        {"s-gz", "s"}, {"s-z", "s"}, {"s_z", "s"}, {"s_Z", "s"},
        {"s.tgz", "s.tar"}, {"s.taz", "s.tar"}, {"s.TAZ", "s.tar"},
    };
    for (i32 i = 0; i < countof(suffixed); i++) {
        char cmd[32];
        mfs_put(ctx, suffixed[i].name, gz.s, gz.len);
        snprintf(cmd, sizeof(cmd), "-d %s", suffixed[i].name);
        TEST(run(ctx, a, cmd) == EXIT_OK);
        TEST(equals(mfs_get(ctx, suffixed[i].plain), text, 50000));
        TEST(!has(ctx, suffixed[i].name));
        mfs_put(ctx, suffixed[i].name, text, 100);
        TEST(run(ctx, a, suffixed[i].name) == EXIT_OK);
        TEST(stderr_has(ctx, " suffix -- unchanged"));
        TEST(equals(mfs_get(ctx, suffixed[i].name), text, 100));
        snprintf(cmd, sizeof(cmd), "-f %s", suffixed[i].name);
        TEST(run(ctx, a, cmd) == EXIT_OK);  // as GNU gzip, compress anyway
        TEST(!has(ctx, suffixed[i].name));
        snprintf(cmd, sizeof(cmd), "%s.gz", suffixed[i].name);
        TEST(has(ctx, cmd));
        os_remove(ctx, cstrs8(cmd), a);
        os_remove(ctx, cstrs8(suffixed[i].plain), a);
    }
    TEST(run(ctx, a, "s.TAZ") == EXIT_ERR);  // missing
    TEST(stderr_has(ctx, "s.TAZ: No such file or directory"));
    TEST(run(ctx, a, "-q s.TAZ") == EXIT_ERR);
    TEST(stderr_has(ctx, "s.TAZ: No such file or directory"));

    // Decompressing or testing a missing name without a suffix tries it
    // with suffixes in turn, as GNU gzip does: zcat nf reads nf.gz
    mfs_put(ctx, "nf.gz", gz.s, gz.len);
    TEST(run(ctx, a, "-dc nf") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "<stdout>"), text, 50000));
    TEST(run(ctx, a, "-t nf") == EXIT_OK);
    TEST(run_as(ctx, a, "zcat", "nf") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "<stdout>"), text, 50000));
    TEST(run(ctx, a, "-d nf") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "nf"), text, 50000) && !has(ctx, "nf.gz"));
    TEST(run(ctx, a, "-d nf") == EXIT_WARN);  // now it exists
    TEST(stderr_has(ctx, "nf: unknown suffix"));
    mfs_put(ctx, "nf.gz", gz.s, gz.len);
    TEST(run(ctx, a, "nf") == EXIT_WARN);  // compressing: as named
    TEST(stderr_has(ctx, "gzip: nf.gz already exists;\tnot overwritten\n"));
    os_remove(ctx, S("nf"), a);
    TEST(run(ctx, a, "nf") == EXIT_ERR);
    TEST(stderr_has(ctx, "nf: No such file or directory"));
    os_remove(ctx, S("nf.gz"), a);
    static char *const others[] = {"nf.z", "nf-z", "nf.Z"};
    for (i32 i = 0; i < countof(others); i++) {
        mfs_put(ctx, others[i], gz.s, gz.len);
        TEST(run(ctx, a, "-d nf") == EXIT_OK);
        TEST(equals(mfs_get(ctx, "nf"), text, 50000) && !has(ctx, others[i]));
        os_remove(ctx, S("nf"), a);
    }
    mfs_put(ctx, "nf.gz", gz.s, gz.len);  // first in turn
    mfs_put(ctx, "nf.z", (u8 *)"junk", 4);
    TEST(run(ctx, a, "-dc nf") == EXIT_OK);
    os_remove(ctx, S("nf.gz"), a);
    os_remove(ctx, S("nf.z"), a);
    static char *const untried[] = {"nf_z", "nf-gz", "nf.tgz", "nf.gz.gz"};
    for (i32 i = 0; i < countof(untried); i++) {
        mfs_put(ctx, untried[i], gz.s, gz.len);
        TEST(run(ctx, a, "-dqc nf") == EXIT_ERR);  // an error, not quiet
        TEST(stderr_has(ctx, "nf.gz: No such file or directory"));
        os_remove(ctx, cstrs8(untried[i]), a);
    }
    mfs_put(ctx, "nf.gz.gz", gz.s, gz.len);  // nor with a suffix already
    TEST(run(ctx, a, "-dc nf.gz") == EXIT_ERR);
    TEST(stderr_has(ctx, "nf.gz: No such file or directory"));
    os_remove(ctx, S("nf.gz.gz"), a);
    mfs_create(ctx, S("nf.gz"))->isdir = 1;
    TEST(run(ctx, a, "-dc nf") == EXIT_WARN);
    TEST(stderr_has(ctx, "gzip: nf.gz is a directory -- ignored\n"));
    os_remove(ctx, S("nf.gz"), a);

    // A suffix alone has no stem, even after a directory
    static char *const stemless[] = {".gz", "d/.gz", "d\\.z"};
    for (i32 i = 0; i < countof(stemless); i++) {
        char cmd[32];
        mfs_put(ctx, stemless[i], gz.s, gz.len);
        snprintf(cmd, sizeof(cmd), "-dk %s", stemless[i]);
        TEST(run(ctx, a, cmd) == EXIT_WARN);
        TEST(stderr_has(ctx, "unknown suffix"));
        snprintf(cmd, sizeof(cmd), "%s", stemless[i]);
        TEST(run(ctx, a, cmd) == EXIT_OK);
        snprintf(cmd, sizeof(cmd), "%s.gz", stemless[i]);
        TEST(has(ctx, cmd) && !has(ctx, stemless[i]));
        os_remove(ctx, cstrs8(cmd), a);
    }

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

    // Forced, as in GNU gzip (zcat -f), data that is not gzip, whole or
    // after a member, is copied to standard output, or passes a test,
    // but in place it is still an error
    mfs_put(ctx, "plain", (u8 *)"plain\n", 6);
    TEST(run(ctx, a, "-dc plain") == EXIT_ERR);
    TEST(run(ctx, a, "-dcf plain ok.gz plain") == EXIT_OK);
    o = mfs_get(ctx, "<stdout>");
    TEST(o.len == 50012 && !memcmp(o.s, "plain\n", 6));
    TEST(!memcmp(o.s+6, text, 50000) && !memcmp(o.s+50006, "plain\n", 6));
    TEST(!mfs_get(ctx, "<stderr>").len);
    trail = cat(gzbytes(gz.s, gz.len), (u8 *)"\0junk", 5);
    mfs_put(ctx, "tr.gz", trail.s, trail.len);
    TEST(run(ctx, a, "-dcf tr.gz") == EXIT_OK);
    o = mfs_get(ctx, "<stdout>");
    TEST(o.len==50005 && !memcmp(o.s+50000, "\0junk", 5));
    TEST(run(ctx, a, "-tf tr.gz plain") == EXIT_OK);
    TEST(!mfs_get(ctx, "<stdout>").len && !mfs_get(ctx, "<stderr>").len);
    free(trail.s);
    static char const *const shorts[] = {"", "\0", "\x1f", "j"};
    for (i32 i = 0; i < countof(shorts); i++) {
        mfs_put(ctx, "short", (u8 *)shorts[i], i>0);
        TEST(run(ctx, a, "-dcf short") == EXIT_OK);
        TEST(equals(mfs_get(ctx, "<stdout>"), (u8 *)shorts[i], i>0));
    }
    set_stdin(ctx, (u8 *)"plain\n", 6);
    TEST(run(ctx, a, "-df") == EXIT_OK);  // standard input goes out too
    TEST(equals(mfs_get(ctx, "<stdout>"), (u8 *)"plain\n", 6));
    mfs_put(ctx, "p.gz", (u8 *)"plain\n", 6);
    TEST(run(ctx, a, "-df p.gz") == EXIT_ERR);
    TEST(stderr_has(ctx, "not in gzip format"));
    TEST(has(ctx, "p.gz") && !has(ctx, "p"));

    // As in GNU gzip, the header is read before the output is created, so
    // an existing output survives input that turns out not to be gzip,
    // even forced, and without -f that is the error, not the output
    static struct {
        char *data;
        iz    len;
        char *why;
    } const notgz[] = {
        {"plain\n", 6, "not in gzip format"},
        {"", 0, "unexpected end of file"},
        {"\x1f", 1, "unexpected end of file"},
        {"\x1f\x8b", 2, "unexpected end of file"},
        {"\x1f\x8b\x08\x08\0\0\0\0\0\x03name", 14, "unexpected end of file"},
        {"\x1f\x8b\x07\0\0\0\0\0\0\x03", 10, "unknown"},
        {"\x1f\x8b\x08\x20\0\0\0\0\0\x03", 10, "encrypted"},
        {"\x1f\x8b\x08\x02\0\0\0\0\0\x03\xff\xff", 12, "checksum"},
    };
    for (i32 i = 0; i < countof(notgz); i++) {
        static char *const cmds[] = {"-df p.gz", "-d p.gz", "-dfk p.gz"};
        for (i32 c = 0; c < countof(cmds); c++) {
            mfs_put(ctx, "p.gz", (u8 *)notgz[i].data, notgz[i].len);
            mfs_put(ctx, "p", (u8 *)"keep me\n", 8);
            TEST(run(ctx, a, cmds[c]) == EXIT_ERR);
            TEST(stderr_has(ctx, notgz[i].why));
            TEST(equals(mfs_get(ctx, "p"), (u8 *)"keep me\n", 8));
            TEST(equals(mfs_get(ctx, "p.gz"), (u8 *)notgz[i].data,
                        notgz[i].len));
        }
    }
    // ...but a member corrupt after its header replaces it, as there
    s8 cut = gzbytes(gz.s, gz.len);
    cut.s[cut.len/2] ^= 0x55;
    mfs_put(ctx, "p.gz", cut.s, cut.len);
    TEST(run(ctx, a, "-d p.gz") == EXIT_WARN);
    TEST(stderr_has(ctx, "already exists"));
    TEST(equals(mfs_get(ctx, "p"), (u8 *)"keep me\n", 8));
    TEST(run(ctx, a, "-df p.gz") == EXIT_ERR);
    TEST(stderr_has(ctx, "invalid compressed data"));
    TEST(has(ctx, "p.gz") && !has(ctx, "p"));
    os_remove(ctx, S("p.gz"), a);
    free(cut.s);

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
    TEST(stderr_has(ctx, "missing: No such file or directory"));
    mfs_create(ctx, S("dir"))->isdir = 1;
    TEST(run(ctx, a, "dir") == EXIT_WARN);
    TEST(stderr_has(ctx, "is a directory"));
    TEST(run(ctx, a, "-x f") == EXIT_ERR);
    TEST(run(ctx, a, "--bogus") == EXIT_ERR);
    TEST(run(ctx, a, "-h") == EXIT_OK);
    TEST(mfs_get(ctx, "<stdout>").len > 0);
    TEST(run(ctx, a, "--version") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "<stdout>"), (u8 *)"gzip (tugz) 1.0\n", 16));

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

    // Headers never carry a name or time, as GNU gzip's under -n, which
    // is accepted, but -N, which asks for them, is not
    TEST(run(ctx, a, "-c l") == EXIT_OK);
    s8 noname = dup8(mfs_get(ctx, "<stdout>"));
    TEST(noname.len>8 && !memcmp(noname.s+3, "\0\0\0\0\0", 5));
    static char *const nflags[] = {"-nc l", "-6n -c l", "--no-name -c l"};
    for (i32 i = 0; i < countof(nflags); i++) {
        TEST(run(ctx, a, nflags[i]) == EXIT_OK);
        TEST(equals(mfs_get(ctx, "<stdout>"), noname.s, noname.len));
    }
    free(noname.s);
    TEST(run(ctx, a, "-dnc l.gz") == EXIT_OK);
    TEST(equals(mfs_get(ctx, "<stdout>"), text, 1000));
    TEST(run(ctx, a, "-Nc l") == EXIT_ERR);
    TEST(run(ctx, a, "--name -c l") == EXIT_ERR);
    TEST(!mfs_get(ctx, "<stdout>").len);

    free(gz.s);
    free(text);
}

static void test_program_names(os *ctx, arena a)
{
    u8 *text = randbytes(5000, 2);
    s8 gz;
    TEST(do_gzip(ctx, a, text, 5000, 6, &gz) == GZ_OK);
    mfs_reset(ctx);

    static char *const unzips[] = {"gunzip", "GUNZIP", "ungzip", "unpack"};
    for (i32 i = 0; i < countof(unzips); i++) {
        mfs_put(ctx, "u.gz", gz.s, gz.len);
        TEST(run_as(ctx, a, unzips[i], "u.gz") == EXIT_OK);
        TEST(equals(mfs_get(ctx, "u"), text, 5000));
        TEST(!has(ctx, "u.gz"));
        mfs_create(ctx, S("u"))->live = 0;
    }

    static char *const cats[] = {"zcat", "gzcat", "ZCat"};
    for (i32 i = 0; i < countof(cats); i++) {
        mfs_put(ctx, "z.gz", gz.s, gz.len);
        mfs_put(ctx, "z", gz.s, gz.len);  // no suffix needed with -c
        TEST(run_as(ctx, a, cats[i], "z.gz z") == EXIT_OK);
        s8 o = mfs_get(ctx, "<stdout>");
        TEST(o.len == 10000);
        TEST(!memcmp(o.s, text, 5000) && !memcmp(o.s+5000, text, 5000));
        TEST(has(ctx, "z.gz") && has(ctx, "z"));
    }

    // Other names compress
    static char *const zips[] = {"gzip", "", "tugz", "cat", "u"};
    for (i32 i = 0; i < countof(zips); i++) {
        mfs_put(ctx, "c", text, 5000);
        TEST(run_as(ctx, a, zips[i], "c") == EXIT_OK);
        TEST(has(ctx, "c.gz") && !has(ctx, "c"));
        mfs_find(ctx, S("c.gz"))->live = 0;
    }

    // Options still apply on top of the name's mode
    mfs_put(ctx, "c", text, 5000);
    TEST(run_as(ctx, a, "gunzip", "-t c") == EXIT_ERR);  // not gzip data
    mfs_put(ctx, "c.gz", gz.s, gz.len);
    TEST(run_as(ctx, a, "gunzip", "-t c.gz") == EXIT_OK);

    free(gz.s);
    free(text);
}

static void test_cli_safety(os *ctx, arena a)
{
    u8 *text = randbytes(20000, 2);
    s8 out;

    // Metadata follows the data in both directions, with the access time
    // the input had before it was read, as in GNU gzip
    mfs_reset(ctx);
    mfile *f = mfs_create(ctx, S("m"));
    mfs_append(f, text, 20000);
    f->mode = 0640;
    f->mtime = 1234567890;
    f->atime = 1234;
    TEST(run(ctx, a, "m") == EXIT_OK);
    f = mfs_find(ctx, S("m.gz"));
    TEST(f && f->mode==0640 && f->mtime==1234567890 && f->atime==1234);
    f->mode = 0604;
    f->mtime = 42;
    f->atime = 43;
    TEST(run(ctx, a, "-d m.gz") == EXIT_OK);
    f = mfs_find(ctx, S("m"));
    TEST(f && f->mode==0604 && f->mtime==42 && f->atime==43);
    TEST(equals(mfs_get(ctx, "m"), text, 20000));

    // As there, failing to set them loses no data, so the output is kept
    // and the input removed, with a warning naming the output
    ctx->failmeta = 1;
    TEST(run(ctx, a, "m") == EXIT_WARN);
    TEST(stderr_has(ctx, "gzip: m.gz: Operation not permitted\n"));
    TEST(!has(ctx, "m") && has(ctx, "m.gz"));
    TEST(run(ctx, a, "-dq m.gz") == EXIT_WARN);
    TEST(!mfs_get(ctx, "<stderr>").len);
    TEST(equals(mfs_get(ctx, "m"), text, 20000) && !has(ctx, "m.gz"));
    ctx->noreason = 1;
    TEST(run(ctx, a, "-k m") == EXIT_WARN);
    TEST(stderr_has(ctx, "gzip: m.gz: cannot set metadata\n"));
    ctx->noreason = 0;
    ctx->failmeta = 0;
    os_remove(ctx, S("m.gz"), a);

    // ...but failing to get them is an error, before any output
    ctx->failstat = 1;
    TEST(run(ctx, a, "-q m") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: m: Input/output error\n"));
    TEST(has(ctx, "m") && !has(ctx, "m.gz"));
    TEST(run(ctx, a, "-c m") == EXIT_OK);  // not needed
    ctx->failstat = 0;

    // Symbolic links are skipped in place unless forced
    f = mfs_create(ctx, S("lnk"));
    mfs_append(f, text, 100);
    f->issymlink = 1;
    // ...which, as for GNU gzip, is an error, not a warning
    TEST(run(ctx, a, "lnk") == EXIT_ERR);
    TEST(stderr_has(ctx, "is a symbolic link"));
    TEST(run(ctx, a, "-q lnk") == EXIT_ERR);
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
    TEST(stderr_has(ctx, "gzip: hard has other links -- file ignored\n"));
    TEST(has(ctx, "hard") && !has(ctx, "hard.gz"));
    TEST(run(ctx, a, "-f hard") == EXIT_OK);
    TEST(has(ctx, "hard.gz"));

    // Special files are never replaced, but may be read
    f = mfs_create(ctx, S("fifo"));
    mfs_append(f, text, 100);
    f->isspecial = 1;
    TEST(run(ctx, a, "fifo") == EXIT_WARN);
    TEST(stderr_has(ctx, "gzip: fifo is not a directory or a regular file "
                         "- ignored\n"));
    TEST(run(ctx, a, "-f fifo") == EXIT_WARN);
    TEST(has(ctx, "fifo") && !has(ctx, "fifo.gz"));
    TEST(run(ctx, a, "-c fifo") == EXIT_OK);
    s8 o = mfs_get(ctx, "<stdout>");
    TEST(do_gunzip(ctx, a, o.s, o.len, &out) == GZ_OK);
    TEST(equals(out, text, 100));
    free(out.s);

    // Failure to close the output keeps the input and removes the output,
    // naming the output, as in GNU gzip, with the reason
    mfs_put(ctx, "c", text, 20000);
    ctx->failclose = 1;
    TEST(run(ctx, a, "c") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: c.gz: Disk quota exceeded\n"));
    ctx->failclose = 0;
    TEST(equals(mfs_get(ctx, "c"), text, 20000));
    TEST(!has(ctx, "c.gz"));

    // ...as when it cannot be created
    ctx->failcreate = 1;
    TEST(run(ctx, a, "c") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: c.gz: Permission denied\n"));
    TEST(run(ctx, a, "-f c") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: c.gz: Permission denied\n"));
    ctx->failcreate = 0;
    TEST(has(ctx, "c") && !has(ctx, "c.gz"));

    // Where the system gives no reason, the failure is described
    ctx->noreason = 1;
    TEST(run(ctx, a, "missing") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: missing: cannot open for reading\n"));
    ctx->failcreate = 1;
    TEST(run(ctx, a, "c") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: c.gz: cannot open for writing\n"));
    ctx->failcreate = 0;
    ctx->failclose = 1;
    TEST(run(ctx, a, "c") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: c.gz: write error\n"));
    ctx->failclose = 0;
    ctx->failread = 1;
    TEST(run(ctx, a, "-c c") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: c: read error\n"));
    ctx->failread = 0;
    ctx->failremove = 1;
    TEST(run(ctx, a, "c") == EXIT_WARN);
    TEST(stderr_has(ctx, "gzip: c: cannot remove input file\n"));
    ctx->failremove = 0;
    ctx->noreason = 0;
    os_remove(ctx, S("c.gz"), a);

    // Write failure mid-stream: nothing partial left behind
    ctx->failwrite = 1;
    TEST(run(ctx, a, "c") == EXIT_ERR);
    ctx->failwrite = 0;
    TEST(has(ctx, "c") && !has(ctx, "c.gz"));

    // Failure to remove the input loses nothing: as in GNU gzip, both
    // files remain, with a warning
    ctx->failremove = 1;
    TEST(run(ctx, a, "c") == EXIT_WARN);
    TEST(stderr_has(ctx, "c: Operation not permitted"));
    TEST(has(ctx, "c") && has(ctx, "c.gz"));
    TEST(run(ctx, a, "-dqf c.gz") == EXIT_WARN);
    TEST(!mfs_get(ctx, "<stderr>").len);
    TEST(equals(mfs_get(ctx, "c"), text, 20000) && has(ctx, "c.gz"));
    ctx->failremove = 0;
    os_remove(ctx, S("c.gz"), a);

    // Terminals: no compressed data written to or read from one
    mfs_put(ctx, "t", text, 1000);
    ctx->tty[1] = 1;
    set_stdin(ctx, text, 1000);
    TEST(run(ctx, a, "") == EXIT_ERR);
    TEST(stderr_has(ctx, "not written to a terminal"));
    TEST(!mfs_get(ctx, "<stdout>").len);
    set_stdin(ctx, text, 1000);
    TEST(run(ctx, a, "-cf") == EXIT_OK);
    TEST(mfs_get(ctx, "<stdout>").len > 0);
    TEST(run(ctx, a, "-c t") == EXIT_OK);  // named files are not checked
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

// As in GNU gzip, a read or write error stops reading and writing at
// once, and ends the run
static void test_io_stops(os *ctx, arena a)
{
    iz len = 4<<20;  // 16 reads, compressed or not
    u8 *big = randbytes(len, 1);
    s8 bigz, out;
    TEST(do_gzip(ctx, a, big, len, 6, &bigz) == GZ_OK);
    TEST(bigz.len > len);
    mfs_reset(ctx);
    mfs_put(ctx, "r", big, len);
    mfs_put(ctx, "z.gz", bigz.s, bigz.len);
    mfs_put(ctx, "s", big, 1000);
    mfs_put(ctx, "plain", (u8 *)"plain\n", 6);

    // A read error leaves a stream unfinished, so that it cannot pass for
    // all of the input, as finishing it would
    ctx->failread = 1;
    ctx->failreadat = len/2;
    TEST(run(ctx, a, "-c r missing") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: r: Input/output error\n"));
    TEST(!stderr_has(ctx, "missing"));
    s8 o = dup8(mfs_get(ctx, "<stdout>"));
    ctx->failread = 0;
    TEST(do_gunzip(ctx, a, o.s, o.len, &out) == GZ_ETRUNC);
    free(out.s);
    free(o.s);
    ctx->failread = 1;
    TEST(run(ctx, a, "r s") == EXIT_ERR);
    TEST(has(ctx, "r") && !has(ctx, "r.gz") && !has(ctx, "s.gz"));
    TEST(run(ctx, a, "-dc z.gz missing") == EXIT_ERR);
    TEST(stderr_has(ctx, "gzip: z.gz: Input/output error\n"));
    TEST(!stderr_has(ctx, "missing"));
    TEST(run(ctx, a, "-d z.gz s") == EXIT_ERR);
    TEST(has(ctx, "z.gz") && !has(ctx, "z") && !has(ctx, "s.gz"));
    ctx->failreadat = 0;
    set_stdin(ctx, big, 1000);
    TEST(run(ctx, a, "-c - s") == EXIT_ERR);
    TEST(stderr_has(ctx, "stdin: Input/output error"));
    TEST(!mfs_get(ctx, "<stdout>").len);  // not even an empty stream
    ctx->failread = 0;

    // Nor does anything more get read after a write error, nor any more
    // files, whether compressing, decompressing, or passing data through.
    // A write error names the output. As in GNU gzip, a closed pipe, which
    // the default SIGPIPE would have ended quietly, is only a warning.
    static struct {
        char *cmd;
        char *out;
    } const cmds[] = {
        {"-c r missing",       "gzip: stdout: "},
        {"r missing",          "gzip: r.gz: "},
        {"-dc z.gz missing",   "gzip: stdout: "},
        {"-dk z.gz missing",   "gzip: z: "},
        {"-dcf z.gz missing",  "gzip: stdout: "},
        {"-dcf plain missing", "gzip: stdout: "},
    };
    for (i32 i = 0; i < countof(cmds); i++) {
        char want[64];
        ctx->failwrite = 1;
        TEST(run(ctx, a, cmds[i].cmd) == EXIT_ERR);
        ctx->failwrite = 0;
        TEST(ctx->writes==1 && ctx->reads<len/IO_RDBUF/2);
        snprintf(want, sizeof(want), "%sNo space left on device\n",
                 cmds[i].out);
        TEST(stderr_has(ctx, want) && !stderr_has(ctx, "missing"));
        TEST(has(ctx, "r") && !has(ctx, "r.gz"));
        TEST(has(ctx, "z.gz") && !has(ctx, "z"));

        ctx->brokenpipe = 1;
        TEST(run(ctx, a, cmds[i].cmd) == EXIT_WARN);
        TEST(ctx->writes==1 && ctx->reads<len/IO_RDBUF/2);
        snprintf(want, sizeof(want), "%sBroken pipe\n", cmds[i].out);
        TEST(stderr_has(ctx, want) && !stderr_has(ctx, "missing"));
        char quiet[64];
        snprintf(quiet, sizeof(quiet), "-q %s", cmds[i].cmd);
        TEST(run(ctx, a, quiet) == EXIT_WARN);
        TEST(!mfs_get(ctx, "<stderr>").len);
        ctx->brokenpipe = 0;
        TEST(has(ctx, "r") && !has(ctx, "r.gz"));
        TEST(has(ctx, "z.gz") && !has(ctx, "z"));
    }

    free(bigz.s);
    free(big);
}

static void test_oom(os *ctx, arena a)
{
    // Every truncation of the arena must fail cleanly via os_exit
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
        fprintf(stderr, "unexpected os_exit\n");
        __builtin_trap();
    }

    test_tables();
    test_huffman();
    test_complete_codes(&ctx, a);
    test_inflate_vectors(&ctx, a);
    test_inflate_repeats(&ctx, a);
    test_inflate_splits(a);
    test_container(&ctx, a);
    test_io_errors(&ctx, a);
    test_cli(&ctx, a);
    test_cli_safety(&ctx, a);
    test_io_stops(&ctx, a);
    test_program_names(&ctx, a);
    test_oom(&ctx, a);
    test_push_invariance(&ctx, a);
    test_large_offset(&ctx, a);
    test_golden(a);
    test_inflate_zlib(&ctx, a);
    test_roundtrip(&ctx, a);

    puts("all tests pass");
    return 0;
}
