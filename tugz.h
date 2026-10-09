// tugz: streaming DEFLATE (RFC 1951), zlib (RFC 1950), and gzip (RFC 1952)
//
// The caller owns all memory and all I/O. Query the state size, provide
// that much memory at any alignment, then pass input and output buffers
// of any size, as many times as needed:
//
//   ptrdiff_t      len = tugz_inflate_size(TUGZ_GZIP);
//   void          *mem = malloc(len);
//   tugz_inflator *z   = tugz_inflate_init(mem, len, TUGZ_GZIP);
//   tugz_buf       b   = {in, inlen, out, outlen};
//   int status = tugz_inflate(z, &b);
//   ...
//   free(mem);
//
// Each call advances b.in/b.out and decreases b.inlen/b.outlen by what it
// consumed and produced, failing calls included. A pointer may be null
// when its length is zero. The input and output must not overlap. A null
// state or tugz_buf, a negative length, or a null pointer with a positive
// length gets TUGZ_EUSAGE.
//
// Statuses: TUGZ_DONE is zero. A positive status asks for a call again
// with what it names, more input or more output space. A negative status
// is an error. Errors are sticky: once returned, later calls return the
// same error, consuming and producing nothing, until a reset (or init).
// TUGZ_EUSAGE alone is not sticky: it refuses that call, changing
// nothing. Releases 1.x may add statuses of either sign, so test the sign
// rather than switch over the values here: a new positive status will
// still ask for another call, and a new negative one will be an error.
// tugz_strerror describes any of them.
//
// Init places the state inside mem, aligned, so the state it returns
// need not be mem: the caller keeps mem to release it, and must never
// free the state itself. Init returns null for null mem, and memory
// beyond the size goes unused. A state holds pointers into its own
// memory, so it works only where init placed it and may not be copied
// or moved (as by memcpy or realloc). Init on the same memory again
// starts over, and the memory is free for other uses once the state is
// no longer used. The state is fixed in size and never grows, so it
// needs no cleanup beyond releasing its memory.
//
// A state's size is the same for every format and level, but for
// TUGZ_RAW64: currently about 301 KiB to inflate (398 KiB for TUGZ_RAW64)
// and 2.6 MiB to deflate. Sizes may change between releases, and there is
// no compile-time constant for them: always ask tugz_inflate_size or
// tugz_deflate_size. The library has no global or lazily initialized
// state, so every function may be called from any thread at any time,
// and separate states may be used concurrently. One state may be used by
// only one thread at a time.
//
// A reset starts another stream in the same state and format, and may
// come at any point in a stream. To compress many streams, reset rather
// than init: a deflate init clears 512 KiB and builds tables, while a
// reset usually takes time in proportion to the previous stream's first
// 1 KiB of input, making deflate about 4x faster for 100-byte streams.
// Inflate init and reset both take a small constant time. (Timings here
// are informative, not promises.)
//
// Inflate returns:
//
//   TUGZ_NEED_INPUT   all input consumed: supply more, or if there is no
//                     more, the stream is truncated
//   TUGZ_NEED_OUTPUT  the output buffer is full and decoded output
//                     remains: call again with more space (and any input
//                     left)
//   TUGZ_DONE         the end of the stream (for gzip, of each member),
//                     with b.in pointing just past its last byte
//
// or an error. So it consumes all input unless the output buffer fills,
// the stream (or member) ends, or an error stops it. Any result but
// TUGZ_NEED_OUTPUT means that all output decoded so far has been
// delivered, and an error is returned only once the output before it
// has been. A raw or zlib state stays done: later calls return TUGZ_DONE
// and consume nothing, leaving any data after the stream in b.in for the
// caller. Calling again on a gzip state with input decodes the next
// member, and bytes that do not begin a member produce TUGZ_ENOTGZ, while
// a call with no input returns TUGZ_DONE again. So at the end of input,
// TUGZ_DONE always means a complete stream (or whole gzip members) and
// TUGZ_NEED_INPUT a truncated one, with no need to track where gzip
// members end.
//
// Inflate errors:
//
//   TUGZ_EDATA    invalid compressed data
//   TUGZ_ENOTGZ   gzip only: a member does not begin 1f 8b (or, as in GNU
//                 gzip, though not zlib, the old magic 1f 9e), including
//                 bytes after a member, such as zero padding
//   TUGZ_EHEADER  gzip: a method (CM) other than 8, reserved flag bits
//                 set, or a header CRC (FHCRC) mismatch; zlib: a bad
//                 header check (FCHECK), a method other than 8, a window
//                 over 32 KiB, or a preset dictionary (FDICT, which is
//                 unsupported), reported once its ID has been read, where
//                 zlib would ask for the dictionary
//   TUGZ_ECHECK   CRC-32 (gzip) or Adler-32 (zlib) mismatch
//   TUGZ_ELENGTH  gzip length (ISIZE) mismatch
//   TUGZ_EUSAGE   an invalid argument, as above
//
// With TUGZ_ECHECK and TUGZ_ELENGTH, the stream's output has already been
// delivered and must not be trusted. Where b.in stops in a failing call
// is otherwise unspecified, so to find data after a stream, look at b.in
// after TUGZ_DONE. tugz_inflate_reset clears any error, along with all
// stream and gzip member state.
//
// Deflate takes a flush mode with each call:
//
//   TUGZ_NONE    compress, with output perhaps staged internally
//   TUGZ_SYNC    also emit all output so far, byte-aligned, ending in an
//                empty stored block (00 00 FF FF)
//   TUGZ_FULL    as SYNC, and forget history, so decoding can restart
//                there
//   TUGZ_FINISH  end the stream
//
// A SYNC, FULL, or FINISH falls due once its call has consumed all
// input, and is complete once all its output has been delivered. A call
// does the first of these that applies, by its mode, whether it has
// input, and whether due flushes are incomplete:
//
//   mode                input  incomplete  the call
//   any but FINISH      any    -           after a FINISH falls due:
//   FINISH              yes    -             returns TUGZ_EUSAGE
//   FINISH              none   -             completes all: TUGZ_DONE
//   latest due's mode   none   yes         completes them, returning
//                                          TUGZ_DONE (a repeat)
//   SYNC, FULL, FINISH  none   yes         falls due behind them, and
//                                          completes all: TUGZ_DONE
//   any                 any    yes         completes them, consuming no
//                                          input, then goes on below
//   NONE                any    no          consumes all input, returning
//                                          TUGZ_NEED_INPUT
//   SYNC, FULL, FINISH  any    no          consumes all input, falls due,
//                                          and completes: TUGZ_DONE
//
// Any call returns TUGZ_NEED_OUTPUT when the output buffer is full and it
// cannot go on: call again with more space (and any input left). A call
// returning TUGZ_DONE has completed its flush and every flush before it.
// The caller need not wait for TUGZ_DONE to move on, as to more input,
// another flush, or TUGZ_FINISH at the end of input: the stream is the
// one it would get by waiting for each flush, whatever the buffer sizes.
// Only a repeat does not flush again, so a no-input SYNC right after a
// completed SYNC adds another empty stored block. TUGZ_NONE never
// returns TUGZ_DONE, and consumes input even with no output space until
// its internal staging, which is bounded, fills.
// Once a FINISH falls due, the stream is over until a reset. TUGZ_EUSAGE,
// deflate's only error, changes nothing: a misused call, as after a
// FINISH, does not spoil the stream.
//
// Compression levels are 1 (fastest) through 9 (smallest). Others are
// rejected, and reserved, as 0 and those above 9 may gain meanings: init
// and new return null, and reset returns TUGZ_EUSAGE and leaves the
// state as it was. The gzip header records no name or time, and its XFL
// marks levels 1 and 9 as GNU gzip and zlib mark them.
//
// Define TUGZ_API (e.g. as static) to control the linkage of definitions.
// The single-file tugz.c (in each release, or from cmake -P
// cmake/amalgamate.cmake) may so be embedded in a program's translation
// unit. Its macros are its own: any of the program's with the same names
// (assert, MIN, ...) are saved before it and restored after, and its
// type names are renamed (u8 to tugz__u8, and so on).
// Its other internal names are not: static functions such as alloc and
// enumerators such as GZ_OK. Should those collide with the program's,
// compile tugz.c on its own instead. A shared Windows build (CMake with
// BUILD_SHARED_LIBS) exports the functions by a module-definition file,
// and they are functions only, so programs need not define TUGZ_API as
// __declspec(dllimport): with MinGW and MSVC-ABI toolchains alike, calls
// link through the import library's stubs.
#ifndef TUGZ_H
#define TUGZ_H

// The release this header belongs to, as a string and as numbers for
// #if. It is the one place the version is set: the programs report it,
// and the build, the amalgamations, and the release workflow read it.
#define TUGZ_VERSION "0.4.0"
#define TUGZ_VERSION_MAJOR 0
#define TUGZ_VERSION_MINOR 4
#define TUGZ_VERSION_PATCH 0

#include <stddef.h>
#include <stdint.h>

#ifndef TUGZ_API
#  define TUGZ_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Deflate64 (PKWARE's "Enhanced Deflating", ZIP method 9: a 64 KiB
// window, matches up to 65538 bytes) inflates as format TUGZ_RAW64 when
// the library is compiled with TUGZ_DEFLATE64 defined (CMake option
// TUGZ_DEFLATE64), with a state about 97 KiB larger. It is raw only, as
// zlib and gzip never carry it, and decode only: deflate rejects it as an
// invalid format, and tugz_deflate_size(TUGZ_RAW64) is zero. TUGZ_RAW64
// is declared in every build, and a library without Deflate64 rejects it
// too, so tugz_inflate_size(TUGZ_RAW64) tells whether it is available.
// The option changes nothing else, such as other formats' sizes. RAW
// decodes no Deflate64-only codes: distance codes 30 and 31 are
// TUGZ_EDATA, and length code 285 is 258.
enum {  // formats
    TUGZ_RAW,
    TUGZ_ZLIB,
    TUGZ_GZIP,
    TUGZ_RAW64,  // inflate only, with TUGZ_DEFLATE64 (see above)
};

enum {  // deflate flush modes
    TUGZ_NONE,
    TUGZ_SYNC,
    TUGZ_FULL,
    TUGZ_FINISH,
};

enum {  // results (see above)
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

// For new and free: called with ptr null, oldsize zero, and newsize the
// state's size to allocate memory, which need not be aligned, returning
// null on failure; and with ptr and oldsize from that allocation and
// newsize zero to free it, its result then ignored. Never to resize.
typedef void *tugz_allocator(void *ctx, void *ptr, ptrdiff_t oldsize,
                             ptrdiff_t newsize);

// Size of the state for a format, or zero if the format is invalid (as
// TUGZ_RAW64 is without TUGZ_DEFLATE64).
TUGZ_API ptrdiff_t      tugz_inflate_size(int format);
// Returns null if the memory is null or too small, or the format is
// invalid.
TUGZ_API tugz_inflator *tugz_inflate_init(void *mem, ptrdiff_t len,
                                          int format);
TUGZ_API int            tugz_inflate(tugz_inflator *, tugz_buf *);
// Discard the current stream, and any error, and start another in the
// same format. A null state is ignored.
TUGZ_API void           tugz_inflate_reset(tugz_inflator *);

// Size of the state for a format, or zero if deflate does not take it.
TUGZ_API ptrdiff_t      tugz_deflate_size(int format);
// Most bytes deflate produces from len bytes of input in one stream in
// the format, at any level, when TUGZ_FINISH is the only flush: with an
// output buffer this large, one TUGZ_FINISH call given all the input
// returns TUGZ_DONE. Each SYNC or FULL flush may add 16 bytes more. The
// bound is len plus at most 0.12%, plus 17 bytes and the format's header
// and trailer (6 bytes for zlib, 18 for gzip). Returns zero for a format
// deflate does not take, a negative len, or a bound past PTRDIFF_MAX.
TUGZ_API ptrdiff_t      tugz_deflate_bound(int format, ptrdiff_t len);
// Returns null if the memory is null or too small, or the format or
// level is invalid.
TUGZ_API tugz_deflator *tugz_deflate_init(void *mem, ptrdiff_t len,
                                          int format, int level);
TUGZ_API int            tugz_deflate(tugz_deflator *, tugz_buf *,
                                     int flush);
// Discard the current stream and start another in the same format at a
// level. Returns TUGZ_DONE, or TUGZ_EUSAGE, changing nothing, for an
// invalid level or a null state.
TUGZ_API int            tugz_deflate_reset(tugz_deflator *, int level);

// Convenience: one allocation of the state's size through the allocator,
// which must not be null, and free releases it, given the state new
// returned (not one from init); free with a null state does nothing. New
// returns null if the allocator does, or, allocating nothing, if the
// format (or level) is invalid.
TUGZ_API tugz_inflator *tugz_inflate_new(tugz_allocator *, void *ctx,
                                         int format);
TUGZ_API void           tugz_inflate_free(tugz_inflator *,
                                          tugz_allocator *, void *ctx);
TUGZ_API tugz_deflator *tugz_deflate_new(tugz_allocator *, void *ctx,
                                         int format, int level);
TUGZ_API void           tugz_deflate_free(tugz_deflator *,
                                          tugz_allocator *, void *ctx);

// CRC-32 (as gzip and ZIP use it: zlib's crc32) and Adler-32 (as the
// zlib format uses it: zlib's adler32) of len bytes at p, continued from
// an earlier result: start with 0 for CRC-32, or 1 for Adler-32, the
// checks of no data, and pass each result to the next call. A len of
// zero or less returns the check unchanged, and p may then be null.
// Neither keeps any state. CRC-32 uses the CPU's CRC instructions where
// the target has them (ARMv8), or carry-less multiplication (x86 with
// -mpclmul). Otherwise, on x86, it asks the CPU for the latter on each
// call of 4 KiB or more, as asking can take a microsecond under a
// hypervisor: larger pieces are faster.
TUGZ_API uint32_t       tugz_crc32(uint32_t crc, void const *p,
                                   ptrdiff_t len);
TUGZ_API uint32_t       tugz_adler32(uint32_t adler, void const *p,
                                     ptrdiff_t len);

// A short, constant message for a status, such as "invalid compressed
// data": one for each of the results above, those that are not errors
// included, and "unknown status" for any other value.
TUGZ_API char const    *tugz_strerror(int status);

// The library's TUGZ_VERSION, as it was built, which for a shared
// library may differ from the header's.
TUGZ_API char const    *tugz_version(void);

#ifdef __cplusplus
}
#endif

#endif
