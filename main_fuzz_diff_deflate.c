// libFuzzer differential harness: foreign encoders versus our decoder
// The leading bytes configure zlib (level, window, memory, strategy,
// flush points, mid-stream parameter changes) or libdeflate (level), and
// the rest is compressed. Our raw decoder must reproduce it exactly. Our
// own encoder's output must also decode under both libraries.
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined \
//         main_fuzz_diff_deflate.c -lz -ldeflate
#include "test/fuzzos.c"
#include <libdeflate.h>
#include <zlib.h>

static s8 zlib_compress(u8 const *cfg, u8 const *in, iz len)
{
    i32 level    = cfg[0] % 10;
    i32 wbits    = 9 + cfg[1]%7;
    i32 memlevel = 1 + cfg[2]%9;
    i32 strategy = cfg[3] % 5;
    z_stream z = {0};
    CHECK(deflateInit2(&z, level, Z_DEFLATED, -wbits, memlevel, strategy)
          == Z_OK);

    iz cap = 2*len + 4096;
    s8 r = {malloc((uz)cap), 0};
    z.next_out = r.s;
    z.avail_out = (u32)cap;

    // Feed in segments, choosing a flush mode for each
    static i32 const flushes[] = {
        Z_NO_FLUSH, Z_NO_FLUSH, Z_SYNC_FLUSH, Z_FULL_FLUSH, Z_BLOCK,
        Z_PARTIAL_FLUSH,
    };
    u32 seed = (u32)cfg[4]<<8 | cfg[5];
    for (iz off = 0; off < len;) {
        seed = seed*1103515245 + 12345;
        iz n = MIN(len-off, 1 + (iz)(seed>>16)%2048);
        z.next_in = (u8 *)in + off;
        z.avail_in = (u32)n;
        i32 flush = flushes[(seed>>8) % countof(flushes)];
        CHECK(deflate(&z, flush) != Z_STREAM_ERROR);
        CHECK(z.avail_in == 0);
        off += n;
        if (cfg[6]&1 && (seed>>4)%5==0) {
            // Change parameters mid-stream
            i32 l = (i32)(seed>>12) % 10;
            i32 s = (i32)(seed>>20) % 5;
            CHECK(deflateParams(&z, l, s) == Z_OK);
        }
    }
    CHECK(deflate(&z, Z_FINISH) == Z_STREAM_END);
    r.len = (iz)z.total_out;
    deflateEnd(&z);
    return r;
}

static s8 libdeflate_compress(u8 cfg, u8 const *in, iz len)
{
    struct libdeflate_compressor *c = libdeflate_alloc_compressor(cfg%13);
    uz cap = libdeflate_deflate_compress_bound(c, (uz)len);
    s8 r = {malloc(cap), 0};
    r.len = (iz)libdeflate_deflate_compress(c, in, (uz)len, r.s, cap);
    CHECK(r.len);
    libdeflate_free_compressor(c);
    return r;
}

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    enum { NCFG = 8 };
    fuzzenv *env = fuzz_env((iz)1 << 24);
    if (setjmp(env->ctx.fail) || size<NCFG) {
        CHECK(size < NCFG);
        return 0;
    }
    u8 const *cfg = data;
    u8 const *in  = data + NCFG;
    iz len = (iz)size - NCFG;

    s8 z = cfg[7]&1 ? libdeflate_compress(cfg[0], in, len)
                    : zlib_compress(cfg, in, len);
    CHECK(fuzz_inflate(env, z.s, z.len) == GZ_OK);
    CHECK(env->ctx.outlen == len);
    CHECK(!memcmp(env->ctx.out, in, (uz)len));
    free(z.s);

    // Our encoder's output under libdeflate
    fuzz_deflate(env, in, len, 1 + cfg[0]%9, 0, 0);
    struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
    u8 *out = malloc((uz)len + 1);
    uz actual;
    CHECK(libdeflate_deflate_decompress(d, env->ctx.out, (uz)env->ctx.outlen,
                                        out, (uz)len, &actual)
          == LIBDEFLATE_SUCCESS);
    CHECK(actual==(uz)len && !memcmp(out, in, (uz)len));
    libdeflate_free_decompressor(d);
    free(out);
    return 0;
}
