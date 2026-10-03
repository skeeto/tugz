// libFuzzer differential harness: our decoders versus zlib
// Raw DEFLATE: both must agree exactly on accept/reject and on output.
// gzip: our decoder versus a zlib multi-member loop that applies the GNU
// gzip trailing-data policy (non-magic trailing bytes are a warning).
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined main_fuzz_diff_inflate.c -lz
#include "test/fuzzos.c"
#include <zlib.h>

#define OUTCAP ((iz)1 << 24)

static u8 *zout;

// Returns 1 if zlib accepts, 0 if it rejects, -1 if output is too long.
static i32 zlib_raw(u8 const *in, iz len, iz *outlen)
{
    z_stream z = {0};
    CHECK(inflateInit2(&z, -15) == Z_OK);
    z.next_in = (u8 *)in;
    z.avail_in = (u32)len;
    z.next_out = zout;
    z.avail_out = (u32)OUTCAP;
    i32 r = inflate(&z, Z_FINISH);
    *outlen = (iz)z.total_out;
    inflateEnd(&z);
    if (r == Z_STREAM_END) {
        return 1;
    }
    return r==Z_BUF_ERROR && !z.avail_out ? -1 : 0;
}

static i32 zlib_gzip(u8 const *in, iz len, iz *outlen, b32 *trailing)
{
    z_stream z = {0};
    CHECK(inflateInit2(&z, 31) == Z_OK);
    *outlen = 0;
    *trailing = 0;
    i32 result = 0;
    for (iz off = 0;; ) {
        iz rem = len - off;
        if (off) {
            if (rem<2 || in[off]!=0x1f || in[off+1]!=0x8b) {
                *trailing = rem > 0;
                result = 1;
                break;
            }
            inflateReset(&z);
        } else if (!rem) {
            break;
        }
        z.next_in = (u8 *)in + off;
        z.avail_in = (u32)rem;
        z.next_out = zout + *outlen;
        z.avail_out = (u32)(OUTCAP - *outlen);
        i32 r = inflate(&z, Z_FINISH);
        *outlen = OUTCAP - z.avail_out;
        if (r != Z_STREAM_END) {
            result = r==Z_BUF_ERROR && !z.avail_out ? -1 : 0;
            break;
        }
        off = len - z.avail_in;
    }
    inflateEnd(&z);
    return result;
}

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    fuzzenv *env = fuzz_env(OUTCAP);
    if (!zout) {
        zout = malloc((uz)OUTCAP);
    }
    if (setjmp(env->ctx.fail)) {
        __builtin_trap();
    }

    iz zlen;
    i32 want = zlib_raw(data, (iz)size, &zlen);
    i32 got = fuzz_inflate(env, data, (iz)size);
    if (want>=0 && got!=GZ_EWRITE) {
        CHECK(want == (got==GZ_OK));
        if (want) {
            CHECK(zlen == env->ctx.outlen);
            CHECK(!memcmp(zout, env->ctx.out, (uz)zlen));
        }
    }

    b32 trailing;
    want = zlib_gzip(data, (iz)size, &zlen, &trailing);
    got = fuzz_gunzip(env, data, (iz)size);
    if (want>=0 && got!=GZ_EWRITE) {
        CHECK(want == (got==GZ_OK || got==GZ_TRAILING));
        if (want) {
            CHECK(trailing == (got==GZ_TRAILING));
            CHECK(zlen == env->ctx.outlen);
            CHECK(!memcmp(zout, env->ctx.out, (uz)zlen));
        }
    }
    return 0;
}
