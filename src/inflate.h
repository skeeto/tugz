#ifndef GZ_INFLATE_H
#define GZ_INFLATE_H

#include "common.h"
#include "win32.h"

typedef struct inflate_state inflate_state;

inflate_state *inflate_create(reader *in, writer *out);
b32 inflate_stream(inflate_state *s);
b32 inflate_finish(inflate_state *s);
i32 inflate_read_byte(inflate_state *s);
u32 inflate_crc(inflate_state *s);
u64 inflate_total(inflate_state *s);

#endif