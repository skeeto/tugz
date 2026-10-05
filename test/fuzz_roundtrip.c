// libFuzzer harness: compress then decompress must reproduce the input
// The first bytes select the level, push size, and stream offset. Output
// must also be identical regardless of push size and offset, and must
// decompress identically under zlib.
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined test/fuzz_roundtrip.c -lz
#include "fuzzos.c"
#include <zlib.h>

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

    // Reference: one push, zero base
    fuzz_deflate(env, in, len, level, 0, 0);
    iz zlen = env->ctx.outlen;
    u8 *z = malloc((uz)zlen + 1);
    memcpy(z, env->ctx.out, (uz)zlen);

    fuzz_deflate(env, in, len, level, piece, base);
    CHECK(env->ctx.outlen == zlen);
    CHECK(!memcmp(env->ctx.out, z, (uz)zlen));

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
