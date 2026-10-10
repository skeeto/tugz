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
    OS_NOSETID  = 1 << 3,  // refuse set-user-ID and set-group-ID (POSIX)
    OS_NOSTICKY = 1 << 4,  // refuse files with the sticky bit (POSIX)

    // Create a file for writing. On POSIX it is created owner-only, for
    // the program to give it permissions later (gzip's os_setmeta, zip's
    // os_commit), or with the defaults for a new file (umask, inherited
    // ACL) given OS_DEFPERMS; on Windows it is opened exclusively and
    // inherits access control either way. It is discarded when closed,
    // or if the process is interrupted, unless first kept (os_keep) or
    // committed (zip's os_commit).
    OS_CREATE   = 1 << 5,  // fail if it exists
    OS_FORCE    = 1 << 6,  // replace if it exists, OS_ERR if it cannot
    OS_DEFPERMS = 1 << 7,
};
enum {
    OS_ERR      = -1,
    OS_EEXIST   = -2,
    OS_EISDIR   = -3,
    OS_ESYMLINK = -4,
    OS_ENOTREG  = -5,
    OS_ELINKS   = -6,
    OS_ESETUID  = -7,
    OS_ESETGID  = -8,
    OS_ESTICKY  = -9,
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
// Returns false if it cannot be kept (on Windows, where the file system
// refuses to cancel its deletion), and closing it still discards it.
static b32  os_keep(os *, i32 fd);
// Exit with a status. A created file not yet kept is discarded.
[[noreturn]] static void os_exit(os *, i32 status);
// Milliseconds on a monotonic clock, from an arbitrary start, as coarse
// as a system timer's tick. It may wrap (Windows, after 49 days).
static i64  os_now(os *);

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

[[maybe_unused]] static reader *newreader(arena *a, i32 fd, iz cap)
{
    reader *r = new(a, 1, reader);
    r->ctx = a->ctx;
    r->buf = newbytes(a, cap);
    r->cap = cap;
    r->fd  = fd;
    return r;
}

// Ensure at least one byte is buffered. Returns false at end of input.
[[maybe_unused]] static b32 reader_fill(reader *r)
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

// A writer with a negative descriptor discards output. One to a
// terminal (tty), which a program sets for its messages, never its data,
// notes when its buffer last began to fill, for writer_poll.
typedef struct {
    os *ctx;
    u8 *buf;
    iz  len;
    iz  cap;
    i32 fd;
    b32 err;
    b32 tty;
    i64 since;  // when the buffered text began to wait
} writer;

enum { WRITER_WAIT = 100 };  // milliseconds, writer_poll's

[[maybe_unused]] static writer *newwriter(arena *a, i32 fd, iz cap)
{
    writer *w = new(a, 1, writer);
    w->ctx = a->ctx;
    w->buf = newbytes(a, cap);
    w->cap = cap;
    w->fd  = fd;
    return w;
}

// The buffer is emptied before it is written, so that a signal handler
// that writes what is buffered (zip's, on POSIX) never writes it twice.
[[maybe_unused]] static b32 writer_flush(writer *w)
{
    iz len = w->len;
    w->len = 0;
    if (!w->err && len && w->fd>=0) {
        w->err = !os_write(w->ctx, w->fd, w->buf, len);
    }
    return !w->err;
}

[[maybe_unused]] static void writer_write(writer *w, u8 const *p, iz len)
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
        if (!w->len && w->tty) {
            w->since = os_now(w->ctx);
        }
        iz take = MIN(len, w->cap-w->len);
        bytecopy(w->buf+w->len, p, take);
        w->len += take;
        p += take;
        len -= take;
    }
}

[[maybe_unused]] static void writer_s8(writer *w, s8 s)
{
    writer_write(w, s.s, s.len);
}

[[maybe_unused]] static void writer_byte(writer *w, u8 b)
{
    if (w->len == w->cap) {
        writer_flush(w);
    }
    if (!w->len && w->tty) {
        w->since = os_now(w->ctx);
    }
    w->buf[w->len++] = b;
}

// A check point, called between pieces of a program's work: text to a
// terminal is flushed once it has waited WRITER_WAIT, as Nagle's
// algorithm holds small packets. Output is otherwise flushed only when
// full, or by the program before a message to standard error, or a
// prompt, and at the end. A line then shows within WRITER_WAIT and a
// piece of work, a flood of lines is written ten times a second, and a
// fast run is written at once, at the end. The clock is read only with
// text waiting, so polling costs nothing for a pipe or file.
[[maybe_unused]] static void writer_poll(writer *w)
{
    if (w->tty && w->len) {
        i64 now = os_now(w->ctx);
        if (now-w->since>=WRITER_WAIT || now<w->since) {  // (or wrapped)
            writer_flush(w);
        }
    }
}

[[maybe_unused]] static b32 s8equals(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !__builtin_memcmp(a.s, b.s, (uz)a.len));
}

// Carve out exactly the memory a codec claims to need, as the library
// does, so that every run checks the claim.
[[maybe_unused]] static arena subarena(arena *a, iz size)
{
    arena r = {0};
    r.beg = (byte *)newbytes(a, size);
    r.end = r.beg + size;
    r.ctx = a->ctx;
    return r;
}

// Hand pending output to a descriptor, if any. Returns false on failure.
[[maybe_unused]] static b32 put_pending(os *ctx, i32 fd, s8 p)
{
    return !p.len || fd<0 || os_write(ctx, fd, p.s, p.len);
}
