// tugz core: base types and arena allocator
//
// Everything is a unity build. Each program's entry file
// (platform/*_posix.c, platform/*_windows.c, platform/libtugz.c, and the
// test programs) includes the sources it needs, this one first. The
// hooks os_oom and os_extend come from the program's own layer
// (src/gzipio.c, src/zipcli.c) or from its entry file.

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef   signed int        i32;
typedef unsigned int        u32;
typedef   signed long long  i64;
typedef unsigned long long  u64;
typedef   signed int        b32;
typedef __PTRDIFF_TYPE__    iz;
typedef __SIZE_TYPE__       uz;
typedef __UINTPTR_TYPE__    uptr;
typedef char                byte;

#define assert(c)       while (!(c)) __builtin_unreachable()
#define countof(a)      (iz)(sizeof(a) / sizeof(*(a)))
#define new(a, n, t)    (t *)alloc(a, n, sizeof(t), _Alignof(t), 1)
#define newbytes(a, n)  (u8 *)alloc(a, n, 1, 64, 0)  // buffers
#define newstr(a, n)    (u8 *)alloc(a, n, 1, 1, 0)   // strings, packed
#define S8(s)           {(u8 *)s, countof(s)-1}
#define S(s)            (s8)S8(s)
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

typedef struct os os;

typedef struct {
    u8 *s;
    iz  len;
} s8;

// Allocates from beg up, or if down, from end down, so that a scratch
// arena may grow toward a permanent one in the same memory.
typedef struct {
    byte *beg;
    byte *end;
    os   *ctx;
    b32   down;
} arena;

// Status codes for compression and decompression.
enum {
    GZ_OK,
    GZ_TRAILING,  // warning: decompressed fine, but junk followed
    GZ_ENOTGZ,
    GZ_EMETHOD,
    GZ_EFLAGS,
    GZ_EHCRC,
    GZ_ETRUNC,
    GZ_EDATA,
    GZ_ECRC,
    GZ_ELEN,
    GZ_EREAD,
    GZ_EWRITE,
    GZ_EHEADER,   // invalid zlib header
    GZ_EUSAGE,    // streaming: input or flush after finishing
    GZ_NEEDIN,    // streaming: all input consumed
    GZ_NEEDOUT,   // streaming: output buffer full
};

// Streaming buffers, advanced as input is consumed and output produced.
typedef struct {
    u8 const *in;
    iz        inlen;
    u8       *out;
    iz        outlen;
} zbuf;

// Null pointers are permitted with zero lengths, as library callers may
// legitimately pass empty buffers that way.
static void *bytecopy(void *dst, void const *src, iz len)
{
    assert(len >= 0);
    return len ? __builtin_memcpy(dst, src, (uz)len) : dst;
}

static void *bytemove(void *dst, void const *src, iz len)
{
    assert(len >= 0);
    return len ? __builtin_memmove(dst, src, (uz)len) : dst;
}

static void *bytefill(void *dst, i32 c, iz len)
{
    assert(len >= 0);
    return __builtin_memset(dst, c, (uz)len);
}

// Explicit little-endian accesses; each compiles to a single unaligned
// load or store on little-endian targets.
static u64 load64le(u8 const *p)
{
    return (u64)p[0]     | (u64)p[1]<< 8 | (u64)p[2]<<16 | (u64)p[3]<<24 |
           (u64)p[4]<<32 | (u64)p[5]<<40 | (u64)p[6]<<48 | (u64)p[7]<<56;
}

static void store64le(u8 *p, u64 v)
{
    for (i32 i = 0; i < 8; i++) {
        p[i] = (u8)(v >> (8*i));
    }
}

// Called when memory runs out.
[[noreturn]] static void os_oom(os *);

// Called when an arena lacks room for need bytes, padding included.
// Makes room, as by committing more memory, while keeping the end from
// which the arena allocates, or exits through os_oom.
static void os_extend(os *, arena *, iz need);

// Padding to align an allocation of len bytes, which must fit unpadded.
static iz alloc_pad(arena *a, iz len, iz align)
{
    uptr p = a->down ? (uptr)(a->end - len) : -(uptr)a->beg;
    return (iz)(p & (uptr)(align - 1));
}

static void *alloc(arena *a, iz count, iz size, iz align, b32 zero)
{
    assert(count >= 0);
    assert(size > 0);
    iz avail = a->end - a->beg;
    if (count>avail/size || alloc_pad(a, count*size, align)>avail-count*size) {
        if (count > ((iz)((uz)-1 >> 1) - align)/size) {
            os_oom(a->ctx);
        }
        os_extend(a->ctx, a, count*size + align-1);  // room for any padding
    }
    iz    len = count * size;
    iz    pad = alloc_pad(a, len, align);
    byte *r   = a->down ? a->end - len - pad : a->beg + pad;
    if (a->down) {
        a->end = r;
    } else {
        a->beg = r + len;
    }
    return zero ? bytefill(r, 0, len) : r;
}
