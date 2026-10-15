// tugz core: Adler-32 checksum (RFC 1950)

#define ADLER_MOD   65521
#define ADLER_NMAX  5552  // most bytes before the sums may overflow 32 bits

static u32 adler32_scalar(u32 adler, u8 const *p, iz len)
{
    u32 a = adler & 0xffff;
    u32 b = adler >> 16;
    while (len) {
        iz n = MIN(len, ADLER_NMAX);
        len -= n;
        for (; n >= 16; n -= 16, p += 16) {
            for (i32 i = 0; i < 16; i++) {
                a += p[i];
                b += a;
            }
        }
        for (; n; n--) {
            a += *p++;
            b += a;
        }
        a %= ADLER_MOD;
        b %= ADLER_MOD;
    }
    return a | b<<16;
}

#if __SSE2__
// SSE2, baseline on x86-64 (i686 keeps the scalar loop): 32 bytes per
// step. Over a block of n bytes, a byte k bytes in adds itself to a and
// (n - k) times itself to b, and the old a adds n times to b. psadbw sums
// each step's bytes into a; b gets 32 times the sum of a's lanes before
// each step, plus each position's bytes, summed in 16-bit lanes, times
// its weight, 32 down to 1, by pmaddwd. Those are signed, so a block has
// at most 128 steps (128*255 < 32768), well within ADLER_NMAX.
#include <emmintrin.h>
static u32 adler32_update(u32 adler, u8 const *p, iz len)
{
    u32 a = adler & 0xffff;
    u32 b = adler >> 16;
    __m128i const zero = _mm_setzero_si128();
    __m128i const w1 = _mm_setr_epi16(32, 31, 30, 29, 28, 27, 26, 25);
    __m128i const w2 = _mm_setr_epi16(24, 23, 22, 21, 20, 19, 18, 17);
    __m128i const w3 = _mm_setr_epi16(16, 15, 14, 13, 12, 11, 10,  9);
    __m128i const w4 = _mm_setr_epi16( 8,  7,  6,  5,  4,  3,  2,  1);
    while (len >= 32) {
        iz steps = MIN(len/32, 128);
        b += a * (u32)(steps*32);
        __m128i s1 = zero, s2 = zero;
        __m128i c1 = zero, c2 = zero, c3 = zero, c4 = zero;
        for (iz i = 0; i < steps; i++, p += 32) {
            __m128i x = _mm_loadu_si128((__m128i const *)p);
            __m128i y = _mm_loadu_si128((__m128i const *)(p + 16));
            s2 = _mm_add_epi32(s2, s1);
            s1 = _mm_add_epi32(s1, _mm_add_epi32(_mm_sad_epu8(x, zero),
                                                 _mm_sad_epu8(y, zero)));
            c1 = _mm_add_epi16(c1, _mm_unpacklo_epi8(x, zero));
            c2 = _mm_add_epi16(c2, _mm_unpackhi_epi8(x, zero));
            c3 = _mm_add_epi16(c3, _mm_unpacklo_epi8(y, zero));
            c4 = _mm_add_epi16(c4, _mm_unpackhi_epi8(y, zero));
        }
        len -= steps*32;
        s2 = _mm_slli_epi32(s2, 5);
        s2 = _mm_add_epi32(s2, _mm_add_epi32(_mm_madd_epi16(c1, w1),
                                             _mm_madd_epi16(c2, w2)));
        s2 = _mm_add_epi32(s2, _mm_add_epi32(_mm_madd_epi16(c3, w3),
                                             _mm_madd_epi16(c4, w4)));
        s1 = _mm_add_epi32(s1, _mm_shuffle_epi32(s1, 0x4e));
        s1 = _mm_add_epi32(s1, _mm_shuffle_epi32(s1, 0xb1));
        s2 = _mm_add_epi32(s2, _mm_shuffle_epi32(s2, 0x4e));
        s2 = _mm_add_epi32(s2, _mm_shuffle_epi32(s2, 0xb1));
        a += (u32)_mm_cvtsi128_si32(s1);
        b += (u32)_mm_cvtsi128_si32(s2);
        a %= ADLER_MOD;
        b %= ADLER_MOD;
    }
    return adler32_scalar(a | b<<16, p, len);
}
#else
static u32 adler32_update(u32 adler, u8 const *p, iz len)
{
    return adler32_scalar(adler, p, len);
}
#endif
