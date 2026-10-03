#include "crc32.h"

static u32 crc_tab[256];

void crc32_init(void)
{
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (i32 k = 0; k < 8; k++) {
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        }
        crc_tab[n] = c;
    }
}

u32 crc32_update(u32 state, const u8 *p, iz n)
{
    u32 c = state;
    for (iz i = 0; i < n; i++) {
        c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }
    return c;
}