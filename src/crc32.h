#ifndef GZ_CRC32_H
#define GZ_CRC32_H

#include "common.h"

void crc32_init(void);
u32 crc32_update(u32 state, const u8 *p, iz n);

#endif