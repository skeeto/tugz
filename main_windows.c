// CRT-free Win32 platform layer for gzip
// $ cc -O2 -fno-builtin -nostartfiles -o gzip.exe main_windows.c
//      -lmemory -lshell32 -lkernel32
#include "src/base.c"
#include "src/crc32.c"
#include "src/inflate.c"
#include "src/deflate.c"
#include "src/gzip.c"
#include "src/cli.c"

typedef unsigned short c16;
typedef uptr           iptr;

#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)    CloseHandle(iptr);
W32(c16 **) CommandLineToArgvW(c16 *, i32 *);
W32(iptr)   CreateFileW(c16 *, u32, u32, uptr, u32, u32, iptr);
W32(b32)    DeleteFileW(c16 *);
W32(void)   ExitProcess(u32) __attribute((noreturn));
W32(c16 *)  GetCommandLineW(void);
W32(u32)    GetCurrentDirectoryW(u32, c16 *);
W32(u32)    GetFileAttributesW(c16 *);
W32(u32)    GetLastError(void);
W32(iptr)   GetStdHandle(u32);
W32(b32)    ReadFile(iptr, void *, u32, u32 *, uptr);
W32(void *) VirtualAlloc(uptr, iz, u32, u32);
W32(b32)    WriteFile(iptr, void const *, u32, u32 *, uptr);

#define GENERIC_READ               0x80000000u
#define GENERIC_WRITE              0x40000000u
#define FILE_SHARE_ALL             7u
#define CREATE_NEW                 1u
#define CREATE_ALWAYS              2u
#define OPEN_EXISTING              3u
#define FILE_ATTRIBUTE_NORMAL      0x80u
#define FILE_ATTRIBUTE_DIRECTORY   0x10u
#define INVALID_FILE_ATTRIBUTES    0xffffffffu
#define INVALID_HANDLE_VALUE       ((iptr)-1)
#define ERROR_FILE_EXISTS          80u
#define ERROR_BROKEN_PIPE          109u
#define ERROR_HANDLE_EOF           38u
#define MEM_COMMIT                 0x1000u
#define MEM_RESERVE                0x2000u
#define PAGE_READWRITE             4u

enum { MAX_HANDLES = 8 };

struct os {
    iptr handles[MAX_HANDLES];
};

typedef struct {
    c16 *s;
    iz   len;
} s16;

// Convert UTF-16 to WTF-8: unpaired surrogates encode like any other
// code point so that arbitrary file names round trip.
static s8 towtf8(arena *a, c16 *w)
{
    iz wlen = 0;
    for (; w[wlen]; wlen++) {}

    s8 r = {newbytes(a, 3*wlen), 0};
    for (iz i = 0; i < wlen; i++) {
        u32 c = w[i];
        if (c>=0xd800 && c<=0xdbff && i+1<wlen &&
            w[i+1]>=0xdc00 && w[i+1]<=0xdfff) {
            c = 0x10000 + ((c - 0xd800)<<10) + (w[++i] - 0xdc00);
        }
        if (c < 0x80) {
            r.s[r.len++] = (u8)c;
        } else if (c < 0x800) {
            r.s[r.len++] = (u8)(0xc0 | c>>6);
            r.s[r.len++] = (u8)(0x80 | (c & 63));
        } else if (c < 0x10000) {
            r.s[r.len++] = (u8)(0xe0 | c>>12);
            r.s[r.len++] = (u8)(0x80 | (c>>6 & 63));
            r.s[r.len++] = (u8)(0x80 | (c & 63));
        } else {
            r.s[r.len++] = (u8)(0xf0 | c>>18);
            r.s[r.len++] = (u8)(0x80 | (c>>12 & 63));
            r.s[r.len++] = (u8)(0x80 | (c>>6 & 63));
            r.s[r.len++] = (u8)(0x80 | (c & 63));
        }
    }
    return r;
}

// Convert WTF-8 to null-terminated UTF-16. Invalid sequences become
// U+FFFD and slashes become backslashes.
static s16 fromwtf8(arena *a, s8 s)
{
    s16 r = {new(a, s.len+1, c16), 0};
    for (iz i = 0; i < s.len;) {
        u32 c = s.s[i++];
        i32 n = c>=0xc2 && c<0xe0 ? 1 : c>=0xe0 && c<0xf0 ? 2 :
                c>=0xf0 && c<0xf5 ? 3 : 0;
        if (c >= 0x80) {
            u32 min = n==3 ? 0x10000 : n==2 ? 0x800 : 0x80;
            c = n==3 ? c&7 : n==2 ? c&15 : c&31;
            i32 k = 0;
            for (; k<n && i<s.len && (s.s[i]&0xc0)==0x80; k++) {
                c = c<<6 | (s.s[i++] & 63);
            }
            if (!n || k<n || c<min || c>0x10ffff) {
                c = 0xfffd;
            }
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            r.s[r.len++] = (c16)(0xd800 | c>>10);
            r.s[r.len++] = (c16)(0xdc00 | (c & 0x3ff));
        } else {
            r.s[r.len++] = c=='/' ? '\\' : (c16)c;
        }
    }
    r.s[r.len] = 0;
    return r;
}

static c16 *s16cat(arena *a, s16 x, s16 y)
{
    c16 *r = new(a, x.len+y.len+1, c16);
    bytecopy(r, x.s, x.len*(iz)sizeof(c16));
    bytecopy(r+x.len, y.s, y.len*(iz)sizeof(c16));
    return r;
}

static s16 s16lit(c16 *z)
{
    s16 r = {z, 0};
    for (; z[r.len]; r.len++) {}
    return r;
}

// Convert a path to an absolute \\?\ path, lifting the MAX_PATH limit.
static c16 *winpath(arena *a, s8 path)
{
    s16 p = fromwtf8(a, path);
    if (p.len>=4 && p.s[0]=='\\' && p.s[1]=='\\' &&
        (p.s[2]=='?' || p.s[2]=='.') && p.s[3]=='\\') {
        return p.s;
    } else if (p.len>=2 && p.s[0]=='\\' && p.s[1]=='\\') {
        s16 tail = {p.s+2, p.len-2};
        return s16cat(a, s16lit(L"\\\\?\\UNC\\"), tail);
    }

    b32 drive = p.len>=3 && p.s[1]==':' && p.s[2]=='\\' &&
        ((p.s[0]>='A' && p.s[0]<='Z') || (p.s[0]>='a' && p.s[0]<='z'));
    if (!drive) {
        u32 cap = GetCurrentDirectoryW(0, 0);
        s16 cwd = {new(a, cap+1, c16), 0};
        cwd.len = GetCurrentDirectoryW(cap+1, cwd.s);
        if (!cwd.len || cwd.len>cap) {
            return 0;
        }
        if (cwd.s[cwd.len-1] != '\\') {
            cwd.s[cwd.len++] = '\\';
        }
        p.s = s16cat(a, cwd, p);
        p.len += cwd.len;
    }
    return s16cat(a, s16lit(L"\\\\?\\"), p);
}

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    i32 fd = 3;
    for (; fd<MAX_HANDLES && ctx->handles[fd]; fd++) {}
    c16 *wpath = winpath(&scratch, path);
    if (fd==MAX_HANDLES || !wpath) {
        return OS_ERR;
    }

    iptr h = INVALID_HANDLE_VALUE;
    switch (mode) {
    case OS_READ:
        h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_ALL, 0,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
        if (h == INVALID_HANDLE_VALUE) {
            u32 attr = GetFileAttributesW(wpath);
            b32 dir = attr!=INVALID_FILE_ATTRIBUTES &&
                      (attr & FILE_ATTRIBUTE_DIRECTORY);
            return dir ? OS_EISDIR : OS_ERR;
        }
        break;
    case OS_CREATE:
    case OS_FORCE:
        h = CreateFileW(wpath, GENERIC_WRITE, 0, 0,
                        mode==OS_CREATE ? CREATE_NEW : CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, 0);
        if (h == INVALID_HANDLE_VALUE) {
            return GetLastError()==ERROR_FILE_EXISTS ? OS_EEXIST : OS_ERR;
        }
        break;
    }
    ctx->handles[fd] = h;
    return fd;
}

static void os_close(os *ctx, i32 fd)
{
    CloseHandle(ctx->handles[fd]);
    ctx->handles[fd] = 0;
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    u32 got = 0;
    if (!ReadFile(ctx->handles[fd], buf, (u32)MIN(cap, 1<<30), &got, 0)) {
        u32 err = GetLastError();
        return err==ERROR_BROKEN_PIPE || err==ERROR_HANDLE_EOF ? 0 : -1;
    }
    return got;
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    while (len) {
        u32 wrote = 0;
        u32 n = (u32)MIN(len, 1<<30);
        if (!WriteFile(ctx->handles[fd], buf, n, &wrote, 0) || !wrote) {
            return 0;
        }
        buf += wrote;
        len -= wrote;
    }
    return 1;
}

static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    return wpath && DeleteFileW(wpath);
}

static void os_fail(os *ctx)
{
    (void)ctx;
    ExitProcess(EXIT_ERR);
}

__attribute((force_align_arg_pointer))
void mainCRTStartup(void)
{
    os ctx = {0};
    ctx.handles[0] = GetStdHandle((u32)-10);
    ctx.handles[1] = GetStdHandle((u32)-11);
    ctx.handles[2] = GetStdHandle((u32)-12);

    iz cap = (iz)1 << 25;
    config conf = {0};
    conf.perm.beg = VirtualAlloc(0, cap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!conf.perm.beg) {
        ExitProcess(EXIT_ERR);
    }
    conf.perm.end = conf.perm.beg + cap;
    conf.perm.ctx = &ctx;

    i32 argc = 0;
    c16 **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    conf.nargs = argv ? argc-1 : 0;
    conf.args = new(&conf.perm, conf.nargs, s8);
    for (i32 i = 0; i < conf.nargs; i++) {
        conf.args[i] = towtf8(&conf.perm, argv[i+1]);
    }
    ExitProcess((u32)gzip_main(&conf));
}
