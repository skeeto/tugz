// CRT-free Win32 platform layer for tugz zip
// $ cc -O2 -nostartfiles -o zip.exe platform/zip_windows.c -lmemory
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/deflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/zipcli.c"

#include "windows.c"

typedef struct {
    u32 attributes;
    u32 created[2], accessed[2], written[2];
    u32 size_hi, size_lo;
    u32 reserved[2];
    c16 name[260];
    c16 altname[14];
} find_data;

typedef struct {
    uptr internal, internal_high;
    u32  offset, offset_high;
    uptr event;
} overlapped;

typedef struct {
    u16 year, month, weekday, day, hour, minute, second, ms;
} systemtime;

typedef struct {
    u32  flags;  // FileRenameInfoEx, else a BOOLEAN ReplaceIfExists
    iptr root;
    u32  len;    // bytes
    c16  name[];
} rename_info;

typedef struct {
    u64 volume;
    u64 id[2];  // 128 bits
} file_id_info;

W32(b32)  FindClose(iptr);
W32(iptr) FindFirstFileExW(c16 *, i32, find_data *, i32, uptr, u32);
W32(b32)  FindNextFileW(iptr, find_data *);
W32(b32)  FlushFileBuffers(iptr);
W32(u32)  GetEnvironmentVariableW(c16 *, c16 *, u32);
W32(void) SetLastError(u32);
W32(b32)  SystemTimeToTzSpecificLocalTime(uptr, systemtime *, systemtime *);

#define FILE_ATTRIBUTE_HIDDEN      0x02u
#define FILE_ATTRIBUTE_SYSTEM      0x04u
#define FILE_READ_ATTRIBUTES       0x80u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_TYPE_UNKNOWN          0u
#define FIND_FIRST_EX_LARGE_FETCH  2u
#define FILE_RENAME_REPLACE        1u
#define FILE_RENAME_POSIX          2u
#define ERROR_INVALID_FUNCTION     1u
#define ERROR_FILE_NOT_FOUND       2u
#define ERROR_NO_MORE_FILES        18u
#define ERROR_NOT_SUPPORTED        50u
#define ERROR_ENVVAR_NOT_FOUND     203u

enum {
    FileRenameInfo    = 3,
    FileEndOfFileInfo = 6,
    FileIdInfo        = 18,
    FileRenameInfoEx  = 22,
};

// Unix seconds from a FILETIME, rounding down.
static i64 unixtime(u32 const ft[2])
{
    i64 t = (i64)((u64)ft[1]<<32 | ft[0]) - 116444736000000000;
    return t>=0 ? t/10000000 : -((-t + 9999999)/10000000);
}

// Symbolic links and junctions are always followed, as Info-ZIP does on
// Windows, so follow is ignored.
static b32 os_stat(os *ctx, s8 path, b32 follow, os_info *info,
                   arena scratch)
{
    (void)ctx;
    (void)follow;
    c16 *wpath = winpath(&scratch, path);
    if (!wpath) {
        return 0;
    }
    iptr h = CreateFileW(wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    by_handle_info bh = {0};
    file_id_info   id = {0};
    b32 ok   = GetFileInformationByHandle(h, &bh);
    b32 isid = GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id));
    u32 type = GetFileType(h);
    CloseHandle(h);
    if (!ok && (type==FILE_TYPE_DISK || type==FILE_TYPE_UNKNOWN)) {
        return 0;
    }

    // The 64-bit index is not unique on ReFS, so prefer the 128-bit ID,
    // available from Windows 8 where the file system supports it. An ID
    // of all zero or all one bits is unknown.
    if (!isid) {
        id.volume = bh.volume;
        id.id[0]  = (u64)bh.index_hi<<32 | bh.index_lo;
        id.id[1]  = 0;
    }
    if (id.id[0]==(u64)-1 && (id.id[1]==(u64)-1 || !id.id[1])) {
        id.id[0] = id.id[1] = 0;
    }

    // Only files and directories on disk are archived. A device (NUL),
    // even one that answers no queries, or the pipe namespace, is special.
    b32 dir = bh.attributes & FILE_ATTRIBUTE_DIRECTORY;
    info->type   = type!=FILE_TYPE_DISK ? FT_OTHER : dir ? FT_DIR : FT_FILE;
    info->size   = (i64)((u64)bh.size_hi<<32 | bh.size_lo);
    info->mtime  = unixtime(bh.written);
    info->atime  = unixtime(bh.accessed);
    info->mode   = 0;
    info->attr   = bh.attributes;
    info->uid    = 0;
    info->gid    = 0;
    info->dev    = id.volume;
    info->ino[0] = id.id[0];
    info->ino[1] = id.id[1];
    return 1;
}

// Hidden and system entries are judged by the attributes in the listing,
// which are a link's own, as Info-ZIP does, and which need no handle to
// the file (some, like pagefile.sys, cannot be opened at all). Plain
// files and directories are described from the listing too, so that
// scanning opens only those whose identity it needs. Links, and other
// reparse points, are left to os_stat, which follows them. A directory
// entry's size and times can lag for a file changed through another of
// its hard links, as Microsoft documents, where a handle's would not.
static os_dirent *os_listdir(os *ctx, s8 path, b32 all, iz *count,
                             arena *perm, arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    if (!wpath) {
        return 0;
    }
    s16 dir = s16lit(wpath);
    b32 sep = dir.len && dir.s[dir.len-1]=='\\';
    c16 *pattern = s16cat(&scratch, dir, s16lit(sep ? L"*" : L"\\*"));

    os_dirents list = {0};
    find_data fd = {0};
    iptr h = FindFirstFileExW(pattern, 1, &fd, 0, 0,
                              FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        // Nothing matched: an empty directory without . and .., such as
        // an empty drive's root. A missing directory is PATH_NOT_FOUND.
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return 0;
        }
    } else {
        u32 skip = all ? 0 : FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM;
        do {
            if (fd.attributes & skip) {
                continue;
            }
            s8 name = towtf8(perm, fd.name);
            if (zequals(name, S(".")) || zequals(name, S(".."))) {
                continue;
            }
            os_dirent *e = push(perm, &list);
            *e = (os_dirent){name, {0}};  // type FT_NONE
            if (!(fd.attributes & FILE_ATTRIBUTE_REPARSE)) {
                b32 isdir = fd.attributes & FILE_ATTRIBUTE_DIRECTORY;
                e->info.type  = isdir ? FT_DIR : FT_FILE;
                e->info.size  = (i64)((u64)fd.size_hi<<32 | fd.size_lo);
                e->info.mtime = unixtime(fd.written);
                e->info.atime = unixtime(fd.accessed);
                e->info.attr  = fd.attributes;
            }
        } while (FindNextFileW(h, &fd));
        b32 done = GetLastError() == ERROR_NO_MORE_FILES;
        FindClose(h);
        if (!done) {
            return 0;  // not a partial listing
        }
    }
    *count = list.len;
    return list.data ? list.data : new(perm, 1, os_dirent);
}

static s8 os_readlink(os *ctx, s8 path, arena *perm, arena scratch)
{
    (void)ctx;
    (void)path;
    (void)perm;
    (void)scratch;
    return (s8){0};  // never asked: links are followed
}

static b32 os_readat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    while (len) {
        overlapped ov = {0};
        ov.offset      = (u32)off;
        ov.offset_high = (u32)((u64)off >> 32);
        u32 got = 0;
        u32 n   = (u32)MIN(len, 1<<30);
        if (!ReadFile(ctx->handles[fd], buf, n, &got, (uptr)&ov) || !got) {
            return 0;
        }
        buf += got;
        len -= got;
        off += got;
    }
    return 1;
}

static b32 os_writeat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    while (len) {
        overlapped ov = {0};
        ov.offset      = (u32)off;
        ov.offset_high = (u32)((u64)off >> 32);
        u32 wrote = 0;
        u32 n     = (u32)MIN(len, 1<<30);
        if (!WriteFile(ctx->handles[fd], buf, n, &wrote, (uptr)&ov) ||
            !wrote) {
            return 0;
        }
        buf += wrote;
        len -= wrote;
        off += wrote;
    }
    return 1;
}

static b32 os_truncate(os *ctx, i32 fd, i64 len)
{
    return SetFileInformationByHandle(ctx->handles[fd], FileEndOfFileInfo,
                                      &len, sizeof(len));
}

// Rename by handle while the file is still open, replacing the target,
// so that it is never visible incomplete under its final name. POSIX
// semantics replace a target that others hold open with delete sharing
// (scanners, indexers), which the classic rename refuses. Flush first,
// while the file is still delete-pending, since deferred write errors
// (network, quotas) may surface only then. Once renamed, the archive is
// replaced, and closing, after the flush, can lose nothing.
static b32 os_commit(os *ctx, i32 fd, s8 path, arena scratch)
{
    iptr h = ctx->handles[fd];
    c16 *wpath = winpath(&scratch, path);
    b32  ok    = wpath && FlushFileBuffers(h);
    if (wpath && !ok) {
        // As POSIX accepts EINVAL and ENOTSUP from fsync: a file system
        // that cannot flush (some network and virtual ones) defers nothing
        u32 err = GetLastError();
        ok = err==ERROR_INVALID_FUNCTION || err==ERROR_NOT_SUPPORTED;
    }
    if (!ok) {
        os_close(ctx, fd);
        return 0;
    }
    s16 name = s16lit(wpath);
    iz  size = (iz)sizeof(rename_info) + (name.len+1)*(iz)sizeof(c16);
    rename_info *ri = (rename_info *)newbytes(&scratch, size);
    bytefill(ri, 0, size);
    ri->flags = FILE_RENAME_REPLACE | FILE_RENAME_POSIX;
    ri->len   = (u32)(name.len * (iz)sizeof(c16));
    bytecopy(ri->name, name.s, name.len*(iz)sizeof(c16));

    u8 keep = 0;
    if (!SetFileInformationByHandle(h, FileDispositionInfo, &keep, 1)) {
        os_close(ctx, fd);
        return 0;
    }
    if (!SetFileInformationByHandle(h, FileRenameInfoEx, ri, (u32)size)) {
        // Older Windows, or a file system without POSIX semantics
        // (FAT, some SMB servers)
        ri->flags = 1;  // ReplaceIfExists
        if (!SetFileInformationByHandle(h, FileRenameInfo, ri, (u32)size)) {
            u8 discard = 1;
            SetFileInformationByHandle(h, FileDispositionInfo, &discard, 1);
            os_close(ctx, fd);
            return 0;
        }
    }
    os_close(ctx, fd);
    return 1;
}

static b32 os_isatty(os *ctx, i32 fd)
{
    return (u32)fd<3 && ctx->consoles>>fd & 1;
}

// An environment variable as WTF-8, or a null string if it is unset.
static s8 getenv8(arena *a, c16 *name)
{
    SetLastError(0);  // zero is also the length of an empty value
    u32 cap = GetEnvironmentVariableW(name, 0, 0);  // including the null
    if (!cap) {
        b32 unset = GetLastError() == ERROR_ENVVAR_NOT_FOUND;
        return unset ? (s8){0} : S("");
    }
    c16 *buf = new(a, cap, c16);
    u32  len = GetEnvironmentVariableW(name, buf, cap);
    buf[len<cap ? len : 0] = 0;
    return towtf8(a, buf);
}

static void os_localtime(os *ctx, i64 t, i32 tm[6])
{
    (void)ctx;
    zip_gmtime(t, tm);
    if (tm[0]<1601 || tm[0]>30827) {
        return;
    }
    i64 days = t/86400 - (t%86400 < 0);
    systemtime utc = {0};
    utc.year    = (u16)tm[0];
    utc.month   = (u16)tm[1];
    utc.weekday = (u16)(((days + 4)%7 + 7) % 7);  // 1970-01-01: Thursday
    utc.day     = (u16)tm[2];
    utc.hour    = (u16)tm[3];
    utc.minute  = (u16)tm[4];
    utc.second  = (u16)tm[5];
    systemtime loc = {0};
    if (SystemTimeToTzSpecificLocalTime(0, &utc, &loc)) {
        tm[0] = loc.year;
        tm[1] = loc.month;
        tm[2] = loc.day;
        tm[3] = loc.hour;
        tm[4] = loc.minute;
        tm[5] = loc.second;
    }
}

void mainCRTStartup(void)
{
    os ctx = {0};
    zipconfig conf = {0};
    conf.perm    = os_init(&ctx, (iz)1 << 28);
    conf.windows = 1;

    i32 argc = 0;
    s8 *argv = os_args(&conf.perm, &argc);
    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = argv + (argc>0);

    conf.epoch  = getenv8(&conf.perm, L"SOURCE_DATE_EPOCH");
    conf.zipopt = getenv8(&conf.perm, L"ZIPOPT");
    conf.zipenv = getenv8(&conf.perm, L"ZIP");
    os_exit(&ctx, zip_main(&conf));
}
