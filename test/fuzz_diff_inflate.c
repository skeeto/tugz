// libFuzzer differential harness: our decoders versus zlib
// Raw DEFLATE: both must agree exactly on accept/reject and on output.
// gzip: our decoder versus a zlib multi-member loop that applies the GNU
// gzip trailing-data policy (non-magic trailing bytes are a warning,
// unless all zero; a lone non-zero byte is a truncation).
// Streaming: the first byte selects a format and input/output piece
// sizes for the rest. Our streaming decoder must then agree with zlib on
// success, on truncation versus error, on output, and on exactly where
// the stream ends.
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined test/fuzz_diff_inflate.c -lz
#include "fuzzos.c"
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
            if (rem==1 && in[off]) {
                break;  // perhaps the start of the magic: truncated
            } else if (rem<2 || in[off]!=0x1f || in[off+1]!=0x8b) {
                iz zeros = 0;
                for (; zeros<rem && !in[off+zeros]; zeros++) {}
                *trailing = zeros < rem;  // zero bytes are padding
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

static void diff_stream(fuzzenv *env, u8 cfg, u8 const *in, iz len)
{
    enum { CAP = 1 << 20 };
    i32 format = cfg % 3;
    static i32 const wbits[] = {-15, 15, 31};
    z_stream z = {0};
    CHECK(inflateInit2(&z, wbits[format]) == Z_OK);
    z.next_in = (u8 *)in;
    z.avail_in = (u32)len;
    z.next_out = zout;
    z.avail_out = CAP;
    i32 want = inflate(&z, Z_FINISH);
    iz zlen = CAP - z.avail_out;
    iz zused = len - z.avail_in;
    inflateEnd(&z);
    if (want==Z_BUF_ERROR && !z.avail_out) {
        return;  // output too long to compare
    }

    iz used;
    i32 got = fuzz_decode(env, format, in, len, fuzz_pieces[cfg>>2 & 7],
                          fuzz_pieces[cfg>>5], CAP, &used);
    CHECK(got != GZ_NEEDOUT);
    switch (want) {
    case Z_STREAM_END:
        CHECK(got == GZ_OK);
        CHECK(used == zused);
        CHECK(env->ctx.outlen == zlen);
        CHECK(!memcmp(zout, env->ctx.out, (uz)zlen));
        break;
    case Z_BUF_ERROR:  // truncated
        if (format==FMT_ZLIB && len>=2 && in[1]&0x20) {
            // zlib reads the dictionary ID before reporting it
            CHECK(got==GZ_NEEDIN || got==GZ_EHEADER);
            break;
        }
        CHECK(got == GZ_NEEDIN);
        CHECK(env->ctx.outlen == zlen);  // nothing held back
        CHECK(!memcmp(zout, env->ctx.out, (uz)zlen));
        break;
    default:  // Z_DATA_ERROR, Z_NEED_DICT
        CHECK(got!=GZ_OK && got!=GZ_NEEDIN);
        CHECK(env->ctx.outlen == zlen);  // all output before the error
        CHECK(!memcmp(zout, env->ctx.out, (uz)zlen));
    }
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

    // GZ_EWRITE: ours accepted, with more than OUTCAP of output. That
    // agrees with zlib only if zlib's output did not fit either.
    iz zlen;
    i32 want = zlib_raw(data, (iz)size, &zlen);
    i32 got = fuzz_inflate(env, data, (iz)size);
    CHECK(want<0 || got!=GZ_EWRITE);
    if (want >= 0) {
        CHECK(want == (got==GZ_OK));
        if (want) {
            CHECK(zlen == env->ctx.outlen);
            CHECK(!memcmp(zout, env->ctx.out, (uz)zlen));
        }
    }

    b32 trailing;
    want = zlib_gzip(data, (iz)size, &zlen, &trailing);
    got = fuzz_gunzip(env, data, (iz)size);
    CHECK(want<0 || got!=GZ_EWRITE);
    if (want >= 0) {
        CHECK(want == (got==GZ_OK || got==GZ_TRAILING));
        if (want) {
            CHECK(trailing == (got==GZ_TRAILING));
            CHECK(zlen == env->ctx.outlen);
            CHECK(!memcmp(zout, env->ctx.out, (uz)zlen));
        }
    }

    if (size) {
        diff_stream(env, data[0], data+1, (iz)size-1);
    }
    return 0;
}
