// Shared CRT-free Win32 platform code: the os_* file interface of
// src/io.c, path conversion, and the command line. Included by each
// Windows program after the sources it needs.

typedef unsigned short c16;
typedef uptr           iptr;

#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)    CloseHandle(iptr);
W32(c16 **) CommandLineToArgvW(c16 *, i32 *);
W32(iptr)   CreateFileW(c16 *, u32, u32, uptr, u32, u32, iptr);
W32(b32)    DeleteFileW(c16 *);
[[noreturn]] W32(void) ExitProcess(u32);
W32(c16 *)  GetCommandLineW(void);
W32(u32)    GetCurrentDirectoryW(u32, c16 *);
W32(u32)    GetFullPathNameW(c16 *, u32, c16 *, c16 **);
W32(u32)    GetFileAttributesW(c16 *);
W32(b32)    GetConsoleMode(iptr, u32 *);
W32(b32)    GetFileInformationByHandle(iptr, void *);
W32(b32)    GetFileInformationByHandleEx(iptr, i32, void *, u32);
W32(u32)    GetFileType(iptr);
W32(u32)    GetLastError(void);
W32(iptr)   GetStdHandle(u32);
W32(b32)    ReadFile(iptr, void *, u32, u32 *, uptr);
W32(b32)    SetFileInformationByHandle(iptr, i32, void *, u32);
W32(void *) VirtualAlloc(uptr, iz, u32, u32);
W32(b32)    WriteFile(iptr, void const *, u32, u32 *, uptr);

#define DELETE                     0x00010000u
#define GENERIC_READ               0x80000000u
#define GENERIC_WRITE              0x40000000u
#define FILE_SHARE_ALL             7u
#define CREATE_NEW                 1u
#define CREATE_ALWAYS              2u
#define OPEN_EXISTING              3u
#define FILE_ATTRIBUTE_NORMAL      0x80u
#define FILE_ATTRIBUTE_DIRECTORY   0x10u
#define FILE_ATTRIBUTE_REPARSE     0x400u
#define FILE_FLAG_OPEN_REPARSE     0x00200000u
#define FILE_TYPE_DISK             1u
#define IO_REPARSE_TAG_MOUNT_POINT 0xa0000003u
#define IO_REPARSE_TAG_SYMLINK     0xa000000cu
#define INVALID_FILE_ATTRIBUTES    0xffffffffu
#define INVALID_HANDLE_VALUE       ((iptr)-1)
#define ERROR_ACCESS_DENIED        5u
#define ERROR_SHARING_VIOLATION    32u
#define ERROR_FILE_EXISTS          80u
#define ERROR_BROKEN_PIPE          109u
#define ERROR_HANDLE_EOF           38u
#define MEM_COMMIT                 0x1000u
#define MEM_RESERVE                0x2000u
#define PAGE_READWRITE             4u

enum {
    FileBasicInfo        = 0,
    FileDispositionInfo  = 4,
    FileAttributeTagInfo = 9,
};

typedef struct {
    u32 attributes;
    u32 created[2], accessed[2], written[2];
    u32 volume;
    u32 size_hi, size_lo;
    u32 links;
    u32 index_hi, index_lo;
} by_handle_info;

typedef struct {
    i64 created, accessed, written, changed;  // zero means unchanged
    u32 attributes;                           // zero means unchanged
} basic_info;

typedef struct {
    u32 attributes;
    u32 reparse_tag;
} attribute_tag_info;

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

[[maybe_unused]] static c16 *s16cat(arena *a, s16 x, s16 y)
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

// A device path, \\?\ or \\.\, which Win32 does not resolve further.
static b32 isdevice(s16 p)
{
    return p.len>=4 && p.s[0]=='\\' && p.s[1]=='\\' &&
           (p.s[2]=='?' || p.s[2]=='.') && p.s[3]=='\\';
}

// Length of an absolute path's root: a drive "X:", a share
// "\\server\share", or a device path through its first component
// ("\\?\X:", "\\?\Volume{...}") or share ("\\?\UNC\server\share").
// Zero for a relative path.
static iz rootlen(s16 p)
{
    c16 *s = p.s;
    if (p.len < 2) {
        return 0;
    } else if (s[0]!='\\' || s[1]!='\\') {
        b32 letter = (s[0]>='A' && s[0]<='Z') || (s[0]>='a' && s[0]<='z');
        return letter && s[1]==':' ? 2 : 0;
    }
    iz  i     = 2;
    i32 parts = 2;  // server and share
    if (isdevice(p)) {
        b32 unc = p.len>=8 && (s[4]|32)=='u' && (s[5]|32)=='n' &&
                  (s[6]|32)=='c' && s[7]=='\\';
        i     = unc ? 8 : 4;
        parts = unc ? 2 : 1;
    }
    for (i32 k = 0; k < parts; k++) {
        i += k && i<p.len;  // separator
        for (; i<p.len && s[i]!='\\'; i++) {}
    }
    return i;
}

// The current directory, or that of a drive given its letter.
static s16 curdir(arena *a, c16 drive)
{
    c16 name[] = {drive, ':', 0};
    u32 cap = drive ? GetFullPathNameW(name, 0, 0, 0)
                    : GetCurrentDirectoryW(0, 0);
    if (!cap) {
        return (s16){0};
    }
    s16 r = {new(a, cap, c16), 0};
    u32 len = drive ? GetFullPathNameW(name, cap, r.s, 0)
                    : GetCurrentDirectoryW(cap, r.s);
    if (!len || len>=cap) {
        return (s16){0};
    }
    r.len = len;
    return r;
}

// Convert a path to an absolute \\?\ path, lifting the MAX_PATH limit.
// That prefix turns off Win32 path parsing, so first resolve the path as
// Win32 would: against the current directory (or a drive's, or its
// root), dropping "." and ".." components and doubled separators. Unlike
// Win32, keep trailing dots and spaces, so that names from a directory
// listing round trip. Device paths pass through. Returns null for an
// empty path, which names no file.
static c16 *winpath(arena *a, s8 path)
{
    s16 p = fromwtf8(a, path);
    if (!p.len) {
        return 0;
    } else if (isdevice(p)) {
        return p.s;
    }

    b32 dirsep = p.s[p.len-1] == '\\';
    iz  root   = rootlen(p);
    s16 base   = {0};
    if (!root) {
        base = curdir(a, 0);
        if (!base.s) {
            return 0;
        } else if (p.s[0] == '\\') {
            base.len = rootlen(base);  // root-relative
        }
    } else if (p.s[1]==':' && (p.len==2 || p.s[2]!='\\')) {
        base = curdir(a, p.s[0]);  // drive-relative, "X:name"
        if (!base.s) {
            return 0;
        }
        p.s   += 2;
        p.len -= 2;
    }
    if (base.s) {
        c16 *s = new(a, base.len+1+p.len, c16);
        bytecopy(s, base.s, base.len*(iz)sizeof(c16));
        s[base.len] = '\\';
        bytecopy(s+base.len+1, p.s, p.len*(iz)sizeof(c16));
        p.s   = s;
        p.len = base.len + 1 + p.len;
        root  = rootlen(p);
        if (!root) {
            return 0;
        }
    }

    // Prefix the root: "C:" to "\\?\C:", "\\server" to "\\?\UNC\server"
    s16 pre  = s16lit(L"\\\\?\\");
    iz  skip = 0;
    if (isdevice(p)) {
        pre.len = 0;
    } else if (p.s[0] == '\\') {
        pre  = s16lit(L"\\\\?\\UNC");
        skip = 1;
    }
    c16 *r = new(a, pre.len+p.len+2, c16);
    bytecopy(r, pre.s, pre.len*(iz)sizeof(c16));
    bytecopy(r+pre.len, p.s+skip, (root-skip)*(iz)sizeof(c16));
    iz top = pre.len + root - skip;

    iz n = top;
    for (iz i = root, end; i < p.len; i = end + 1) {
        for (end = i; end<p.len && p.s[end]!='\\'; end++) {}
        iz len = end - i;
        if (!len || (len==1 && p.s[i]=='.')) {
            continue;
        } else if (len==2 && p.s[i]=='.' && p.s[i+1]=='.') {
            for (; n>top && r[n-1]!='\\'; n--) {}
            n -= n > top;
            continue;
        }
        r[n++] = '\\';
        bytecopy(r+n, p.s+i, len*(iz)sizeof(c16));
        n += len;
    }
    if (n==top || dirsep) {
        r[n++] = '\\';  // a root is a directory only with its separator
    }
    r[n] = 0;
    return r;
}

// Returns zero and a handle, or an OS_E* code.
static i32 open_input(c16 *wpath, i32 mode, iptr *out)
{
    u32 flags = FILE_ATTRIBUTE_NORMAL;
    flags |= mode & OS_NOFOLLOW ? FILE_FLAG_OPEN_REPARSE : 0;
    iptr h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_ALL, 0,
                         OPEN_EXISTING, flags, 0);
    if (h == INVALID_HANDLE_VALUE) {
        u32 attr = GetFileAttributesW(wpath);
        b32 dir = attr!=INVALID_FILE_ATTRIBUTES &&
                  (attr & FILE_ATTRIBUTE_DIRECTORY);
        return dir ? OS_EISDIR : OS_ERR;
    }

    if (mode & OS_NOFOLLOW) {
        // Refuse links, but other reparse points (e.g. cloud placeholder
        // files) are ordinary files: reopen those normally.
        attribute_tag_info tag = {0};
        GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag));
        if (tag.attributes & FILE_ATTRIBUTE_REPARSE) {
            CloseHandle(h);
            if (tag.reparse_tag==IO_REPARSE_TAG_SYMLINK ||
                tag.reparse_tag==IO_REPARSE_TAG_MOUNT_POINT) {
                return OS_ESYMLINK;
            }
            return open_input(wpath, mode & ~OS_NOFOLLOW, out);
        }
    }

    i32 err = 0;
    by_handle_info info = {0};
    if ((mode & OS_REGULAR) && GetFileType(h)!=FILE_TYPE_DISK) {
        err = OS_ENOTREG;
    } else if (!GetFileInformationByHandle(h, &info)) {
        err = (mode & OS_REGULAR) ? OS_ENOTREG : 0;
    } else if ((mode & OS_ONELINK) && info.links>1) {
        err = OS_ELINKS;
    }
    if (err) {
        CloseHandle(h);
        return err;
    }
    *out = h;
    return 0;
}

// Created files are marked delete-pending immediately, so that the file
// system removes them however the process ends, until os_keep.
static i32 open_output(os *ctx, i32 fd, c16 *wpath, i32 mode)
{
    if (mode & OS_FORCE) {
        DeleteFileW(wpath);  // replace rather than write through a link
    }
    iptr h = CreateFileW(wpath, GENERIC_WRITE|DELETE, 0, 0, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        // A name that another process holds delete-pending (its output
        // not yet kept) refuses access rather than reporting that it
        // exists, so check for it
        u32 err = GetLastError();
        if (err==ERROR_ACCESS_DENIED || err==ERROR_SHARING_VIOLATION) {
            b32 found = GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES;
            err = found || GetLastError()==ERROR_ACCESS_DENIED ?
                  ERROR_FILE_EXISTS : err;
        }
        return err==ERROR_FILE_EXISTS ? OS_EEXIST : OS_ERR;
    }
    u8 discard = 1;
    if (!SetFileInformationByHandle(h, FileDispositionInfo,
                                    &discard, sizeof(discard))) {
        CloseHandle(h);
        DeleteFileW(wpath);
        return OS_ERR;
    }
    ctx->handles[fd] = h;
    return fd;
}

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    i32 fd = 3;
    for (; fd<MAX_HANDLES && ctx->handles[fd]; fd++) {}
    c16 *wpath = winpath(&scratch, path);
    if (fd==MAX_HANDLES || !wpath) {
        return OS_ERR;
    }

    if (mode & (OS_CREATE|OS_FORCE)) {
        return open_output(ctx, fd, wpath, mode);
    }
    iptr h = 0;
    i32 err = open_input(wpath, mode, &h);
    if (err) {
        return err;
    }
    ctx->handles[fd] = h;
    return fd;
}

static b32 os_close(os *ctx, i32 fd)
{
    b32 ok = CloseHandle(ctx->handles[fd]);
    ctx->handles[fd] = 0;
    return ok;
}

[[maybe_unused]] static void os_keep(os *ctx, i32 fd)
{
    u8 keep = 0;
    SetFileInformationByHandle(ctx->handles[fd], FileDispositionInfo,
                               &keep, sizeof(keep));
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

[[maybe_unused]] static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    return wpath && DeleteFileW(wpath);
}

static void os_exit(os *ctx, i32 status)
{
    (void)ctx;  // created files not kept are delete-pending
    ExitProcess((u32)status);
}

// Initialize standard handles and allocate a committed arena of cap
// bytes. Exits on failure.
static arena os_init(os *ctx, iz cap)
{
    ctx->handles[0] = GetStdHandle((u32)-10);
    ctx->handles[1] = GetStdHandle((u32)-11);
    ctx->handles[2] = GetStdHandle((u32)-12);
    arena a = {0};
    a.beg = VirtualAlloc(0, cap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!a.beg) {
        ExitProcess(1);
    }
    a.end = a.beg + cap;
    a.ctx = ctx;
    return a;
}

// Command line arguments as WTF-8, including the program name first.
static s8 *os_args(arena *a, i32 *argc)
{
    *argc = 0;
    c16 **argv = CommandLineToArgvW(GetCommandLineW(), argc);
    if (!argv) {
        *argc = 0;
    }
    s8 *args = new(a, *argc, s8);
    for (i32 i = 0; i < *argc; i++) {
        args[i] = towtf8(a, argv[i]);
    }
    return args;
}
