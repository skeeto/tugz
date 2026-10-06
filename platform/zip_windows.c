// CRT-free Win32 platform layer for tugz zip
// $ cc -O2 -nostartfiles -o zip.exe platform/zip_windows.c -lmemory
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/deflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/wild.c"
#include "../src/dir.c"
#include "../src/zipcli.c"

#include "windows.c"

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

W32(b32)  FlushFileBuffers(iptr);
W32(u32)  GetEnvironmentVariableW(c16 *, c16 *, u32);
W32(u32)  GetFinalPathNameByHandleW(iptr, c16 *, u32, u32);
W32(i32)  MultiByteToWideChar(u32, u32, u8 const *, i32, c16 *, i32);
W32(b32)  SystemTimeToTzSpecificLocalTime(uptr, systemtime *, systemtime *);

#define CP_ACP                     0u
#define CP_OEMCP                   1u
#define MB_ERR_INVALID_CHARS       0x8u
#define FILE_ATTRIBUTE_ARCHIVE     0x20u
#define FILE_ATTRIBUTE_NOT_INDEXED 0x2000u
#define FILE_READ_ATTRIBUTES       0x80u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_FLAG_DELETE_ON_CLOSE  0x04000000u
#define FILE_TYPE_UNKNOWN          0u
#define FILE_RENAME_REPLACE        1u
#define FILE_RENAME_POSIX          2u
#define ERROR_PATH_NOT_FOUND       3u
#define ERROR_LOCK_VIOLATION       33u
#define ERROR_DISK_FULL            112u
#define ERROR_ALREADY_EXISTS       183u
#define ERROR_ENVVAR_NOT_FOUND     203u

enum {
    FileRenameInfo    = 3,
    FileEndOfFileInfo = 6,
    FileIdInfo        = 18,
    FileRenameInfoEx  = 22,
};

// zip's memory is one reserved range of address space, committed a
// chunk at a time as it is used, so that the commit charge grows with
// use: perm from the bottom up, and scratch from the top down. As much
// as the system will reserve, up to 64 GiB (1 GiB in 32-bit processes),
// halving on refusal.
static void reserve(os *ctx)
{
    iz cap = (iz)1 << (sizeof(void *)==8 ? 36 : 30);
    for (; cap >= (iz)1<<24; cap /= 2) {
        byte *p = VirtualAlloc(0, cap, MEM_RESERVE, PAGE_READWRITE);
        if (p) {
            ctx->lo = p;
            ctx->hi = p + cap;
            return;
        }
    }
    os_oom(ctx);
}

// Commit more of the reservation to perm, from below the middle, or to
// a scratch arena, from above it, a megabyte at a time. Scratch below
// the arena asking is free, since the functions that allocated it have
// returned. Any other arena, such as a codec's exact one, is fixed.
static void os_extend(os *ctx, arena *a, iz need)
{
    if (a->down) {
        a->beg = ctx->hi;
    } else if (a->end != ctx->lo) {
        os_oom(ctx);
    }
    iz want = need - (a->end - a->beg);
    iz room = ctx->hi - ctx->lo;
    if (want > room) {
        os_oom(ctx);
    } else if (want > 0) {
        iz    chunk = (iz)1 << 20;
        iz    take  = MIN((want + chunk - 1) & -chunk, room);
        byte *at    = a->down ? ctx->hi-take : ctx->lo;
        if (!VirtualAlloc((uptr)at, take, MEM_COMMIT, PAGE_READWRITE)) {
            os_oom(ctx);  // the system's commit limit
        }
        if (a->down) {
            ctx->hi = at;
            a->beg  = at;
        } else {
            ctx->lo = at + take;
            a->end  = ctx->lo;
        }
    }
}

static b32 handle_info(iptr h, os_info *info)
{
    by_handle_info bh = {0};
    file_id_info   id = {0};
    b32 ok   = GetFileInformationByHandle(h, &bh);
    b32 isid = GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof(id));
    u32 type = GetFileType(h);
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

// Symbolic links and junctions are followed, as Info-ZIP does on Windows
// (there is no -y), unless asked not to, as for the directories along an
// entry's name, when a link or a junction is FT_LINK. Other reparse
// points, such as cloud placeholders, are ordinary files.
static b32 os_stat(os *ctx, s8 path, b32 follow, os_info *info,
                   arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);  // no file can have it
        return 0;
    }
    u32 flags = FILE_FLAG_BACKUP_SEMANTICS;
    flags |= follow ? 0 : FILE_FLAG_OPEN_REPARSE;
    iptr h = CreateFileW(wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                         OPEN_EXISTING, flags, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    b32 ok = handle_info(h, info);
    attribute_tag_info tag = {0};
    if (ok && !follow && (info->attr & FILE_ATTRIBUTE_REPARSE) &&
        GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag,
                                     sizeof(tag)) &&
        (tag.reparse_tag==IO_REPARSE_TAG_SYMLINK ||
         tag.reparse_tag==IO_REPARSE_TAG_MOUNT_POINT)) {
        info->type = FT_LINK;
    }
    CloseHandle(h);
    return ok;
}

// A name that no file could have, as one with a character Windows does
// not allow (<, |), is missing, as creating the archive fails anyway.
// Not so a network path or name not found, which a server down may give.
static b32 os_missing(os *ctx)
{
    (void)ctx;
    u32 err = GetLastError();
    return err==ERROR_FILE_NOT_FOUND || err==ERROR_PATH_NOT_FOUND ||
           err==ERROR_INVALID_NAME;
}

static b32 os_fstat(os *ctx, i32 fd, os_info *info)
{
    return handle_info(ctx->handles[fd], info);
}

static s8 os_readlink(os *ctx, s8 path, arena *a)
{
    (void)ctx;
    (void)path;
    (void)a;
    return (s8){0};  // never asked: links are followed
}

// The final path of an open file, a \\?\ path, with its volume named
// one way: as a drive letter path (DOS), by its GUID, or by its NT
// device. Null if the system has no such name for it.
enum { VOLUME_NAME_DOS, VOLUME_NAME_GUID, VOLUME_NAME_NT };
static c16 *final_path(iptr h, u32 how, arena *a)
{
    u32  cap = GetFinalPathNameByHandleW(h, 0, 0, how);  // including the null
    c16 *buf = cap ? new(a, cap, c16) : 0;
    u32  len = buf ? GetFinalPathNameByHandleW(h, buf, cap, how) : 0;
    return len && len<cap ? buf : 0;
}

// A link (or junction) at the end of the path is followed by opening the
// file it leads to, which the system finds, and asking for its final
// path. A dangling link is followed as writing through it would be: by
// creating its target, which is discarded at once. Where the system has
// no drive letter path for it (on a volume mounted nowhere, at least),
// its volume is named by GUID, and if the mount manager does not know
// the volume (a RAM disk), by its device, under \\?\GLOBALROOT. Since
// messages name files beside the archive, a target within the directory
// where the user named the archive is named from there, and otherwise,
// a drive or share path loses its \\?\ (winpath restores it).
static s8 os_resolve(os *ctx, s8 path, arena *perm, arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    u32  attr  = wpath ? GetFileAttributesW(wpath) : INVALID_FILE_ATTRIBUTES;
    if (attr==INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_REPARSE)) {
        return path;  // no link
    }
    iptr h = CreateFileW(wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(wpath, DELETE, FILE_SHARE_ALL, 0, CREATE_NEW,
                        FILE_FLAG_DELETE_ON_CLOSE, 0);
    }
    if (h == INVALID_HANDLE_VALUE) {
        return (s8){0};  // e.g. a loop
    }
    u32  how    = VOLUME_NAME_DOS;
    c16 *target = final_path(h, how, &scratch);
    while (!target && how<VOLUME_NAME_NT) {
        target = final_path(h, ++how, &scratch);
    }
    CloseHandle(h);
    if (!target) {
        return (s8){0};
    }

    // The directory as named, which for "X:name" is "X:"
    iz cut = path.len;
    for (; cut>0 && path.s[cut-1]!='/' && path.s[cut-1]!='\\'; cut--) {}
    u8 drive = path.len>2 ? (u8)(path.s[0] | 0x20) : 0;
    cut = !cut && drive>='a' && drive<='z' && path.s[1]==':' ? 2 : cut;
    s8   dir  = {path.s, cut};
    c16 *wdir = winpath(&scratch, cut ? dir : S("."));
    iptr dh   = !wdir ? INVALID_HANDLE_VALUE :
                CreateFileW(wdir, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    s16 base = {0};
    if (dh != INVALID_HANDLE_VALUE) {
        c16 *final = final_path(dh, how, &scratch);
        base = final ? s16lit(final) : base;
        CloseHandle(dh);
    }

    s16 t      = s16lit(target);
    b32 sep    = base.len && base.s[base.len-1]=='\\';  // a root
    iz  len    = base.len + !sep;  // with the separator after it
    b32 within = base.len && len<t.len && (sep || t.s[base.len]=='\\');
    for (iz i = 0; within && i<base.len; i++) {
        within = t.s[i] == base.s[i];
    }
    if (within) {
        return JOIN(perm, dir, towtf8(&scratch, t.s+len));
    } else if (how == VOLUME_NAME_NT) {
        return JOIN(perm, S("\\\\?\\GLOBALROOT"), towtf8(&scratch, t.s));
    } else if (how == VOLUME_NAME_DOS) {
        b32 unc = t.s[4]=='U' && t.s[5]=='N' && t.s[6]=='C' && t.s[7]=='\\';
        t.s += unc ? 6 : 4;  // "\\?\UNC\server" to "\\server"
        t.s[0] = unc ? '\\' : t.s[0];
    }
    return towtf8(perm, t.s);
}

// The file's final path, by which file systems that report no file IDs
// still tell files apart, following links as os_stat does.
static s8 os_fullpath(os *ctx, s8 path, arena *perm, arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    iptr h     = !wpath ? INVALID_HANDLE_VALUE :
                 CreateFileW(wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return (s8){0};
    }
    c16 *full = 0;
    for (u32 how = VOLUME_NAME_DOS; !full && how<=VOLUME_NAME_NT; how++) {
        full = final_path(h, how, &scratch);
    }
    CloseHandle(h);
    return full ? towtf8(perm, full) : (s8){0};
}

static void release_guard(os *ctx)
{
    if (ctx->guard) {
        CloseHandle(ctx->guard);
        ctx->guard = 0;
    }
}

// Whether the archive may be replaced, found before any work, since the
// rename that replaces it would refuse only after all of it: not if it
// is read-only, which Info-ZIP's port cannot open to update either, nor
// if another process holds it open without sharing delete access, as
// the rename needs, which Info-ZIP's port finds only at the end. Hold it
// with that access, sharing all, until os_commit, so that no process can
// open it so in the meantime. Each fails as that open would.
static b32 os_writable(os *ctx, s8 path, arena scratch)
{
    c16 *wpath = winpath(&scratch, path);
    u32  attr  = wpath ? GetFileAttributesW(wpath) : INVALID_FILE_ATTRIBUTES;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        return 1;
    } else if (attr & FILE_ATTRIBUTE_READONLY) {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    iptr h = CreateFileW(wpath, DELETE, FILE_SHARE_ALL, 0, OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    release_guard(ctx);
    ctx->guard = h;
    return 1;
}

static i32 os_readat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    while (len) {
        overlapped ov = {0};
        ov.offset      = (u32)off;
        ov.offset_high = (u32)((u64)off >> 32);
        u32 got = 0;
        u32 n   = (u32)MIN(len, 1<<30);
        if (!ReadFile(ctx->handles[fd], buf, n, &got, (uptr)&ov)) {
            return GetLastError()==ERROR_HANDLE_EOF ? 0 : -1;
        } else if (!got) {
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

// Rename by handle while the file is still open, replacing the target
// if asked, so that it is never visible incomplete under its final name
// (the rename refuses a target that is not to be replaced). POSIX
// semantics replace a target that others hold open with delete sharing
// (scanners, indexers), which the classic rename refuses. Flush first,
// while the file is still delete-pending, since deferred write errors
// (network, quotas) may surface only then: closing, the POSIX layer's
// check, comes after the rename. Once renamed, the archive is replaced,
// and closing, after the flush, can lose nothing.
static i32 os_commit(os *ctx, i32 fd, s8 temp, s8 path, b32 replace,
                     arena scratch)
{
    iptr h = ctx->handles[fd];
    if (!FlushFileBuffers(h)) {
        // A file system that cannot flush (some network and virtual ones)
        // defers nothing
        u32 err = GetLastError();
        if (err!=ERROR_INVALID_FUNCTION && err!=ERROR_NOT_SUPPORTED) {
            os_close(ctx, fd);  // still delete-pending
            SetLastError(err);
            return COMMIT_ECLOSE;
        }
    }
    c16 *wpath = winpath(&scratch, path);
    c16 *wtemp = winpath(&scratch, temp);
    if (!wpath || !wtemp) {
        os_close(ctx, fd);
        SetLastError(ERROR_INVALID_NAME);
        return COMMIT_EREPLACE;
    }

    // Keep a replaced archive's attributes, as POSIX keeps its mode:
    // hidden, system, and not indexed. Read-only was refused, and the
    // archive bit stays set, as for any changed file. Access control is
    // inherited from the directory, as for any new file, since copying
    // the old would take advapi32.
    u32 old  = GetFileAttributesW(wpath);
    u32 kept = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
               FILE_ATTRIBUTE_NOT_INDEXED;
    if (old!=INVALID_FILE_ATTRIBUTES && (old & kept)) {
        basic_info info = {0};
        info.attributes = (old & kept) | FILE_ATTRIBUTE_ARCHIVE;
        SetFileInformationByHandle(h, FileBasicInfo, &info, sizeof(info));
    }

    s16 name = s16lit(wpath);
    iz  size = (iz)sizeof(rename_info) + (name.len+1)*(iz)sizeof(c16);
    rename_info *ri = (rename_info *)newbytes(&scratch, size);
    bytefill(ri, 0, size);
    ri->flags = FILE_RENAME_POSIX | (replace ? FILE_RENAME_REPLACE : 0);
    ri->len   = (u32)(name.len * (iz)sizeof(c16));
    bytecopy(ri->name, name.s, name.len*(iz)sizeof(c16));

    u8 keep = 0;
    if (!SetFileInformationByHandle(h, FileDispositionInfo, &keep, 1)) {
        u32 err = GetLastError();
        os_close(ctx, fd);  // still delete-pending
        SetLastError(err);
        return COMMIT_EREPLACE;
    }
    if (!SetFileInformationByHandle(h, FileRenameInfoEx, ri, (u32)size)) {
        // Older Windows, or a file system without POSIX semantics
        // (FAT, some SMB servers), which also refuses while os_writable
        // holds the archive
        release_guard(ctx);
        ri->flags = !!replace;  // ReplaceIfExists
        if (!SetFileInformationByHandle(h, FileRenameInfo, ri, (u32)size)) {
            // Discard it again, else, as when a network session has been
            // lost with the handle, delete it by name once closed
            u32 err     = GetLastError();
            u8  discard = 1;
            b32 marked  = SetFileInformationByHandle(h, FileDispositionInfo,
                                                     &discard, 1);
            os_close(ctx, fd);
            if (!marked) {
                DeleteFileW(wtemp);
            }
            SetLastError(err);
            return COMMIT_EREPLACE;
        }
    }
    release_guard(ctx);
    os_close(ctx, fd);
    return COMMIT_OK;
}

static b32 os_isatty(os *ctx, i32 fd)
{
    return (u32)fd<3 && ctx->consoles>>fd & 1;
}

// The common errors, worded as the C runtime that Info-ZIP's port uses
// words the errno values it maps them to.
static s8 os_error(os *ctx)
{
    (void)ctx;
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return S("No such file or directory");
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return S("Permission denied");
    case ERROR_DISK_FULL:
        return S("No space left on device");
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return S("File exists");
    }
    return S("");
}

// By the system's OEM code page, as Info-ZIP's port converts such names
// with OemToAnsi, or else by its ANSI code page, in which the port keeps
// names. A name has at most 65,535 bytes, each at most one unit. Bytes
// that the code page cannot decode (DBCS, UTF-8) leave it without one.
static s8 os_fromcp(os *ctx, s8 name, b32 oem, arena *perm, arena scratch)
{
    (void)ctx;
    i32  len = (i32)name.len;
    c16 *w   = new(&scratch, len+1, c16);
    u32  cp  = oem ? CP_OEMCP : CP_ACP;
    i32  n   = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, name.s, len, w,
                                   len);
    w[n>0 ? n : 0] = 0;
    return n>0 ? towtf8(perm, w) : (s8){0};
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

// By the time zone's rules for that year, as POSIX localtime and .NET
// apply them, where Info-ZIP's port, by the C runtime's localtime,
// applies the current year's to every year.
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
    os_init(&ctx);
    reserve(&ctx);
    zipconfig conf = {0};
    conf.perm    = (arena){ctx.lo, ctx.lo, &ctx, 0};
    conf.scratch = (arena){ctx.hi, ctx.hi, &ctx, 1};
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
