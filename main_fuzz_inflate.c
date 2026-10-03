// libFuzzer harness: arbitrary input to the gzip and raw DEFLATE decoders
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined main_fuzz_inflate.c
// $ ./a.out -max_len=65536 corpus/
#include "test/fuzzos.c"

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
    return 0;
}
