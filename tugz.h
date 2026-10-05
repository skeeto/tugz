// tugz: streaming DEFLATE (RFC 1951), zlib (RFC 1950), and gzip (RFC 1952)
//
// The caller owns all memory and all I/O. Query the state size, provide
// that much memory at any alignment, then pass input and output buffers
// of any size, as many times as needed:
//
//   ptrdiff_t      len = tugz_inflate_size(TUGZ_GZIP);
//   tugz_inflator *z   = tugz_inflate_init(malloc(len), len, TUGZ_GZIP);
//   tugz_buf       b   = {in, inlen, out, outlen};
//   int status = tugz_inflate(z, &b);
//
// Each call advances b.in/b.out and decreases b.inlen/b.outlen by what it
// consumed and produced. The state is fixed in size and never grows, so
// it needs no cleanup beyond releasing its memory. States are
// independent, so separate states may be used concurrently from
// different threads.
//
// To compress many streams, reset one state rather than init it again:
// init clears about 512 KiB and builds tables, while a reset takes time
// in proportion to the previous stream's input, clearing the 512 KiB
// only past about 8 KiB of it. For 100-byte streams this makes deflate
// about 4x faster. Inflate init and reset both take a small constant
// time. A reset may come at any point in a stream and keeps the format.
// The deflate reset sets the level, so a deflate state's size depends
// only on its format.
//
// Inflate returns TUGZ_DONE at the end of the stream (for gzip, the end
// of each member), with b.in pointing just past its last byte. Calling
// again on a gzip state decodes the next member, and bytes that do not
// begin a member produce TUGZ_ENOTGZ. TUGZ_NEED_INPUT means all input was
// consumed: supply more, or if there is no more, the stream is truncated.
// TUGZ_NEED_OUTPUT means the output buffer is full. Errors are sticky.
//
// Deflate with TUGZ_NONE consumes all input, returning TUGZ_NEED_INPUT;
// output may remain staged internally until the output buffer has room.
// With TUGZ_SYNC (byte-align and emit everything so far), TUGZ_FULL (also
// forget history, so decoding can restart there), or TUGZ_FINISH, call
// until it returns TUGZ_DONE, supplying output space whenever it returns
// TUGZ_NEED_OUTPUT. After finishing, only TUGZ_FINISH with no input is
// accepted, and it returns TUGZ_DONE.
//
// Compression levels are 1 (fastest) through 9 (smallest), and others are
// clamped into that range. The gzip header records no name or time.
//
// Define TUGZ_API (e.g. as static) to control the linkage of definitions.
#ifndef TUGZ_H
#define TUGZ_H

#include <stddef.h>

#ifndef TUGZ_API
#  define TUGZ_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {  // formats
    TUGZ_RAW,
    TUGZ_ZLIB,
    TUGZ_GZIP,
};

enum {  // deflate flush modes
    TUGZ_NONE,
    TUGZ_SYNC,
    TUGZ_FULL,
    TUGZ_FINISH,
};

enum {  // results
    TUGZ_DONE        =  0,
    TUGZ_NEED_INPUT  =  1,
    TUGZ_NEED_OUTPUT =  2,
    TUGZ_EDATA       = -1,  // invalid compressed data
    TUGZ_ENOTGZ      = -2,  // input is not gzip
    TUGZ_EHEADER     = -3,  // invalid or unsupported header
    TUGZ_ECHECK      = -4,  // checksum mismatch
    TUGZ_ELENGTH     = -5,  // gzip length mismatch
    TUGZ_EUSAGE      = -6,  // invalid argument or call sequence
};

typedef struct tugz_inflator tugz_inflator;
typedef struct tugz_deflator tugz_deflator;

typedef struct {
    unsigned char const *in;
    ptrdiff_t            inlen;
    unsigned char       *out;
    ptrdiff_t            outlen;
} tugz_buf;

// Allocate (ptr null, oldsize zero) or free (newsize zero), Lua-style.
typedef void *tugz_allocator(void *ctx, void *ptr, ptrdiff_t oldsize,
                             ptrdiff_t newsize);

// Size of the state for a format, or zero if the format is invalid.
TUGZ_API ptrdiff_t      tugz_inflate_size(int format);
// Returns null if the memory is too small or the format is invalid.
TUGZ_API tugz_inflator *tugz_inflate_init(void *mem, ptrdiff_t len,
                                          int format);
TUGZ_API int            tugz_inflate(tugz_inflator *, tugz_buf *);
// Discard the current stream and start another in the same format.
TUGZ_API void           tugz_inflate_reset(tugz_inflator *);

TUGZ_API ptrdiff_t      tugz_deflate_size(int format);
TUGZ_API tugz_deflator *tugz_deflate_init(void *mem, ptrdiff_t len,
                                          int format, int level);
TUGZ_API int            tugz_deflate(tugz_deflator *, tugz_buf *,
                                     int flush);
// Discard the current stream and start another in the same format.
TUGZ_API void           tugz_deflate_reset(tugz_deflator *, int level);

// Convenience: one allocation of the state's size, freed with its size.
TUGZ_API tugz_inflator *tugz_inflate_new(tugz_allocator *, void *ctx,
                                         int format);
TUGZ_API void           tugz_inflate_free(tugz_inflator *,
                                          tugz_allocator *, void *ctx);
TUGZ_API tugz_deflator *tugz_deflate_new(tugz_allocator *, void *ctx,
                                         int format, int level);
TUGZ_API void           tugz_deflate_free(tugz_deflator *,
                                          tugz_allocator *, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
