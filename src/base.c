// tugz core: base types, arena allocator, platform interface, buffered I/O
//
// The program is a unity build. A platform layer (main_*.c) includes the
// sources it needs, starting with this one, and then defines the os_*
// functions declared below along with the program entry point.

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
#define newbytes(a, n)  (u8 *)alloc(a, n, 1, 64, 0)
#define S8(s)           {(u8 *)s, countof(s)-1}
#define S(s)            (s8)S8(s)
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

typedef struct os os;

typedef struct {
    u8 *s;
    iz  len;
} s8;

typedef struct {
    byte *beg;
    byte *end;
    os   *ctx;
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
};

// Platform interface. File descriptors 0, 1, and 2 are standard input,
// output, and error. Paths are UTF-8 (WTF-8 on Windows).
enum {
    // Open an existing file for reading. Directories are always refused.
    OS_READ     = 0,
    OS_REGULAR  = 1 << 0,  // refuse anything but a regular file
    OS_NOFOLLOW = 1 << 1,  // refuse symbolic links (and reparse points)
    OS_ONELINK  = 1 << 2,  // refuse files with multiple hard links

    // Create a file for writing. It is created inaccessible to others
    // until os_copymeta, and it is discarded when closed, or if the
    // process is interrupted, unless os_keep was called first.
    OS_CREATE   = 1 << 3,  // fail if it exists
    OS_FORCE    = 1 << 4,  // replace if it exists
};
enum {
    OS_ERR      = -1,
    OS_EEXIST   = -2,
    OS_EISDIR   = -3,
    OS_ESYMLINK = -4,
    OS_ENOTREG  = -5,
    OS_ELINKS   = -6,
};

// Returns a non-negative descriptor or a negative OS_E* code.
static i32  os_open(os *, s8 path, i32 mode, arena scratch);
// Returns false if the file could not be closed cleanly, meaning written
// data may be lost.
static b32  os_close(os *, i32 fd);
// Returns the number of bytes read, 0 at end of file, or -1 on error.
static iz   os_read(os *, i32 fd, u8 *buf, iz cap);
// Writes all bytes or returns false.
static b32  os_write(os *, i32 fd, u8 *buf, iz len);
static b32  os_remove(os *, s8 path, arena scratch);
static b32  os_isatty(os *, i32 fd);
// Keep a created file when it is closed instead of discarding it.
static void os_keep(os *, i32 fd);
// Best effort: give an open output file the input file's permissions,
// ownership, and timestamps, as far as the platform supports.
static void os_copymeta(os *, i32 from, i32 to);
static void os_fail(os *) __attribute((noreturn));

static void *bytecopy(void *dst, void const *src, iz len)
{
    assert(len >= 0);
    return __builtin_memcpy(dst, src, (uz)len);
}

static void *bytemove(void *dst, void const *src, iz len)
{
    assert(len >= 0);
    return __builtin_memmove(dst, src, (uz)len);
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

static b32 s8equals(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !__builtin_memcmp(a.s, b.s, (uz)a.len));
}

static void oom(os *ctx)
{
    s8 msg = S("gzip: out of memory\n");
    os_write(ctx, 2, msg.s, msg.len);
    os_fail(ctx);
}

static void *alloc(arena *a, iz count, iz size, iz align, b32 zero)
{
    assert(count >= 0);
    assert(size > 0);
    iz pad = (iz)(-(uptr)a->beg & (uptr)(align - 1));
    iz avail = a->end - a->beg - pad;
    if (avail < 0 || count > avail/size) {
        oom(a->ctx);
    }
    void *r = a->beg + pad;
    a->beg += pad + count*size;
    return zero ? bytefill(r, 0, count*size) : r;
}

typedef struct {
    os *ctx;
    u8 *buf;
    iz  len;
    iz  off;
    iz  cap;
    i32 fd;
    b32 eof;
    b32 err;
} reader;

static reader *newreader(arena *a, i32 fd, iz cap)
{
    reader *r = new(a, 1, reader);
    r->ctx = a->ctx;
    r->buf = newbytes(a, cap);
    r->cap = cap;
    r->fd  = fd;
    return r;
}

// Ensure at least one byte is buffered. Returns false at end of input.
static b32 reader_fill(reader *r)
{
    if (r->off < r->len) {
        return 1;
    }
    r->off = r->len = 0;
    if (r->eof) {
        return 0;
    }
    iz n = os_read(r->ctx, r->fd, r->buf, r->cap);
    if (n <= 0) {
        r->eof = 1;
        r->err = n < 0;
        return 0;
    }
    r->len = n;
    return 1;
}

static iz reader_read(reader *r, u8 *dst, iz len)
{
    iz got = 0;
    while (got<len && reader_fill(r)) {
        iz take = MIN(len-got, r->len-r->off);
        bytecopy(dst+got, r->buf+r->off, take);
        r->off += take;
        got += take;
    }
    return got;
}

// Returns the next byte, or -1 at end of input.
static i32 reader_byte(reader *r)
{
    if (!reader_fill(r)) {
        return -1;
    }
    return r->buf[r->off++];
}

// A writer with a negative descriptor discards output.
typedef struct {
    os *ctx;
    u8 *buf;
    iz  len;
    iz  cap;
    i32 fd;
    b32 err;
} writer;

static writer *newwriter(arena *a, i32 fd, iz cap)
{
    writer *w = new(a, 1, writer);
    w->ctx = a->ctx;
    w->buf = newbytes(a, cap);
    w->cap = cap;
    w->fd  = fd;
    return w;
}

static b32 writer_flush(writer *w)
{
    if (!w->err && w->len && w->fd>=0) {
        w->err = !os_write(w->ctx, w->fd, w->buf, w->len);
    }
    w->len = 0;
    return !w->err;
}

static void writer_write(writer *w, u8 const *p, iz len)
{
    if (w->fd < 0) {
        return;
    } else if (len > w->cap-w->len) {
        writer_flush(w);
        if (len >= w->cap) {
            // Large writes bypass the buffer
            if (!w->err) {
                w->err = !os_write(w->ctx, w->fd, (u8 *)p, len);
            }
            return;
        }
    }
    while (len) {
        if (w->len == w->cap) {
            writer_flush(w);
        }
        iz take = MIN(len, w->cap-w->len);
        bytecopy(w->buf+w->len, p, take);
        w->len += take;
        p += take;
        len -= take;
    }
}

static void writer_s8(writer *w, s8 s)
{
    writer_write(w, s.s, s.len);
}

static void writer_byte(writer *w, u8 b)
{
    if (w->len == w->cap) {
        writer_flush(w);
    }
    w->buf[w->len++] = b;
}
