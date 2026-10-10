// CRT-free Win32 platform layer for tugz zip
// $ cc -O2 -nostartfiles -o zip.exe platform/zip_windows.c -lmemory
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/deflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/wild.c"
#include "../src/dir.c"
#include "../src/zipin.c"
#include "../src/zipcli.c"

#include "windows.c"
#include "zipfs_windows.c"

W32(b32)  FlushFileBuffers(iptr);

#define FILE_ATTRIBUTE_ARCHIVE     0x20u
#define FILE_ATTRIBUTE_NOT_INDEXED 0x2000u
#define FILE_FLAG_DELETE_ON_CLOSE  0x04000000u
#define FILE_RENAME_REPLACE        1u
#define FILE_RENAME_POSIX          2u

static s8 os_readlink(os *ctx, s8 path, arena *a)
{
    (void)ctx;
    (void)path;
    (void)a;
    return (s8){0};  // never asked: links are followed
}

// The final path of an open file, a \\?\ path, with its volume named
// one way: as a drive letter path (DOS), by its GUID, or by its NT
// device. Null if the system has no such name for it. XP, which lacks
// GetFinalPathNameByHandleW, has only the NT one, from the object
// manager (NtQueryObject's ObjectNameInformation): the path past any
// junctions, as GetFinalPathNameByHandleW gives it, but for any short
// names and the case of each name, which stay as they were opened.
enum { VOLUME_NAME_DOS, VOLUME_NAME_GUID, VOLUME_NAME_NT };
static c16 *final_path(os *ctx, iptr h, u32 how, arena *a)
{
    winapi *w = &ctx->api;
    if (w->finalpath) {
        u32  cap = w->finalpath(h, 0, 0, how);  // including the null
        c16 *buf = cap ? new(a, cap, c16) : 0;
        u32  len = buf ? w->finalpath(h, buf, cap, how) : 0;
        return len && len<cap ? buf : 0;
    } else if (how!=VOLUME_NAME_NT || !w->ntname) {
        return 0;
    }

    // A UNICODE_STRING, then the name, at most 65,534 bytes
    u32 cap = (u32)sizeof(unicode_string) + 0x10000;
    unicode_string *name = (unicode_string *)newbytes(a, cap);
    u32 len = 0;
    if (!ntresult(w, w->ntname(h, 1, name, cap, &len)) || !name->len) {
        return 0;
    }
    iz   n = name->len / 2;
    c16 *r = new(a, n+1, c16);
    bytecopy(r, name->s, n*(iz)sizeof(c16));
    r[n] = 0;
    return r;
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
// a drive or share path loses its \\?\ (winpath restores it). As on
// POSIX, a path that cannot be examined, other than for nothing there,
// cannot be followed, so that zip refuses it before any work, as
// Info-ZIP does on failing to create it then: a name that no file can
// have (<, a component over 255 units), a file that another process
// holds delete-pending, a share or server not found.
static s8 os_resolve(os *ctx, s8 path, arena *perm, arena scratch)
{
    c16 *wpath = winpath(&scratch, path);
    u32  attr  = wpath ? GetFileAttributesW(wpath) : INVALID_FILE_ATTRIBUTES;
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);  // as in os_open
        return (s8){0};
    } else if (attr == INVALID_FILE_ATTRIBUTES) {
        return os_missing(ctx) ? path : (s8){0};
    } else if (!(attr & FILE_ATTRIBUTE_REPARSE)) {
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
    c16 *target = final_path(ctx, h, how, &scratch);
    while (!target && how<VOLUME_NAME_NT) {
        target = final_path(ctx, h, ++how, &scratch);
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
        c16 *final = final_path(ctx, dh, how, &scratch);
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
    c16 *wpath = winpath(&scratch, path);
    iptr h     = !wpath ? INVALID_HANDLE_VALUE :
                 CreateFileW(wpath, FILE_READ_ATTRIBUTES, FILE_SHARE_ALL, 0,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return (s8){0};
    }
    c16 *full = 0;
    for (u32 how = VOLUME_NAME_DOS; !full && how<=VOLUME_NAME_NT; how++) {
        full = final_path(ctx, h, how, &scratch);
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

static b32 os_writeat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    stop_if_ending();
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
    return set_info(ctx, ctx->handles[fd], FileEndOfFileInfo, &len,
                    sizeof(len));
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
    stop_if_ending();
    iptr h = ctx->handles[fd];
    if (!FlushFileBuffers(h)) {
        // A file system that cannot flush (some network and virtual ones)
        // defers nothing
        u32 err = GetLastError();
        if (err!=ERROR_INVALID_FUNCTION && err!=ERROR_NOT_SUPPORTED) {
            os_close(ctx, fd);  // still delete-pending
            release_guard(ctx);
            SetLastError(err);
            return COMMIT_ECLOSE;
        }
    }
    c16 *wpath = winpath(&scratch, path);
    c16 *wtemp = winpath(&scratch, temp);
    if (!wpath || !wtemp) {
        os_close(ctx, fd);
        release_guard(ctx);
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
        set_info(ctx, h, FileBasicInfo, &info, sizeof(info));
    }

    s16 name = s16lit(wpath);
    iz  size = (iz)sizeof(rename_info) + (name.len+1)*(iz)sizeof(c16);
    rename_info *ri = (rename_info *)newbytes(&scratch, size);
    bytefill(ri, 0, size);
    ri->flags = FILE_RENAME_POSIX | (replace ? FILE_RENAME_REPLACE : 0);
    ri->len   = (u32)(name.len * (iz)sizeof(c16));
    bytecopy(ri->name, name.s, name.len*(iz)sizeof(c16));

    u8 keep = 0;
    if (!set_info(ctx, h, FileDispositionInfo, &keep, 1)) {
        u32 err = GetLastError();
        os_close(ctx, fd);  // still delete-pending
        release_guard(ctx);
        SetLastError(err);
        return COMMIT_EREPLACE;
    }
    if (!set_info(ctx, h, FileRenameInfoEx, ri, (u32)size)) {
        // Older Windows, or a file system without POSIX semantics
        // (FAT, some SMB servers), which also refuses while os_writable
        // holds the archive
        release_guard(ctx);
        ri->flags = !!replace;  // ReplaceIfExists
        if (!set_info(ctx, h, FileRenameInfo, ri, (u32)size)) {
            // Discard it again, else, as when a network session has been
            // lost with the handle, delete it by name once closed
            u32 err     = GetLastError();
            u8  discard = 1;
            b32 marked  = set_info(ctx, h, FileDispositionInfo, &discard, 1);
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
