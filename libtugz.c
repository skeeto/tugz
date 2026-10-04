// Library layer for tugz: the tugz.h interface over the core
// $ cc -c -O2 libtugz.c
//
// Exports only the tugz_* functions. The core allocates only from the
// caller's memory, after checking its size, so it never runs out.
#include "src/base.c"
#include "src/crc32.c"
#include "src/adler32.c"
#include "src/inflate.c"
#include "src/deflate.c"
#include "src/gzip.c"
#include "tugz.h"

static void os_oom(os *ctx)
{
    (void)ctx;
    __builtin_trap();  // unreachable: sizes are checked first
}

struct tugz_inflator {
    decoder  *z;
    void     *mem;
    ptrdiff_t len;
};

struct tugz_deflator {
    encoder  *e;
    void     *mem;
    ptrdiff_t len;
};

static b32 valid_format(int format)
{
    return format>=TUGZ_RAW && format<=TUGZ_GZIP;
}

static int tugz_status(i32 status)
{
    switch (status) {
    case GZ_OK:      return TUGZ_DONE;
    case GZ_NEEDIN:  return TUGZ_NEED_INPUT;
    case GZ_NEEDOUT: return TUGZ_NEED_OUTPUT;
    case GZ_EDATA:   return TUGZ_EDATA;
    case GZ_ENOTGZ:  return TUGZ_ENOTGZ;
    case GZ_EMETHOD:
    case GZ_EFLAGS:
    case GZ_EHCRC:
    case GZ_EHEADER: return TUGZ_EHEADER;
    case GZ_ECRC:    return TUGZ_ECHECK;
    case GZ_ELEN:    return TUGZ_ELENGTH;
    }
    return TUGZ_EUSAGE;
}

static arena mem_arena(void *mem, ptrdiff_t len)
{
    arena a = {0};
    a.beg = (byte *)mem;
    a.end = a.beg + len;
    return a;
}

static b32 valid_buf(tugz_buf *b)
{
    return b && b->inlen>=0 && b->outlen>=0 &&
           (b->in || !b->inlen) && (b->out || !b->outlen);
}

TUGZ_API ptrdiff_t tugz_inflate_size(int format)
{
    if (!valid_format(format)) {
        return 0;
    }
    return (iz)sizeof(tugz_inflator) + 64 + decoder_memsize();
}

TUGZ_API tugz_inflator *tugz_inflate_init(void *mem, ptrdiff_t len,
                                          int format)
{
    if (!mem || !valid_format(format) || len<tugz_inflate_size(format)) {
        return 0;
    }
    arena a = mem_arena(mem, len);
    tugz_inflator *s = new(&a, 1, tugz_inflator);
    s->z   = decoder_new(&a, format);
    s->mem = mem;
    s->len = len;
    return s;
}

TUGZ_API int tugz_inflate(tugz_inflator *s, tugz_buf *b)
{
    if (!s || !valid_buf(b)) {
        return TUGZ_EUSAGE;
    }
    zbuf zb = {b->in, b->inlen, b->out, b->outlen};
    i32 status = decoder_run(s->z, &zb);
    b->in     = zb.in;
    b->inlen  = zb.inlen;
    b->out    = zb.out;
    b->outlen = zb.outlen;
    return tugz_status(status);
}

TUGZ_API ptrdiff_t tugz_deflate_size(int format, int level)
{
    (void)level;
    if (!valid_format(format)) {
        return 0;
    }
    return (iz)sizeof(tugz_deflator) + 64 + encoder_memsize();
}

TUGZ_API tugz_deflator *tugz_deflate_init(void *mem, ptrdiff_t len,
                                          int format, int level)
{
    if (!mem || !valid_format(format) ||
        len<tugz_deflate_size(format, level)) {
        return 0;
    }
    arena a = mem_arena(mem, len);
    tugz_deflator *s = new(&a, 1, tugz_deflator);
    s->e   = encoder_new(&a, format, level);
    s->mem = mem;
    s->len = len;
    return s;
}

TUGZ_API int tugz_deflate(tugz_deflator *s, tugz_buf *b, int flush)
{
    if (!s || !valid_buf(b) || flush<TUGZ_NONE || flush>TUGZ_FINISH) {
        return TUGZ_EUSAGE;
    }
    zbuf zb = {b->in, b->inlen, b->out, b->outlen};
    i32 status = encoder_run(s->e, &zb, flush);
    b->in     = zb.in;
    b->inlen  = zb.inlen;
    b->out    = zb.out;
    b->outlen = zb.outlen;
    return tugz_status(status);
}

TUGZ_API tugz_inflator *tugz_inflate_new(tugz_allocator *alloc, void *ctx,
                                         int format)
{
    ptrdiff_t len = tugz_inflate_size(format);
    if (!len) {
        return 0;
    }
    void *mem = alloc(ctx, 0, 0, len);
    return mem ? tugz_inflate_init(mem, len, format) : 0;
}

TUGZ_API void tugz_inflate_free(tugz_inflator *s, tugz_allocator *alloc,
                                void *ctx)
{
    if (s) {
        alloc(ctx, s->mem, s->len, 0);
    }
}

TUGZ_API tugz_deflator *tugz_deflate_new(tugz_allocator *alloc, void *ctx,
                                         int format, int level)
{
    ptrdiff_t len = tugz_deflate_size(format, level);
    if (!len) {
        return 0;
    }
    void *mem = alloc(ctx, 0, 0, len);
    return mem ? tugz_deflate_init(mem, len, format, level) : 0;
}

TUGZ_API void tugz_deflate_free(tugz_deflator *s, tugz_allocator *alloc,
                                void *ctx)
{
    if (s) {
        alloc(ctx, s->mem, s->len, 0);
    }
}
