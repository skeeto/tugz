#include "win32.h"

#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)    CloseHandle(iptr);
W32(i32)    CreateFileW(c16 *, u32, u32, uptr, u32, u32, iptr);
W32(b32)    DeleteFileW(c16 *);
W32(void)   ExitProcess(i32) __attribute((noreturn));
W32(u32)    GetFileAttributesW(c16 *);
W32(u32)    GetLastError(void);
W32(c16 *)  GetCommandLineW(void);
W32(c16 **) CommandLineToArgvW(c16 *, i32 *);
W32(u32)    GetCurrentDirectoryW(u32, c16 *);
W32(iptr)   GetStdHandle(u32);
W32(b32)    ReadFile(iptr, void *, u32, u32 *, uptr);
W32(b32)    WriteFile(iptr, const void *, u32, u32 *, uptr);
W32(i32)    WideCharToMultiByte(u32, u32, c16 *, i32, char *, i32, const char *, i32 *);

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_ALL 7u
#define OPEN_EXISTING 3u
#define CREATE_ALWAYS 2u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define INVALID_FILE_ATTRIBUTES 0xffffffffu

#define STD_INPUT_HANDLE ((u32)-10)
#define STD_OUTPUT_HANDLE ((u32)-11)
#define STD_ERROR_HANDLE ((u32)-12)

static byte arena_mem[64 << 20];
static byte *arena_beg;
static byte *arena_end;

void arena_init(void)
{
    arena_beg = arena_mem;
    arena_end = arena_mem + sizeof(arena_mem);
}

void *arena_alloc(iz size, iz align)
{
    uz p = (uz)arena_beg;
    p = (p + (uz)align - 1) & ~((uz)align - 1);
    if (p + (uz)size > (uz)arena_end) {
        fatal("out of memory");
    }
    arena_beg = (byte *)(p + (uz)size);
    return (void *)p;
}

void *arena_zalloc(iz size, iz align)
{
    void *p = arena_alloc(size, align);
    memset(p, 0, size);
    return p;
}

arena_mark arena_save(void)
{
    arena_mark m;
    m.beg = arena_beg;
    m.end = arena_end;
    return m;
}

void arena_restore(arena_mark m)
{
    arena_beg = m.beg;
    arena_end = m.end;
}

void fatal(const char *msg)
{
    out_str(std_handle(2), "gzip: ");
    out_str(std_handle(2), msg);
    out_str(std_handle(2), "\n");
    ExitProcess(1);
}

iptr std_handle(i32 which)
{
    switch (which) {
    case 0:  return GetStdHandle(STD_INPUT_HANDLE);
    case 1:  return GetStdHandle(STD_OUTPUT_HANDLE);
    default: return GetStdHandle(STD_ERROR_HANDLE);
    }
}

void out_str(iptr h, const char *s)
{
    iz n = strlen(s);
    while (n) {
        u32 wrote = 0;
        if (!WriteFile(h, s, (u32)n, &wrote, 0) || !wrote) {
            return;
        }
        s += wrote;
        n -= wrote;
    }
}

void err_msg(const char *msg)
{
    out_str(std_handle(2), "gzip: ");
    out_str(std_handle(2), msg);
    out_str(std_handle(2), "\n");
}

iz c16_len(const c16 *s)
{
    iz n = 0;
    while (s[n]) n++;
    return n;
}

void err_path(const c16 *path, const char *msg)
{
    char buf[2048];
    iz n = 0;
    const char *pre = "gzip: ";
    while (*pre && n < (iz)sizeof(buf) - 4) buf[n++] = *pre++;
    iz plen = c16_len(path);
    if (plen > 700) plen = 700;
    i32 m = WideCharToMultiByte(65001, 0, (c16 *)path, (i32)plen,
                                buf + n, (i32)(sizeof(buf) - n - 4), 0, 0);
    if (m > 0) n += m;
    const char *suf = ": ";
    while (*suf && n < (iz)sizeof(buf) - 3) buf[n++] = *suf++;
    while (*msg && n < (iz)sizeof(buf) - 2) buf[n++] = *msg++;
    buf[n++] = '\n';
    out_str(std_handle(2), buf);
}

void reader_init(reader *r, iptr h, u8 *buf, iz cap)
{
    r->h = h;
    r->buf = buf;
    r->cap = cap;
    r->pos = r->len = 0;
    r->eof = r->err = 0;
}

static b32 reader_fill(reader *r)
{
    if (r->pos < r->len) {
        return 1;
    }
    r->pos = r->len = 0;
    u32 got = 0;
    if (!ReadFile(r->h, r->buf, (u32)r->cap, &got, 0)) {
        u32 e = GetLastError();
        if (e == 109 /* ERROR_BROKEN_PIPE */ || e == 38 /* ERROR_HANDLE_EOF */) {
            r->eof = 1;
            return 0;
        }
        r->err = 1;
        return 0;
    }
    r->len = got;
    if (!got) {
        r->eof = 1;
    }
    return got != 0;
}

iz reader_read(reader *r, void *dst, iz n)
{
    u8 *d = dst;
    iz got = 0;
    while (got < n) {
        if (!reader_fill(r)) {
            break;
        }
        iz avail = r->len - r->pos;
        iz take = MIN(n - got, avail);
        memcpy(d + got, r->buf + r->pos, take);
        r->pos += take;
        got += take;
    }
    return got;
}

i32 reader_byte(reader *r)
{
    if (!reader_fill(r)) {
        return -1;
    }
    return r->buf[r->pos++];
}

void writer_init(writer *w, iptr h, u8 *buf, iz cap, b32 null)
{
    w->h = h;
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->err = 0;
    w->null = null;
}

b32 writer_flush(writer *w)
{
    if (w->err) {
        return 0;
    }
    if (!w->null) {
        iz off = 0;
        while (off < w->len) {
            u32 wrote = 0;
            if (!WriteFile(w->h, w->buf + off, (u32)(w->len - off), &wrote, 0) || !wrote) {
                w->err = 1;
                return 0;
            }
            off += wrote;
        }
    }
    w->len = 0;
    return 1;
}

b32 writer_write(writer *w, const void *p, iz n)
{
    const u8 *s = p;
    while (n) {
        iz space = w->cap - w->len;
        iz take = MIN(n, space);
        memcpy(w->buf + w->len, s, take);
        w->len += take;
        s += take;
        n -= take;
        if (w->len == w->cap && !writer_flush(w)) {
            return 0;
        }
    }
    return !w->err;
}

void writer_byte(writer *w, u8 b)
{
    if (w->len == w->cap) {
        writer_flush(w);
    }
    w->buf[w->len++] = b;
}

iptr file_open_read(const c16 *path)
{
    return CreateFileW((c16 *)path, GENERIC_READ, FILE_SHARE_ALL, 0,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
}

iptr file_open_write(const c16 *path)
{
    return CreateFileW((c16 *)path, GENERIC_WRITE, 0, 0,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
}

b32 file_exists(const c16 *path)
{
    return GetFileAttributesW((c16 *)path) != INVALID_FILE_ATTRIBUTES;
}

b32 file_delete(const c16 *path)
{
    return DeleteFileW((c16 *)path);
}

static c16 *walloc(iz n)
{
    return arena_alloc((n + 1) * (iz)sizeof(c16), sizeof(c16));
}

static c16 *wcopy(c16 *dst, const c16 *src)
{
    while ((*dst = *src) != 0) {
        dst++;
        src++;
    }
    return dst;
}

c16 *path_absolute(const c16 *path)
{
    iz n = c16_len(path);
    c16 *tmp = walloc(n);
    for (iz i = 0; i < n; i++) {
        tmp[i] = path[i] == '/' ? '\\' : path[i];
    }
    tmp[n] = 0;

    if (n >= 4 && tmp[0] == '\\' && tmp[1] == '\\' &&
        (tmp[2] == '?' || tmp[2] == '.') && tmp[3] == '\\') {
        return tmp;
    }

    if (n >= 2 && tmp[0] == '\\' && tmp[1] == '\\') {
        c16 *out = walloc(n + 6);
        c16 *p = wcopy(out, L"\\\\?\\UNC\\");
        wcopy(p, tmp + 2);
        return out;
    }

    b32 absolute = n >= 3 &&
        ((tmp[0] >= 'A' && tmp[0] <= 'Z') || (tmp[0] >= 'a' && tmp[0] <= 'z')) &&
        tmp[1] == ':' && tmp[2] == '\\';

    if (!absolute) {
        iz cap = 512;
        c16 *cwd = 0;
        for (;;) {
            arena_mark m = arena_save();
            cwd = arena_alloc(cap * (iz)sizeof(c16), sizeof(c16));
            u32 got = GetCurrentDirectoryW((u32)cap, cwd);
            if (got == 0) {
                arena_restore(m);
                return 0;
            }
            if (got < (u32)cap) {
                break;
            }
            arena_restore(m);
            cap *= 2;
        }
        iz cl = c16_len(cwd);
        b32 needslash = cl > 0 && cwd[cl - 1] != '\\';
        c16 *joined = walloc(cl + 1 + n);
        wcopy(joined, cwd);
        c16 *j = joined + cl;
        if (needslash) {
            *j++ = '\\';
        }
        wcopy(j, tmp);
        tmp = joined;
        n = c16_len(tmp);
    }

    c16 *out = walloc(n + 4);
    c16 *p = wcopy(out, L"\\\\?\\");
    wcopy(p, tmp);
    return out;
}

c16 *path_append_gz(const c16 *path)
{
    iz n = c16_len(path);
    c16 *out = walloc(n + 3);
    wcopy(out, path);
    out[n] = '.';
    out[n + 1] = 'g';
    out[n + 2] = 'z';
    out[n + 3] = 0;
    return out;
}

c16 *path_strip_gz(const c16 *path)
{
    iz n = c16_len(path);
    if (n < 3) {
        return 0;
    }
    c16 a = path[n - 3], b = path[n - 2], c = path[n - 1];
    if (a != '.' || (b != 'g' && b != 'G') || (c != 'z' && c != 'Z')) {
        return 0;
    }
    c16 *out = walloc(n - 3);
    for (iz i = 0; i < n - 3; i++) {
        out[i] = path[i];
    }
    out[n - 3] = 0;
    return out;
}

// Exposed for main.c
c16 **win_command_line(i32 *argc)
{
    return CommandLineToArgvW(GetCommandLineW(), argc);
}