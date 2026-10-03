// In-memory platform layer shared by the libFuzzer harnesses
// Standard input reads from a buffer, standard output appends to a
// bounded buffer (writes past the bound fail), and os_fail longjmps.
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/inflate.c"
#include "../src/deflate.c"
#include "../src/gzip.c"

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
};

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    (void)ctx; (void)path; (void)mode; (void)scratch;
    __builtin_trap();
}

static void os_close(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
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
        return 0;
    }
    memcpy(ctx->out + ctx->outlen, buf, (uz)len);
    ctx->outlen += len;
    return 1;
}

static void os_fail(os *ctx)
{
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
}

static i32 fuzz_inflate(fuzzenv *env, u8 const *in, iz len)
{
    fuzz_io(env, in, len);
    arena a = env->perm;
    reader   *r = newreader(&a, 0, 1<<12);
    writer   *w = newwriter(&a, 1, 1<<12);
    inflator *s = inflate_new(&a, r, w);
    i32 status = inflate_run(s);
    writer_flush(w);
    return status;
}

static i32 fuzz_gunzip(fuzzenv *env, u8 const *in, iz len)
{
    fuzz_io(env, in, len);
    return gzip_decompress(0, 1, env->perm);
}

// Raw deflate in pieces of the given size (0 for whole) at a stream base.
static void fuzz_deflate(fuzzenv *env, u8 const *in, iz len, i32 level,
                         iz piece, u64 base)
{
    fuzz_io(env, 0, 0);
    arena a = env->perm;
    writer   *w = newwriter(&a, 1, 1<<12);
    deflator *d = deflate_new(&a, level, w);
    d->base = base;
    for (iz off = 0; off < len;) {
        iz n = piece ? MIN(piece, len-off) : len-off;
        deflate_push(d, in+off, n);
        off += n;
    }
    deflate_finish(d);
    writer_flush(w);
    CHECK(!w->err);
}
