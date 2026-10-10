// Shared CRT-free Win32 platform code: the os_* file interface of
// src/io.c, directory listings for src/dir.c, path conversion, the
// consoles, error wording, and the command line. Included by each
// Windows program after the sources it needs.

typedef unsigned short c16;
typedef uptr           iptr;

typedef struct {
    u32 attributes;
    u32 created[2], accessed[2], written[2];
    u32 size_hi, size_lo;
    u32 reserved[2];
    c16 name[260];
    c16 altname[14];
} find_data;

typedef void (__stdcall *w32proc)(void);

// Only functions that Windows XP has are imported: see winapi for the
// newer ones, looked up at run time.
#define W32(r) __declspec(dllimport) r __stdcall
W32(b32)    CloseHandle(iptr);
W32(c16 **) CommandLineToArgvW(c16 *, i32 *);
W32(iptr)   CreateFileW(c16 *, u32, u32, uptr, u32, u32, iptr);
W32(b32)    DeleteFileW(c16 *);
[[noreturn]] W32(void) ExitProcess(u32);
W32(b32)    FindClose(iptr);
W32(iptr)   FindFirstFileExW(c16 *, i32, find_data *, i32, uptr, u32);
W32(b32)    FindNextFileW(iptr, find_data *);
W32(c16 *)  GetCommandLineW(void);
W32(u32)    GetCurrentDirectoryW(u32, c16 *);
W32(u32)    GetFullPathNameW(c16 *, u32, c16 *, c16 **);
W32(u32)    GetFileAttributesW(c16 *);
W32(b32)    GetConsoleMode(iptr, u32 *);
W32(b32)    GetFileInformationByHandle(iptr, void *);
W32(u32)    GetFileType(iptr);
W32(u32)    GetLastError(void);
W32(iptr)   GetModuleHandleW(c16 const *);
W32(b32)    GetNamedPipeHandleStateW(iptr, u32 *, u32 *, u32 *, u32 *, c16 *,
                                     u32);
W32(w32proc) GetProcAddress(iptr, char const *);
W32(iptr)   GetStdHandle(u32);
W32(u32)    GetTickCount(void);
W32(i32)    LCMapStringW(u32, u32, c16 const *, i32, c16 *, i32);
W32(b32)    ReadConsoleW(iptr, c16 *, u32, u32 *, uptr);
W32(b32)    ReadFile(iptr, void *, u32, u32 *, uptr);
W32(b32)    SetNamedPipeHandleState(iptr, u32 *, u32 *, u32 *);
W32(void)   SetLastError(u32);
W32(void *) VirtualAlloc(uptr, iz, u32, u32);
W32(b32)    WriteConsoleW(iptr, c16 const *, u32, u32 *, uptr);
W32(b32)    WriteFile(iptr, void const *, u32, u32 *, uptr);

#define DELETE                     0x00010000u
#define GENERIC_READ               0x80000000u
#define GENERIC_WRITE              0x40000000u
#define FILE_READ_ATTRIBUTES       0x80u
#define FILE_WRITE_ATTRIBUTES      0x100u
#define FILE_SHARE_ALL             7u
#define CREATE_NEW                 1u
#define CREATE_ALWAYS              2u
#define OPEN_EXISTING              3u
#define FILE_ATTRIBUTE_READONLY    0x01u
#define FILE_ATTRIBUTE_HIDDEN      0x02u
#define FILE_ATTRIBUTE_SYSTEM      0x04u
#define FILE_ATTRIBUTE_NORMAL      0x80u
#define FILE_ATTRIBUTE_DIRECTORY   0x10u
#define FILE_ATTRIBUTE_REPARSE     0x400u
#define FILE_FLAG_OPEN_REPARSE     0x00200000u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_TYPE_UNKNOWN          0u
#define FILE_TYPE_DISK             1u
#define FIND_FIRST_EX_LARGE_FETCH  2u
#define PIPE_NOWAIT                1u
#define PIPE_READMODE_MESSAGE      2u
#define IO_REPARSE_TAG_SURROGATE   0x20000000u
#define INVALID_FILE_ATTRIBUTES    0xffffffffu
#define INVALID_HANDLE_VALUE       ((iptr)-1)
#define LOCALE_INVARIANT           0x7fu
#define LCMAP_UPPERCASE            0x200u
#define ERROR_INVALID_FUNCTION     1u
#define ERROR_FILE_NOT_FOUND       2u
#define ERROR_PATH_NOT_FOUND       3u
#define ERROR_TOO_MANY_OPEN_FILES  4u
#define ERROR_ACCESS_DENIED        5u
#define ERROR_NO_MORE_FILES        18u
#define ERROR_SHARING_VIOLATION    32u
#define ERROR_NOT_SUPPORTED        50u
#define ERROR_FILE_EXISTS          80u
#define ERROR_INVALID_PARAMETER    87u
#define ERROR_BROKEN_PIPE          109u
#define ERROR_INVALID_NAME         123u
#define ERROR_HANDLE_EOF           38u
#define ERROR_NO_DATA              232u
#define ERROR_MORE_DATA            234u
#define MEM_COMMIT                 0x1000u
#define MEM_RESERVE                0x2000u
#define PAGE_READWRITE             4u

// FILE_INFO_BY_HANDLE_CLASS, of GetFileInformationByHandleEx and
// SetFileInformationByHandle
enum {
    FileBasicInfo        = 0,
    FileRenameInfo       = 3,
    FileDispositionInfo  = 4,
    FileEndOfFileInfo    = 6,
    FileAttributeTagInfo = 9,
    FileIdInfo           = 18,
    FileRenameInfoEx     = 22,
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

// Whether a reparse point is a link: its tag a name surrogate, which
// names another file, as a symbolic link, a junction, or a WSL or
// container link does (IsReparseTagNameSurrogate). Others, such as a
// cloud placeholder or a deduplicated file, are the file itself.
static b32 reparse_link(attribute_tag_info tag)
{
    return (tag.attributes & FILE_ATTRIBUTE_REPARSE) &&
           (tag.reparse_tag & IO_REPARSE_TAG_SURROGATE);
}

typedef struct {
    u32  flags;  // FileRenameInfoEx, else a BOOLEAN ReplaceIfExists
    iptr root;
    u32  len;    // bytes
    c16  name[];
} rename_info;

typedef struct {
    uptr status;  // an NTSTATUS, or a pointer
    uptr information;
} io_status;

typedef struct {
    u16  len, cap;  // bytes
    c16 *s;
} unicode_string;

// Functions newer than Windows XP (Vista's), looked up at run time so
// that the programs import only XP's, and where they are missing, the
// native functions of ntdll, which every version has, through which they
// work. Defining TUGZ_FORCE_XP pretends that Vista's are missing, and
// that FindFirstFileExW takes only XP's arguments (find_first), to test
// the fallbacks on a newer Windows. A function not found is null.
typedef struct {
    b32 (__stdcall *getinfo)(iptr, i32, void *, u32);   // ...ByHandleEx
    b32 (__stdcall *setinfo)(iptr, i32, void *, u32);   // ...ByHandle
    u32 (__stdcall *finalpath)(iptr, c16 *, u32, u32);  // ...NameByHandleW
    i32 (__stdcall *ntquery)(iptr, io_status *, void *, u32, i32);
    i32 (__stdcall *ntset)(iptr, io_status *, void *, u32, i32);
    i32 (__stdcall *ntname)(iptr, i32, void *, u32, u32 *);  // NtQueryObject
    u32 (__stdcall *doserror)(i32);  // RtlNtStatusToDosError
    b32 oldfind;  // FindFirstFileExW has refused Windows 7's arguments
} winapi;

#ifdef TUGZ_FORCE_XP
enum { FORCE_XP = 1 };
#else
enum { FORCE_XP = 0 };
#endif

enum { MAX_HANDLES = 8 };

struct os {
    iptr  handles[MAX_HANDLES];
    u32   consoles;    // bit for each handle that is a console
    u8    held[3][4];  // for each output, an incomplete UTF-8 sequence
    u8    nheld[3];
    u8    rest[8];     // console input converted but not yet read
    u8    nrest;
    c16   high;        // a high surrogate read last, for its low one
    b32   wildnames;   // gzip: a name with * or ? is not found (os_open)
    i32   unsure;      // creations refused in a row, each name maybe taken
    iptr  guard;       // zip's archive, held so that it can be replaced
    byte *lo;          // zip: the uncommitted middle of its memory
    byte *hi;
    winapi api;
};

// Look up the functions of winapi, once (os_init).
static void load_api(winapi *w)
{
    iptr k32 = GetModuleHandleW(L"kernel32.dll");
    iptr nt  = GetModuleHandleW(L"ntdll.dll");  // in every process
    if (!FORCE_XP && k32) {
        w->getinfo   = (b32 (__stdcall *)(iptr, i32, void *, u32))
                       GetProcAddress(k32, "GetFileInformationByHandleEx");
        w->setinfo   = (b32 (__stdcall *)(iptr, i32, void *, u32))
                       GetProcAddress(k32, "SetFileInformationByHandle");
        w->finalpath = (u32 (__stdcall *)(iptr, c16 *, u32, u32))
                       GetProcAddress(k32, "GetFinalPathNameByHandleW");
    }
    if (nt) {
        w->ntquery  = (i32 (__stdcall *)(iptr, io_status *, void *, u32, i32))
                      GetProcAddress(nt, "NtQueryInformationFile");
        w->ntset    = (i32 (__stdcall *)(iptr, io_status *, void *, u32, i32))
                      GetProcAddress(nt, "NtSetInformationFile");
        w->ntname   = (i32 (__stdcall *)(iptr, i32, void *, u32, u32 *))
                      GetProcAddress(nt, "NtQueryObject");
        w->doserror = (u32 (__stdcall *)(i32))
                      GetProcAddress(nt, "RtlNtStatusToDosError");
    }
}

// The FILE_INFORMATION_CLASS of a FILE_INFO_BY_HANDLE_CLASS, whose
// structure is the same, or zero where XP has none.
static i32 ntclass(i32 cls)
{
    switch (cls) {
    case FileBasicInfo:        return 4;   // FileBasicInformation
    case FileRenameInfo:       return 10;  // FileRenameInformation
    case FileDispositionInfo:  return 13;  // FileDispositionInformation
    case FileEndOfFileInfo:    return 20;  // FileEndOfFileInformation
    case FileAttributeTagInfo: return 35;  // FileAttributeTagInformation
    }
    return 0;  // FileIdInfo, FileRenameInfoEx: refused, as Windows 7 does
}

// The result of a native call, its status, as the Win32 one would give it.
static b32 ntresult(winapi *w, i32 status)
{
    if (status < 0) {  // an error or a warning, not NT_SUCCESS
        SetLastError(w->doserror ? w->doserror(status)
                                 : ERROR_INVALID_FUNCTION);
        return 0;
    }
    return 1;
}

// GetFileInformationByHandleEx, or as it does, NtQueryInformationFile.
static b32 get_info(os *ctx, iptr h, i32 cls, void *buf, u32 len)
{
    winapi *w = &ctx->api;
    if (w->getinfo) {
        return w->getinfo(h, cls, buf, len);
    } else if (!ntclass(cls) || !w->ntquery) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    io_status io = {0};
    return ntresult(w, w->ntquery(h, &io, buf, len, ntclass(cls)));
}

// SetFileInformationByHandle, or as it does, NtSetInformationFile, which
// renames to an NT path, where it renames to a Win32 one. Each name to
// rename to is a \\?\ path (winpath), which is \??\ in NT.
static b32 set_info(os *ctx, iptr h, i32 cls, void *buf, u32 len)
{
    winapi *w = &ctx->api;
    if (w->setinfo) {
        return w->setinfo(h, cls, buf, len);
    } else if (!ntclass(cls) || !w->ntset) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    c16 *name = 0;
    if (cls == FileRenameInfo) {
        rename_info *ri = buf;
        name = ri->len>=8 && ri->name[0]=='\\' && ri->name[1]=='\\' &&
               ri->name[2]=='?' && ri->name[3]=='\\' ? ri->name : 0;
    }
    if (name) {
        name[1] = '?';
    }
    io_status io = {0};
    i32 status = w->ntset(h, &io, buf, len, ntclass(cls));
    if (name) {
        name[1] = '\\';
    }
    return ntresult(w, status);
}

typedef struct {
    c16 *s;
    iz   len;
} s16;

// Encode UTF-16 as WTF-8, where unpaired surrogates encode like any
// other code point so that arbitrary file names round trip, into d
// unless null. Returns the length, at most three bytes a unit.
static iz wtf8_encode(u8 *d, c16 const *w, iz wlen)
{
    static u8 const lead[] = {0, 0, 0xc0, 0xe0, 0xf0};
    iz len = 0;
    for (iz i = 0; i < wlen; i++) {
        u32 c = w[i];
        if (c>=0xd800 && c<=0xdbff && i+1<wlen &&
            w[i+1]>=0xdc00 && w[i+1]<=0xdfff) {
            c = 0x10000 + ((c - 0xd800)<<10) + (w[++i] - 0xdc00);
        }
        i32 n = c<0x80 ? 1 : c<0x800 ? 2 : c<0x10000 ? 3 : 4;
        for (i32 k = n-1; d && k>0; k--, c >>= 6) {
            d[len+k] = (u8)(0x80 | (c & 63));
        }
        if (d) {
            d[len] = (u8)(lead[n] | c);
        }
        len += n;
    }
    return len;
}

// Convert null-terminated UTF-16 to WTF-8, allocating just its length,
// since an arena that grows down (zip's scratch) could not give back
// the rest of a guess at it, as for every name in a listing.
static s8 towtf8(arena *a, c16 *w)
{
    iz wlen = 0;
    for (; w[wlen]; wlen++) {}
    s8 r  = {0};
    r.len = wtf8_encode(0, w, wlen);
    r.s   = newstr(a, r.len);
    wtf8_encode(r.s, w, wlen);
    return r;
}

// Continuation bytes following a UTF-8 lead byte, or 0 if c is not one.
static i32 wtf8_tail(u32 c)
{
    return c>=0xc2 && c<0xe0 ? 1 : c>=0xe0 && c<0xf0 ? 2 :
           c>=0xf0 && c<0xf5 ? 3 : 0;
}

// Decode a code point from WTF-8 at s[*i], advancing past it. Invalid
// sequences become U+FFFD.
static u32 wtf8_decode(u8 const *s, iz len, iz *i)
{
    u32 c = s[(*i)++];
    if (c >= 0x80) {
        i32 n   = wtf8_tail(c);
        u32 min = n==3 ? 0x10000 : n==2 ? 0x800 : 0x80;
        c = n==3 ? c&7 : n==2 ? c&15 : c&31;
        i32 k = 0;
        for (; k<n && *i<len && (s[*i]&0xc0)==0x80; k++) {
            c = c<<6 | (s[(*i)++] & 63);
        }
        if (!n || k<n || c<min || c>0x10ffff) {
            c = 0xfffd;
        }
    }
    return c;
}

// Append a code point as UTF-16 and return the new length.
static iz utf16_put(c16 *d, iz len, u32 c)
{
    if (c >= 0x10000) {
        c -= 0x10000;
        d[len++] = (c16)(0xd800 | c>>10);
        d[len++] = (c16)(0xdc00 | (c & 0x3ff));
    } else {
        d[len++] = (c16)c;
    }
    return len;
}

// Convert WTF-8 to null-terminated UTF-16. Invalid sequences become
// U+FFFD and slashes become backslashes.
static s16 fromwtf8(arena *a, s8 s)
{
    s16 r = {new(a, s.len+1, c16), 0};
    for (iz i = 0; i < s.len;) {
        u32 c = wtf8_decode(s.s, s.len, &i);
        r.len = utf16_put(r.s, r.len, c=='/' ? '\\' : c);
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

// A device path, \\?\ or \\.\, after slashes became backslashes.
static b32 isdevice(s16 p)
{
    return p.len>=4 && p.s[0]=='\\' && p.s[1]=='\\' &&
           (p.s[2]=='?' || p.s[2]=='.') && p.s[3]=='\\';
}

// A path written exactly \\?\..., which Win32 takes as it is. It
// resolves any other device path (//?/..., \\.\...) like any path.
static b32 isverbatim(s8 path)
{
    return path.len>=4 && path.s[0]=='\\' && path.s[1]=='\\' &&
           path.s[2]=='?' && path.s[3]=='\\';
}

// Length of an absolute path's root: a drive "X:", a share
// "\\server\share", or a device path through its first component
// ("\\?\X:", "\\?\Volume{...}") or share ("\\?\UNC\server\share").
// Zero for a relative path, and negative for a root missing one of
// these parts ("\\server\", "\\?\"), which names nothing.
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
        iz beg = i;
        for (; i<p.len && s[i]!='\\'; i++) {}
        if (i == beg) {
            return -1;
        }
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

// The device path of a bare name that Win32 takes for a DOS device (NUL,
// CON, COM1, ..., perhaps with an extension, as GetFullPathNameW decides
// for this version of Windows), or null.
static c16 *dosdevice(arena *a, s16 p)
{
    for (iz i = 0; i < p.len; i++) {
        if (p.s[i] == '\\') {
            return 0;
        }
    }
    if (rootlen(p)) {
        return 0;  // drive-relative
    }
    c16 dev[16];
    u32 len = GetFullPathNameW(p.s, countof(dev), dev, 0);
    if (!len || len>=countof(dev) || !isdevice((s16){dev, len}) ||
        dev[2]!='.') {
        return 0;
    }
    c16 *r = new(a, len+1, c16);
    bytecopy(r, dev, (len+1)*(iz)sizeof(c16));
    return r;
}

// Convert a path to an absolute \\?\ path, lifting the MAX_PATH limit.
// That prefix turns off Win32 path parsing, so first resolve the path as
// Win32 would: against the current directory (or a drive's, or its
// root), dropping "." and ".." components and doubled separators. Unlike
// Win32, keep trailing dots and spaces, so that names from a directory
// listing round trip, and stop ".." at a device path's volume or share.
// A \\.\ path, which Win32 would parse again, becomes \\?\, which names
// the same devices. A path written exactly \\?\ passes through, as Win32
// passes it, and a bare DOS device name (NUL) becomes its device as in
// any Windows program. Within a directory such names stay files, as they
// are in a listing, since other systems make them. Returns null for a
// path that names no file: an empty one, or one with an incomplete root.
static c16 *winpath(arena *a, s8 path)
{
    s16 p = fromwtf8(a, path);
    if (!p.len) {
        return 0;
    } else if (isverbatim(path)) {
        return p.s;
    }
    c16 *dev = dosdevice(a, p);
    if (dev) {
        return dev;
    }

    b32 dirsep  = p.s[p.len-1] == '\\';
    b32 devpath = isdevice(p);
    iz  root    = rootlen(p);
    s16 base    = {0};
    if (root < 0) {
        return 0;
    } else if (!root) {
        base = curdir(a, 0);
        if (base.s && p.s[0]=='\\') {
            base.len = rootlen(base);  // root-relative
        }
        if (base.len <= 0) {
            return 0;
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
        if (root <= 0) {
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
    r[2] = '?';  // \\.\ too
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
    // A root is a directory only with its separator, but as in Win32, a
    // device path keeps one only if it ends in one: \\?\C: is the volume
    if (dirsep || (n==top && !devpath)) {
        r[n++] = '\\';
    }
    r[n] = 0;
    return r;
}

// Returns zero and a handle, or an OS_E* code.
static i32 open_input(os *ctx, c16 *wpath, i32 mode, iptr *out)
{
    u32 flags = FILE_ATTRIBUTE_NORMAL;
    flags |= mode & OS_NOFOLLOW ? FILE_FLAG_OPEN_REPARSE : 0;
    iptr h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_ALL, 0,
                         OPEN_EXISTING, flags, 0);
    if (h == INVALID_HANDLE_VALUE) {
        u32 why  = GetLastError();
        u32 attr = GetFileAttributesW(wpath);
        b32 dir  = attr!=INVALID_FILE_ATTRIBUTES &&
                   (attr & FILE_ATTRIBUTE_DIRECTORY);
        SetLastError(why);  // not the check's, as for a volume (87)
        return dir ? OS_EISDIR : OS_ERR;
    }

    if (mode & OS_NOFOLLOW) {
        // Refuse links, but other reparse points (e.g. cloud placeholder
        // files) are ordinary files: reopen those normally.
        attribute_tag_info tag = {0};
        get_info(ctx, h, FileAttributeTagInfo, &tag, sizeof(tag));
        if (tag.attributes & FILE_ATTRIBUTE_REPARSE) {
            CloseHandle(h);
            if (reparse_link(tag)) {
                return OS_ESYMLINK;
            }
            return open_input(ctx, wpath, mode & ~OS_NOFOLLOW, out);
        }
    }

    // A volume or raw disk is a disk file too, but it has no file
    // information, which tells it apart from a file that failed to give
    // it, as through a network error, which is no file type. Likewise an
    // unknown type is one only if telling it did not fail.
    i32 err  = 0;
    u32 type = FILE_TYPE_DISK;
    if (mode & OS_REGULAR) {
        SetLastError(0);
        type = GetFileType(h);
    }
    by_handle_info info = {0};
    if (type==FILE_TYPE_UNKNOWN && GetLastError()) {
        err = OS_ERR;
    } else if (type != FILE_TYPE_DISK) {
        err = OS_ENOTREG;
    } else if (!GetFileInformationByHandle(h, &info)) {
        u32 why    = GetLastError();
        b32 device = why==ERROR_INVALID_FUNCTION ||
                     why==ERROR_INVALID_PARAMETER || why==ERROR_NOT_SUPPORTED;
        err = !(mode & OS_REGULAR) ? 0 : device ? OS_ENOTREG : OS_ERR;
    } else if ((mode & OS_ONELINK) && info.links>1) {
        err = OS_ELINKS;
    }
    if (err) {
        u32 why = GetLastError();
        CloseHandle(h);
        SetLastError(why);  // for os_error
        return err;
    }
    *out = h;
    return 0;
}

// Delete a file, or a link rather than its target, as POSIX unlink
// does. DeleteFileW refuses two of those. A read-only file: clear the
// attribute through a handle, mark the file deleted, then restore it,
// which the deletion survives, for any other hard links to the file. A
// junction or another directory link (reparse_link), which is a
// directory to DeleteFileW:
// mark it deleted through a handle to the link itself, which removes
// only the link, never its target nor what is in it. Each is checked
// through the handle, so that what is deleted is what was checked,
// whatever the name has become meanwhile, and a directory that is not a
// link, read-only or not, empty or not, is left alone, as is any other
// reparse point that is a directory (a cloud placeholder holds files).
// Returns whether the file was deleted, and if not, leaves why in the
// last error.
static b32 remove_file(os *ctx, c16 *wpath)
{
    if (DeleteFileW(wpath)) {
        return 1;
    }
    u32 why     = GetLastError();
    u32 attr    = GetFileAttributesW(wpath);
    u32 dirlink = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE;
    u32 rofile  = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY;
    b32 link    = attr!=INVALID_FILE_ATTRIBUTES && (attr&dirlink)==dirlink;
    b32 locked  = why==ERROR_ACCESS_DENIED && attr!=INVALID_FILE_ATTRIBUTES &&
                  (attr&rofile)==FILE_ATTRIBUTE_READONLY;
    if (!link && !locked) {
        SetLastError(why);  // as DeleteFileW said
        return 0;
    }

    // Only a directory link opens with backup semantics, so the handle
    // is never to a directory otherwise
    u32  flags = FILE_FLAG_OPEN_REPARSE;
    u32  want  = DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES;
    flags |= link ? FILE_FLAG_BACKUP_SEMANTICS : 0;
    iptr h = CreateFileW(wpath, want, FILE_SHARE_ALL, 0, OPEN_EXISTING,
                         flags, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    attribute_tag_info tag = {0};
    b32 ok = get_info(ctx, h, FileAttributeTagInfo, &tag, sizeof(tag));
    if (link) {
        ok = ok && (tag.attributes & FILE_ATTRIBUTE_DIRECTORY) &&
             reparse_link(tag);
    } else {
        ok = ok && (tag.attributes&rofile)==FILE_ATTRIBUTE_READONLY;
    }
    if (!ok) {
        CloseHandle(h);
        SetLastError(why);
        return 0;
    }

    b32 deleted = 0;
    u32 err     = 0;
    u8  discard = 1;
    basic_info info = {0};
    info.attributes = FILE_ATTRIBUTE_NORMAL;
    if (!(tag.attributes & FILE_ATTRIBUTE_READONLY)) {
        deleted = set_info(ctx, h, FileDispositionInfo, &discard, 1);
        err = GetLastError();
    } else if (set_info(ctx, h, FileBasicInfo, &info, sizeof(info))) {
        deleted = set_info(ctx, h, FileDispositionInfo, &discard, 1);
        err = GetLastError();
        if (!link || !deleted) {
            info.attributes = tag.attributes;
            set_info(ctx, h, FileBasicInfo, &info, sizeof(info));
        }
    } else {
        err = GetLastError();
    }
    CloseHandle(h);
    SetLastError(err);
    return deleted;
}

// Created files are marked delete-pending immediately, so that the file
// system removes them however the process ends, until kept: by os_keep
// (gzip), or by zip's os_commit, which clears it just before renaming.
static i32 open_output(os *ctx, i32 fd, c16 *wpath, i32 mode)
{
    u32 kept = 0;  // why a name to replace could not be removed
    if (mode & OS_FORCE && !remove_file(ctx, wpath)) {
        // Replace rather than write through a link
        kept = GetLastError();
        kept = kept==ERROR_FILE_NOT_FOUND ? 0 : kept;
    }
    iptr h = CreateFileW(wpath, GENERIC_WRITE|DELETE, 0, 0, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, 0);
    i32 unsure = ctx->unsure;
    ctx->unsure = 0;
    if (h == INVALID_HANDLE_VALUE) {
        // Forced, a name that could not be replaced is an error, for
        // the reason it could not be removed. Else a name that another
        // process holds delete-pending (its output not yet kept)
        // refuses access rather than reporting that it exists, and so
        // does checking for it. But where the directory refuses that
        // check for any name (no traverse rights), every name looks
        // taken, so after a run of 64 such names give up rather than
        // let a caller seeking a free name try them all.
        u32 err = GetLastError();
        if (mode & OS_FORCE) {
            if (err==ERROR_FILE_EXISTS && kept) {
                SetLastError(kept);
            }
            return OS_ERR;
        } else if (err==ERROR_ACCESS_DENIED || err==ERROR_SHARING_VIOLATION) {
            b32 found = GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES;
            if (found) {
                err = ERROR_FILE_EXISTS;
            } else if (GetLastError()==ERROR_ACCESS_DENIED && unsure<64) {
                ctx->unsure = unsure + 1;
                err = ERROR_FILE_EXISTS;
            }
            SetLastError(err);  // why it failed, rather than the check
        }
        return err==ERROR_FILE_EXISTS ? OS_EEXIST : OS_ERR;
    } else if (GetFileType(h) != FILE_TYPE_DISK) {
        CloseHandle(h);  // a device name (NUL): output is only ever a file
        return OS_ERR;
    }
    u8 discard = 1;
    if (!set_info(ctx, h, FileDispositionInfo, &discard, sizeof(discard))) {
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
        // Say why, as a failed CreateFileW would, for os_missing and the
        // like: a path that names nothing is a name no file can have
        SetLastError(wpath ? ERROR_TOO_MANY_OPEN_FILES : ERROR_INVALID_NAME);
        return OS_ERR;
    }

    if (mode & (OS_CREATE|OS_FORCE)) {
        return open_output(ctx, fd, wpath, mode);
    }
    iptr h = 0;
    i32 err = open_input(ctx, wpath, mode, &h);
    if (err) {
        // A wildcard that matched nothing stays as it is, as a POSIX
        // shell leaves it, and as there names no file, though Windows
        // calls a name with one invalid
        if (err==OS_ERR && ctx->wildnames && zip_haswild(path, 0) &&
            GetLastError()==ERROR_INVALID_NAME) {
            SetLastError(ERROR_FILE_NOT_FOUND);
        }
        return err;
    }
    ctx->handles[fd] = h;

    // A console opened by name (CON, CONIN$), a DOS device, is read as
    // standard input is when it is one
    u32 cmode = 0;
    b32 dosdev = wpath[0]=='\\' && wpath[1]=='\\' && wpath[2]=='.';
    ctx->consoles |= (dosdev && GetConsoleMode(h, &cmode) ? 1u : 0u) << fd;
    return fd;
}

static b32 os_close(os *ctx, i32 fd)
{
    b32 ok = CloseHandle(ctx->handles[fd]);
    ctx->handles[fd] = 0;
    ctx->consoles &= ~(1u << fd);
    return ok;
}

// Setting delete-pending just worked on this handle, so clearing it
// should too, but a file system or filter may yet refuse.
[[maybe_unused]] static b32 os_keep(os *ctx, i32 fd)
{
    u8 keep = 0;
    return set_info(ctx, ctx->handles[fd], FileDispositionInfo, &keep,
                    sizeof(keep));
}

// Read a console as UTF-16, converted to WTF-8, since ReadFile would
// give the bytes in the console's input code page: write_console's
// counterpart, so that names typed for zip -@ are UTF-8 like the rest.
// As ReadFile does, a read that begins with Ctrl+Z, as a line does when
// one ends input, is the end. A high surrogate at the end of a read waits
// for the low one that may follow, and bytes that do not fit for the
// next read.
static iz read_console(os *ctx, i32 fd, u8 *buf, iz cap)
{
    if (ctx->nrest) {
        iz n = MIN(cap, ctx->nrest);
        bytecopy(buf, ctx->rest, n);
        bytemove(ctx->rest, ctx->rest+n, ctx->nrest-n);
        ctx->nrest -= (u8)n;
        return n;
    } else if (cap < countof(ctx->rest)) {
        // Room for any code point and a surrogate held before it
        iz n = read_console(ctx, fd, ctx->rest, countof(ctx->rest));
        ctx->nrest = (u8)MAX(n, 0);
        return n>0 ? read_console(ctx, fd, buf, cap) : n;
    }

    for (;;) {
        c16 w[512];
        iz  n = ctx->high ? 1 : 0;
        w[0] = ctx->high;
        ctx->high = 0;
        u32 max = (u32)MIN(countof(w)-n, (cap - 3*n)/3);  // 3 bytes a unit
        u32 got = 0;
        if (!ReadConsoleW(ctx->handles[fd], w+n, max, &got, 0)) {
            return -1;
        } else if (!n && (!got || w[0]==0x1a)) {
            return 0;
        }
        n += got;
        if (got && w[n-1]>=0xd800 && w[n-1]<=0xdbff) {
            ctx->high = w[--n];
        }
        iz len = wtf8_encode(buf, w, n);
        if (len) {
            return len;
        }
    }
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    if (ctx->consoles>>fd & 1) {
        return read_console(ctx, fd, buf, cap);
    }
    iptr h = ctx->handles[fd];
    for (;;) {
        u32 got = 0;
        if (ReadFile(h, buf, (u32)MIN(cap, 1<<30), &got, 0)) {
            return got;
        }
        u32 err = GetLastError();
        if (err==ERROR_BROKEN_PIPE || err==ERROR_HANDLE_EOF) {
            return 0;
        } else if (err == ERROR_MORE_DATA) {
            return got;  // part of a longer message (message-mode pipe)
        }

        // An inherited pipe left non-blocking (PIPE_NOWAIT), as another
        // program may leave one, has no input yet: wait for input from
        // now on, as POSIX does for O_NONBLOCK, keeping its read mode
        u32 state = 0;
        if (err!=ERROR_NO_DATA ||
            !GetNamedPipeHandleStateW(h, &state, 0, 0, 0, 0, 0) ||
            !(state & PIPE_NOWAIT)) {
            SetLastError(err);
            return -1;
        }
        u32 mode = state & PIPE_READMODE_MESSAGE;  // and PIPE_WAIT
        if (!SetNamedPipeHandleState(h, &mode, 0, 0)) {
            SetLastError(err);
            return -1;
        }
    }
}

// Write UTF-8 (WTF-8) to a console as UTF-16, since WriteFile would take
// the bytes in the console's code page. An incomplete sequence at the
// end is held for the next write, as output may be split anywhere.
static b32 write_console(os *ctx, i32 fd, u8 *buf, iz len)
{
    u8  in[512];
    c16 out[countof(in)];  // at most one unit per byte
    iz  n = ctx->nheld[fd];
    bytecopy(in, ctx->held[fd], n);
    for (;;) {
        iz take = MIN(len, countof(in)-n);
        bytecopy(in+n, buf, take);
        buf += take;
        len -= take;
        n   += take;

        // Find an incomplete sequence at the end
        iz end = n;
        for (iz k = 1; k<=3 && k<=n; k++) {
            if ((in[n-k]&0xc0) != 0x80) {
                end -= wtf8_tail(in[n-k]) >= k ? k : 0;
                break;
            }
        }

        iz m = 0;
        for (iz i = 0; i < end;) {
            m = utf16_put(out, m, wtf8_decode(in, end, &i));
        }
        for (iz i = 0; i < m;) {
            u32 wrote = 0;
            if (!WriteConsoleW(ctx->handles[fd], out+i, (u32)(m-i),
                               &wrote, 0) || !wrote) {
                return 0;
            }
            i += wrote;
        }

        bytemove(in, in+end, n-end);
        n -= end;
        if (!len) {
            break;
        }
    }
    bytecopy(ctx->held[fd], in, n);
    ctx->nheld[fd] = (u8)n;
    return 1;
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    if ((u32)fd<3 && ctx->consoles>>fd & 1) {
        return write_console(ctx, fd, buf, len);
    }
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

static b32 os_isatty(os *ctx, i32 fd)
{
    return (u32)fd<3 && ctx->consoles>>fd & 1;
}

// Nothing holds it (zip's and unzip's standard output): Ctrl+C ends the
// process at once (an output file deleted on close), as no handler is
// installed, which would run on a thread of its own, beside the one
// filling the buffer. What is held is lost: a pipe's or file's buffer,
// or on a console, text that has waited less than 100 ms and a chunk of
// work.
[[maybe_unused]] static void os_holdtext(os *ctx, writer *w)
{
    (void)ctx;
    (void)w;
}

// To the system timer's 10-16 ms, wrapping after 49 days
static i64 os_now(os *ctx)
{
    (void)ctx;
    return GetTickCount();
}

// Nothing there: no such file, nor a directory on the way to it, rather
// than a share or server not found, which one that is down may give, or
// a name that no file can have.
static b32 os_missing(os *ctx)
{
    (void)ctx;
    u32 err = GetLastError();
    return err==ERROR_FILE_NOT_FOUND || err==ERROR_PATH_NOT_FOUND;
}

// Why the last system call failed, as the C runtime of Info-ZIP's port,
// and of GNU gzip built for Windows, would say: in the words of its
// strerror for the errno to which its _dosmaperr maps the code (checked
// against msvcrt, and the UCRT, which differs only in mapping 1113 to
// EILSEQ), or an empty string if none failed. Beyond its table, a disk
// full through a handle (39) is a full disk too, and a pipe being closed
// (232), which gzip takes for a closed pipe, a broken one.
static s8 os_error(os *ctx)
{
    (void)ctx;
    u32 err = GetLastError();
    switch (err) {
    case 0:
        return S("");
    case 2: case 3: case 15: case 18: case 53: case 67: case 161: case 206:
        return S("No such file or directory");
    case 4:
        return S("Too many open files");
    case 5: case 16: case 65: case 82: case 83: case 108: case 132:
    case 158: case 167:
        return S("Permission denied");
    case 6: case 114: case 130:
        return S("Bad file descriptor");
    case 7: case 8: case 9: case 1816:
        return S("Not enough space");
    case 10:
        return S("Arg list too long");
    case 11:
        return S("Exec format error");
    case 17:
        return S("Improper link");
    case 39: case 112:
        return S("No space left on device");
    case 80: case 183:
        return S("File exists");
    case 89: case 164: case 215:
        return S("Resource temporarily unavailable");
    case 109: case 232:
        return S("Broken pipe");
    case 128: case 129:
        return S("No child processes");
    case 145:
        return S("Directory not empty");
    }
    return err>=19 && err<=36   ? S("Permission denied")  // sharing, locks
         : err>=188 && err<=202 ? S("Exec format error")
         : S("Invalid argument");
}

// As unlink, even of a read-only file (remove_file).
[[maybe_unused]] static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);  // as in os_open
        return 0;
    }
    return remove_file(ctx, wpath);
}

// Unix seconds from a FILETIME, rounding down.
static i64 unixtime(u32 const ft[2])
{
    i64 t = (i64)((u64)ft[1]<<32 | ft[0]) - 116444736000000000;
    return t>=0 ? t/10000000 : -((-t + 9999999)/10000000);
}

// The first entry of a listing, which skips short names, fetching many
// entries at a time, with Windows 7's FindExInfoBasic and
// FIND_FIRST_EX_LARGE_FETCH. Older versions refuse both as an invalid
// parameter: then list as they do, from then on once that works.
static iptr find_first(os *ctx, c16 *pattern, find_data *fd)
{
    if (ctx->api.oldfind) {
        return FindFirstFileExW(pattern, 0, fd, 0, 0, 0);
    }
    iptr h = INVALID_HANDLE_VALUE;
    if (FORCE_XP) {
        SetLastError(ERROR_INVALID_PARAMETER);
    } else {
        h = FindFirstFileExW(pattern, 1, fd, 0, 0, FIND_FIRST_EX_LARGE_FETCH);
    }
    if (h==INVALID_HANDLE_VALUE && GetLastError()==ERROR_INVALID_PARAMETER) {
        h = FindFirstFileExW(pattern, 0, fd, 0, 0, 0);
        ctx->api.oldfind |= h!=INVALID_HANDLE_VALUE ||
                            GetLastError()!=ERROR_INVALID_PARAMETER;
    }
    return h;
}

// Hidden and system entries are judged by the attributes in the listing,
// which are a link's own, as Info-ZIP does, and which need no handle to
// the file (some, like pagefile.sys, cannot be opened at all). Plain
// files and directories are described from the listing too, so that
// scanning opens only those whose identity it needs. Links, and other
// reparse points, are left to os_stat, which follows them. A directory
// entry's size and times can lag for a file changed through another of
// its hard links, as Microsoft documents, where a handle's would not.
// The search pattern, dead once the search begins, is overwritten by the
// listing, so that a deep tree's directories do not each keep theirs.
// Each entry is first packed after the last, its name, a terminator, and
// what the listing tells of it, then entries are made for them all at
// once: an array doubled as it grew would, in an arena that grows down
// (zip's scratch), or in one where names follow it (gzip's), leave each
// smaller copy behind, some 265 bytes a name in all on x86-64, where
// this takes 117 and the name.
static os_dirent *os_listdir(os *ctx, s8 path, b32 all, iz *count,
                             arena *a)
{
    arena tmp   = *a;
    c16  *wpath = winpath(&tmp, path);
    if (!wpath) {
        return 0;
    }
    s16 dir = s16lit(wpath);
    b32 sep = dir.len && dir.s[dir.len-1]=='\\';
    c16 *pattern = s16cat(&tmp, dir, s16lit(sep ? L"*" : L"\\*"));

    typedef struct {
        u32 attributes;
        u32 written[2], accessed[2];
        u32 size_hi, size_lo;
    } found;
    u8 *first = (u8 *)(a->down ? a->end : a->beg);  // where entries begin
    u8 *last  = first;                              // and end
    iz  n     = 0;
    find_data fd = {0};
    iptr h = find_first(ctx, pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // Nothing matched: an empty directory without . and .., such as
        // an empty drive's root. A missing directory is PATH_NOT_FOUND.
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return 0;
        }
    } else {
        u32 skip = all ? 0 : FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM;
        do {
            c16 *w    = fd.name;
            b32  dots = w[0]=='.' && (!w[1] || (w[1]=='.' && !w[2]));
            if ((fd.attributes & skip) || dots) {
                continue;
            }
            iz wlen = 0;
            for (; w[wlen]; wlen++) {}
            iz    len  = wtf8_encode(0, w, wlen);
            iz    size = len + 1 + (iz)sizeof(found);
            u8   *e    = newstr(a, size);
            found f    = {
                fd.attributes, {fd.written[0], fd.written[1]},
                {fd.accessed[0], fd.accessed[1]}, fd.size_hi, fd.size_lo
            };
            assert(a->down ? e+size==last : e==last);  // packed
            wtf8_encode(e, w, wlen);
            e[len] = 0;
            bytecopy(e+len+1, &f, sizeof(f));
            last = a->down ? e : e+size;
            n++;
        } while (FindNextFileW(h, &fd));
        b32 done = GetLastError() == ERROR_NO_MORE_FILES;
        FindClose(h);
        if (!done) {
            return 0;  // not a partial listing
        }
    }

    os_dirent *list = new(a, n, os_dirent);  // type FT_NONE
    u8        *p    = MIN(first, last);
    for (iz i = 0; i < n; i++) {
        os_dirent *e = list + i;
        e->name.s = p;
        for (; p[e->name.len]; e->name.len++) {}
        found f;
        bytecopy(&f, p+e->name.len+1, sizeof(f));
        p += e->name.len + 1 + (iz)sizeof(f);
        if (!(f.attributes & FILE_ATTRIBUTE_REPARSE)) {
            b32 isdir = f.attributes & FILE_ATTRIBUTE_DIRECTORY;
            e->info.type  = isdir ? FT_DIR : FT_FILE;
            e->info.size  = (i64)((u64)f.size_hi<<32 | f.size_lo);
            e->info.mtime = unixtime(f.written);
            e->info.atime = unixtime(f.accessed);
            e->info.attr  = f.attributes;
        }
    }
    *count = n;
    return list;
}

// Upper case by the system's "file system rules", its default without
// LCMAP_LINGUISTIC_CASING, which map each UTF-16 unit to one, as file
// names are compared ignoring case: the case that the C runtime's
// towupper, which Info-ZIP's port matches wildcards with, gives too.
static s8 os_upcase(os *ctx, s8 name, arena *a)
{
    (void)ctx;
    s16  w = fromwtf8(a, name);
    c16 *u = new(a, w.len+1, c16);
    i32  n = LCMapStringW(LOCALE_INVARIANT, LCMAP_UPPERCASE, w.s, (i32)w.len,
                          u, (i32)w.len);
    if (n != w.len) {
        return name;
    }
    u[n] = 0;
    return towtf8(a, u);
}

// Created files not kept are delete-pending, so they need no cleanup.
// An incomplete UTF-8 sequence still held for a console ended the
// output, which makes it invalid: write it out as U+FFFD.
static void os_exit(os *ctx, i32 status)
{
    for (i32 fd = 1; fd < 3; fd++) {
        if (ctx->nheld[fd]) {
            c16 bad = 0xfffd;
            u32 wrote = 0;
            WriteConsoleW(ctx->handles[fd], &bad, 1, &wrote, 0);
            ctx->nheld[fd] = 0;
        }
    }
    ExitProcess((u32)status);
}

// Initialize standard handles, and find the functions XP lacks.
static void os_init(os *ctx)
{
    load_api(&ctx->api);
    ctx->handles[0] = GetStdHandle((u32)-10);
    ctx->handles[1] = GetStdHandle((u32)-11);
    ctx->handles[2] = GetStdHandle((u32)-12);
    for (i32 fd = 0; fd < 3; fd++) {
        u32 mode;
        b32 console = GetConsoleMode(ctx->handles[fd], &mode);
        ctx->consoles |= (console ? 1u : 0u) << fd;
    }
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
