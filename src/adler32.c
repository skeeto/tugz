// tugz core: Adler-32 checksum (RFC 1950)

#define ADLER_MOD   65521
#define ADLER_NMAX  5552  // most bytes before the sums may overflow 32 bits

static u32 adler32_update(u32 adler, u8 const *p, iz len)
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
