// libFuzzer harness: arbitrary input to the gzip and raw DEFLATE decoders,
// and to the Deflate64 decoder
// Deflate64 has no reference decoder to compare against, so it is checked
// against itself: the first byte selects input and output piece sizes
// for the rest, which must decode in pieces exactly as it does whole,
// with the same status, output, and end.
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined test/fuzz_inflate.c
// $ ./a.out -max_len=65536 corpus/
#include "fuzzos.c"

static void deflate64_pieces(fuzzenv *env, u8 cfg, u8 const *in, iz len)
{
    enum { CAP = 1 << 20 };
    static u8 *whole;
    if (!whole) {
        whole = malloc(CAP);
    }

    iz wused;
    i32 want = fuzz_decode64(env, in, len, 0, 0, CAP, &wused);
    iz  wlen = env->ctx.outlen;
    CHECK(want==GZ_OK || want==GZ_NEEDIN || want==GZ_NEEDOUT ||
          want==GZ_EDATA);
    CHECK(wlen<=CAP && (want!=GZ_NEEDOUT || wlen==CAP));
    CHECK(want!=GZ_NEEDIN || wused==len);
    memcpy(whole, env->ctx.out, (uz)wlen);

    iz used;
    i32 got = fuzz_decode64(env, in, len, fuzz_pieces[cfg & 7],
                            fuzz_pieces[cfg>>3 & 7], CAP, &used);
    CHECK(got == want);
    CHECK(env->ctx.outlen == wlen);
    CHECK(!memcmp(whole, env->ctx.out, (uz)wlen));
    CHECK(got!=GZ_OK || used==wused);
}

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    fuzzenv *env = fuzz_env((iz)1 << 24);
    if (setjmp(env->ctx.fail)) {
        __builtin_trap();  // out of memory: should never happen
    }

    i32 status = fuzz_inflate(env, data, (iz)size);
    CHECK(status==GZ_OK || status==GZ_ETRUNC || status==GZ_EDATA ||
          status==GZ_EWRITE);
    CHECK(status!=GZ_EWRITE || env->ctx.overflow);

    status = fuzz_gunzip(env, data, (iz)size);
    CHECK(status>=GZ_OK && status<=GZ_EWRITE && status!=GZ_EREAD);
    CHECK(status!=GZ_EWRITE || env->ctx.overflow);

    if (size) {
        deflate64_pieces(env, data[0], data+1, (iz)size-1);
    }
    return 0;
}
