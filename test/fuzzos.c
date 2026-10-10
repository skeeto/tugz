// In-memory platform layer shared by the codec's libFuzzer harnesses
// Standard input reads from a buffer, standard output appends to a
// bounded buffer (writes past the bound fail), and os_exit longjmps.
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/adler32.c"
#include "../src/inflate.c"
#include "../src/deflate.c"
#include "../src/gzip.c"
#include "../src/io.c"
#include "../src/gzipio.c"

#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c)  do { if (!(c)) __builtin_trap(); } while (0)

struct os {
    jmp_buf   fail;
    u8 const *in;
    iz        inlen;
    iz        inoff;
    u8       *out;
    iz        outlen;
    iz        outcap;
    b32       overflow;  // a write was refused for exceeding outcap
};

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    (void)ctx; (void)path; (void)mode; (void)scratch;
    __builtin_trap();
}

static b32 os_close(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    __builtin_trap();
}

static b32 os_keep(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    __builtin_trap();
}

static b32 os_isatty(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    __builtin_trap();
}

static i64 os_now(os *ctx)
{
    (void)ctx;
    __builtin_trap();  // gzip's writers are never a terminal's
}

static b32 os_missing(os *ctx)
{
    (void)ctx;
    __builtin_trap();
}

static s8 os_error(os *ctx)
{
    (void)ctx;
    __builtin_trap();
}

static b32 os_pipeclosed(os *ctx)
{
    (void)ctx;
    __builtin_trap();
}

static osmeta *os_getmeta(os *ctx, i32 fd, arena *a)
{
    (void)ctx; (void)fd; (void)a;
    __builtin_trap();
}

static b32 os_setmeta(os *ctx, i32 fd, osmeta *m)
{
    (void)ctx; (void)fd; (void)m;
    __builtin_trap();
}

static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)ctx; (void)path; (void)scratch;
    __builtin_trap();
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    CHECK(fd==0 && cap>0);
    iz n = MIN(cap, ctx->inlen - ctx->inoff);
    if (n) {
        memcpy(buf, ctx->in + ctx->inoff, (uz)n);
    }
    ctx->inoff += n;
    return n;
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    CHECK(fd==1 || fd==2);
    if (fd == 2) {
        return 1;
    } else if (len > ctx->outcap - ctx->outlen) {
        ctx->overflow = 1;
        return 0;
    }
    memcpy(ctx->out + ctx->outlen, buf, (uz)len);
    ctx->outlen += len;
    return 1;
}

static void os_exit(os *ctx, i32 status)
{
    (void)status;
    longjmp(ctx->fail, 1);
}

typedef struct {
    os    ctx;
    arena perm;
} fuzzenv;

static fuzzenv *fuzz_env(iz outcap)
{
    static fuzzenv *env;
    if (!env) {
        env = calloc(1, sizeof(*env));
        iz cap = (iz)1 << 26;
        env->perm.beg = malloc((uz)cap);
        env->perm.end = env->perm.beg + cap;
        env->perm.ctx = &env->ctx;
        env->ctx.out = malloc((uz)outcap);
        env->ctx.outcap = outcap;
    }
    return env;
}

static void fuzz_io(fuzzenv *env, u8 const *in, iz len)
{
    env->ctx.in = in;
    env->ctx.inlen = len;
    env->ctx.inoff = 0;
    env->ctx.outlen = 0;
    env->ctx.overflow = 0;
}

static i32 fuzz_inflate(fuzzenv *env, u8 const *in, iz len)
{
    fuzz_io(env, in, len);
    arena a = env->perm;
    return stream_decompress(stream_decoder(&a, FMT_RAW), 0, 1, 0, a);
}

static i32 fuzz_gunzip(fuzzenv *env, u8 const *in, iz len)
{
    fuzz_io(env, in, len);
    arena a = env->perm;
    return stream_decompress(stream_decoder(&a, FMT_GZIP), 0, 1, 0, a);
}

// Raw deflate in pieces of the given size (0 for whole) at a stream base.
static void fuzz_deflate(fuzzenv *env, u8 const *in, iz len, i32 level,
                         iz piece, u64 base)
{
    fuzz_io(env, 0, 0);
    arena a = env->perm;
    encoder *e = encoder_new(&a, FMT_RAW, level);
    e->def->base = base;
    zbuf b = {in, 0, 0, 0};
    for (;;) {
        iz inleft = in + len - b.in;
        b.inlen = piece ? MIN(piece, inleft) : inleft;
        b.out = env->ctx.out + env->ctx.outlen;
        b.outlen = env->ctx.outcap - env->ctx.outlen;
        u8 *out = b.out;
        i32 flush = b.inlen==inleft ? DEF_FINISH : DEF_NONE;
        i32 status = encoder_run(e, &b, flush);
        env->ctx.outlen += b.out - out;
        CHECK(status != GZ_NEEDOUT);
        if (status == GZ_OK) {
            break;
        }
        CHECK(status==GZ_NEEDIN && !b.inlen);
    }
}

static iz const fuzz_pieces[8] = {0, 1, 2, 3, 7, 16, 64, 1000};

// Decode one stream (or gzip member) in memory, feeding input and taking
// output in pieces (0 for unlimited), with output capped at cap. Returns
// GZ_OK, GZ_NEEDIN once input is exhausted, GZ_NEEDOUT at the output cap,
// or an error. Output goes to env->ctx.out.
static i32 fuzz_decode(fuzzenv *env, i32 format, u8 const *in, iz len,
                       iz inpiece, iz outpiece, iz cap, iz *used)
{
    fuzz_io(env, 0, 0);
    arena a = env->perm;
    decoder *z = decoder_new(&a, format);
    cap = MIN(cap, env->ctx.outcap);
    *used = 0;
    for (;;) {
        iz inleft  = len - *used;
        iz outleft = cap - env->ctx.outlen;
        zbuf b = {0};
        b.in     = in + *used;
        b.inlen  = inpiece  ? MIN(inpiece, inleft)   : inleft;
        b.out    = env->ctx.out + env->ctx.outlen;
        b.outlen = outpiece ? MIN(outpiece, outleft) : outleft;
        iz inlen  = b.inlen;
        iz outlen = b.outlen;
        i32 status = decoder_run(z, &b);
        *used += inlen - b.inlen;
        env->ctx.outlen += outlen - b.outlen;
        if (status == GZ_NEEDIN) {
            CHECK(!b.inlen);
            if (*used < len) {
                continue;
            }
        } else if (status == GZ_NEEDOUT) {
            CHECK(!b.outlen);
            if (env->ctx.outlen < cap) {
                continue;
            }
        }
        return status;
    }
}

// As fuzz_decode, but a raw Deflate64 stream.
static i32 fuzz_decode64(fuzzenv *env, u8 const *in, iz len, iz inpiece,
                         iz outpiece, iz cap, iz *used)
{
    fuzz_io(env, 0, 0);
    arena a = env->perm;
    inflator *s = inflate64_new(&a);
    cap = MIN(cap, env->ctx.outcap);
    *used = 0;
    for (;;) {
        iz inleft  = len - *used;
        iz outleft = cap - env->ctx.outlen;
        zbuf b = {0};
        b.in     = in + *used;
        b.inlen  = inpiece  ? MIN(inpiece, inleft)   : inleft;
        b.out    = env->ctx.out + env->ctx.outlen;
        b.outlen = outpiece ? MIN(outpiece, outleft) : outleft;
        iz inlen  = b.inlen;
        iz outlen = b.outlen;
        i32 status = inflate_stream(s, &b);
        *used += inlen - b.inlen;
        env->ctx.outlen += outlen - b.outlen;
        if (status == GZ_NEEDIN) {
            CHECK(!b.inlen);
            if (*used < len) {
                continue;
            }
        } else if (status == GZ_NEEDOUT) {
            CHECK(!b.outlen);
            if (env->ctx.outlen < cap) {
                continue;
            }
        }
        return status;
    }
}

typedef struct {
    iz  end;
    i32 flush;
    b32 early;
} fuzzseg;

// The next segment of fuzz_encode's input after off, from its seed.
static fuzzseg fuzz_segment(u32 *seed, iz off, iz len, b32 finishing)
{
    fuzzseg s = {len, DEF_FINISH, 0};
    if (*seed && !finishing) {
        u32 x = *seed = *seed*1103515245 + 12345;
        iz seglen = x>>27 & 3 ? 1 + (iz)(x>>16)%2048 : 0;
        s.end = MIN(len, off + seglen);
        static i32 const modes[] = {DEF_NONE, DEF_NONE, DEF_SYNC, DEF_FULL};
        s.flush = modes[(x>>8)%4];
        if (s.end==len && s.flush==DEF_NONE) {
            s.flush = DEF_FINISH;
        }
        s.early = x>>30 & 1;
    }
    return s;
}

// Encode in memory, feeding input and taking output in pieces (0 for
// unlimited). A nonzero seed splits input into segments, a quarter of
// them empty, each ending in a flush chosen from NONE, SYNC, or FULL,
// and the last in FINISH, perhaps after empty SYNC or FULL segments. It
// sometimes moves on to the next segment before the flush completes
// (from FINISH, to an empty FINISH), which must not change the output:
// an empty segment's flush then falls due behind unfinished ones. But
// not where the next segment is empty and in the mode of the latest
// flush, which would then only complete that flush rather than flush
// again as when waited for. Output must be within the bound, allowing
// for its flushes. Returns a malloc'd buffer.
static s8 fuzz_encode(fuzzenv *env, i32 format, i32 level, u8 const *in,
                      iz len, iz inpiece, iz outpiece, u32 seed)
{
    arena a = env->perm;
    encoder *e = encoder_new(&a, format, level);
    iz cap = 8*len + (1<<16);
    s8 r = {malloc((uz)cap), 0};
    iz off = 0;
    iz nflush = 0;  // SYNC and FULL flushes, each within DEF_FLUSH_BOUND
    i32 latest = DEF_NONE;  // latest flush, unless input completed it
    fuzzseg seg = fuzz_segment(&seed, 0, len, 0);
    for (b32 last = 0; !last;) {
        iz  end   = seg.end;
        i32 flush = seg.flush;
        last = flush==DEF_FINISH && !seg.early;
        latest = flush ? flush : end>off ? DEF_NONE : latest;
        fuzzseg next = fuzz_segment(&seed, end, len, flush==DEF_FINISH);
        b32 early = seg.early && (next.end>end || next.flush!=latest ||
                                  latest==DEF_FINISH);
        for (;;) {
            zbuf b = {0};
            b.in     = in + off;
            b.inlen  = inpiece ? MIN(inpiece, end-off) : end-off;
            b.out    = r.s + r.len;
            b.outlen = outpiece ? MIN(outpiece, cap-r.len) : cap-r.len;
            iz inlen = b.inlen;
            i32 status = encoder_run(e, &b, off+inlen==end ? flush : DEF_NONE);
            off += inlen - b.inlen;
            r.len = b.out - r.s;
            CHECK(r.len < cap);
            if (status == GZ_NEEDOUT) {
                CHECK(!b.outlen);
                if (early && off==end) {
                    break;  // the flush is due, and completes in later calls
                }
                continue;
            } else if (status == GZ_NEEDIN) {
                CHECK(!b.inlen);
                if (off<end || flush!=DEF_NONE) {
                    CHECK(off < end);
                    continue;
                }
                break;
            }
            CHECK(status == GZ_OK);
            CHECK(off==end && flush!=DEF_NONE);
            break;
        }
        nflush += flush==DEF_SYNC || flush==DEF_FULL;
        seg = next;
    }
    CHECK((u64)r.len <= encoder_bound(format, (u64)len) +
                        DEF_FLUSH_BOUND*(u64)nflush);
    return r;
}
