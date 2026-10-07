// Test of the single-file library, tugz.c, embedded with TUGZ_API static
// in a program that has its own assert and MIN macros and its own i64 and
// byte types, which must all survive it. Round-trips data through each
// format. On success prints "all amalgamation tests pass".
// $ cmake -DTUGZ_ARTIFACT=tugz -P cmake/amalgamate.cmake
// $ cc -I. -o tests-amalg test/amalgtests.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define assert(c)  "the program's assert"
#define MIN(a, b)  "the program's MIN"
typedef long          i64;   // as int64_t is on LP64
typedef unsigned char byte;  // as <windows.h> defines it

#define TUGZ_API static
#include "tugz.c"

#define TEST(c) \
    do { \
        if (!(c)) { \
            printf("%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
            exit(1); \
        } \
    } while (0)

int main(void)
{
    TEST(!strcmp(assert(0), "the program's assert"));
    TEST(!strcmp(MIN(0, 1), "the program's MIN"));
    i64 *wide = 0;
    long *same = wide;  // the program's i64, not the core's long long
    (void)same;
    TEST(sizeof(byte) == 1);

    enum { N = 1 << 16 };
    static unsigned char in[N], z[2*N], out[N];
    for (int i = 0; i < N; i++) {
        in[i] = (unsigned char)("tugboat"[i%7] ^ (i>>9));
    }

    for (int format = TUGZ_RAW; format <= TUGZ_GZIP; format++) {
        ptrdiff_t      dlen = tugz_deflate_size(format);
        void          *dmem = malloc(dlen);
        tugz_deflator *d    = tugz_deflate_init(dmem, dlen, format, 6);
        TEST(d);
        tugz_buf b = {in, N, z, sizeof(z)};
        TEST(tugz_deflate(d, &b, TUGZ_FINISH) == TUGZ_DONE);
        ptrdiff_t zlen = (ptrdiff_t)sizeof(z) - b.outlen;
        TEST(zlen > 0 && zlen < N/4);
        free(dmem);

        ptrdiff_t      ilen = tugz_inflate_size(format);
        void          *imem = malloc(ilen);
        tugz_inflator *f    = tugz_inflate_init(imem, ilen, format);
        TEST(f);
        tugz_buf c = {z, zlen, out, sizeof(out)};
        TEST(tugz_inflate(f, &c) == TUGZ_DONE);
        TEST(c.inlen == 0 && c.outlen == 0);
        TEST(!memcmp(in, out, N));
        free(imem);
    }

    puts("all amalgamation tests pass");
    return 0;
}
