#ifndef GZ_DEFLATE_H
#define GZ_DEFLATE_H

#include "common.h"
#include "win32.h"

typedef struct deflate deflate;

void deflate_init_tables(void);
deflate *deflate_create(i32 level, writer *out);
b32 deflate_push(deflate *d, const u8 *data, iz len);
b32 deflate_end(deflate *d);

#endif