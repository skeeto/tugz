// tugz application I/O: platform interface, buffered reader and writer
//
// Only programs include this file. The library layer does not, so the
// codec itself never touches the os_* functions declared here. Each
// program declares its own additional platform functions (gzipio.c,
// zipcli.c), and may leave some of these helpers unused.

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
// Keep a created file when it is closed instead of discarding it.
static void os_keep(os *, i32 fd);
// Exit with a status. A created file not yet kept is discarded.
[[noreturn]] static void os_exit(os *, i32 status);

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

// Returns the next byte, or -1 at end of input.
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

static b32 s8equals(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !__builtin_memcmp(a.s, b.s, (uz)a.len));
}

// Carve out exactly the memory a codec claims to need, as the library
// does, so that every run checks the claim.
static arena subarena(arena *a, iz size)
{
    arena r = {0};
    r.beg = (byte *)newbytes(a, size);
    r.end = r.beg + size;
    r.ctx = a->ctx;
    return r;
}

// Hand pending output to a descriptor, if any, noting the first failure.
static void put_pending(os *ctx, i32 fd, s8 p, b32 *err)
{
    if (p.len && fd>=0 && !*err) {
        *err = !os_write(ctx, fd, p.s, p.len);
    }
}
