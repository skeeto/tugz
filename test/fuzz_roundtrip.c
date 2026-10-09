// libFuzzer harness: compress then decompress must reproduce the input
// The first bytes select the level, push size, and stream offset. Output
// must also be identical regardless of push size and offset, and from an
// encoder reset after other streams, in each format, must be within the
// bound (tugz_deflate_bound's), and must decompress identically under
// zlib.
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined test/fuzz_roundtrip.c -lz
#include "fuzzos.c"
#include <zlib.h>

// Compress all input with a flush, appending output to env->ctx.out.
static void encode_all(fuzzenv *env, encoder *e, u8 const *in, iz len,
                       i32 flush)
{
    zbuf b = {in, len, 0, 0};
    for (;;) {
        i32 status = encoder_run(e, &b, flush);
        s8  p = encoder_pending(e);
        CHECK(p.len <= env->ctx.outcap-env->ctx.outlen);
        memcpy(env->ctx.out+env->ctx.outlen, p.s, (uz)p.len);
        env->ctx.outlen += p.len;
        encoder_consume(e, p.len);
        if (status != GZ_NEEDOUT) {
            CHECK(status == (flush==DEF_NONE ? GZ_NEEDIN : GZ_OK));
            return;
        }
    }
}

// Compress with an encoder that first compressed the input repeated one
// to eight times, at the same or another level, through a sync flush or
// to the end, then was reset. Output goes to env->ctx.out.
static void reused_encode(fuzzenv *env, i32 format, u8 const *in, iz len,
                          i32 level, u8 how)
{
    arena a = env->perm;
    encoder *e = encoder_new(&a, format, how&0x40 ? level : 10-level);
    env->ctx.outlen = 0;
    i32 reps = 1 + (how>>1)%8;
    for (i32 i = 0; i < reps; i++) {
        i32 flush = i<reps-1 ? DEF_NONE : how&1 ? DEF_FINISH : DEF_SYNC;
        encode_all(env, e, in, len, flush);
    }
    encoder_reset(e, level);
    env->ctx.outlen = 0;
    encode_all(env, e, in, len, DEF_FINISH);
}

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    fuzzenv *env = fuzz_env((iz)1 << 24);
    if (setjmp(env->ctx.fail) || size<3) {
        CHECK(size < 3);
        return 0;
    }

    i32 level = 1 + data[0]%9;
    static iz const pieces[] = {0, 1, 2, 3, 7, 100, 258, 4096};
    iz  piece = pieces[data[1] % countof(pieces)];
    u64 base  = data[1]&0x80 ? 0xfffffffffull - (u64)data[2]*12345 : 0;
    u8 const *in = data + 3;
    iz len = (iz)size - 3;

    // Reference: one push, zero base, within the bound, as FINISH is the
    // only flush
    fuzz_deflate(env, in, len, level, 0, 0);
    iz zlen = env->ctx.outlen;
    CHECK((u64)zlen <= deflate_bound((u64)len));
    u8 *z = malloc((uz)zlen + 1);
    memcpy(z, env->ctx.out, (uz)zlen);

    fuzz_deflate(env, in, len, level, piece, base);
    CHECK(env->ctx.outlen == zlen);
    CHECK(!memcmp(env->ctx.out, z, (uz)zlen));

    i32 format = (data[2]>>4) % 3;
    s8  ref = {z, zlen};
    if (format != FMT_RAW) {
        ref = fuzz_encode(env, format, level, in, len, 0, 0, 0);
    }
    CHECK((u64)ref.len <= encoder_bound(format, (u64)len));
    reused_encode(env, format, in, len, level, data[2]);
    CHECK(env->ctx.outlen == ref.len);
    CHECK(!memcmp(env->ctx.out, ref.s, (uz)ref.len));
    if (format != FMT_RAW) {
        free(ref.s);
    }

    CHECK(fuzz_inflate(env, z, zlen) == GZ_OK);
    CHECK(env->ctx.outlen == len);
    CHECK(!memcmp(env->ctx.out, in, (uz)len));

    z_stream zs = {0};
    CHECK(inflateInit2(&zs, -15) == Z_OK);
    u8 *out = malloc((uz)len + 1);
    zs.next_in = z;
    zs.avail_in = (u32)zlen;
    zs.next_out = out;
    zs.avail_out = (u32)len + 1;
    CHECK(inflate(&zs, Z_FINISH) == Z_STREAM_END);
    CHECK(zs.total_out==(uLong)len && zs.avail_in==0);
    CHECK(!memcmp(out, in, (uz)len));
    inflateEnd(&zs);

    free(out);
    free(z);
    return 0;
}
