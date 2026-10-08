// CRT-free Win32 platform layer for tugz unzip
// $ cc -O2 -nostartfiles -o unzip.exe platform/unzip_windows.c -lmemory
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/inflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/wild.c"
#include "../src/dir.c"
#include "../src/zipin.c"
#include "../src/unzip.c"
#include "../src/unzipcli.c"

#include "windows.c"
#include "zipfs_windows.c"

// Extraction: directories, attributes, link files, times

W32(b32) CreateDirectoryW(c16 *, uptr);
W32(b32) TzSpecificLocalTimeToSystemTime(uptr, systemtime *, systemtime *);

#define FILE_ATTRIBUTE_ARCHIVE      0x20u
#define ERROR_CANT_RESOLVE_FILENAME 1921u

static u32 os_umask(os *ctx)
{
    (void)ctx;
    return 0;  // access control is inherited from the directory
}

// By the time zone's rules for that year, as os_localtime applies them,
// rather than the current year's for every year, as Info-ZIP's port's
// C runtime mktime does (win32.c, dos_to_unix_time in fileio.c). Fields
// out of range carry over first, as through uz_timegm. A local time
// skipped by a change to daylight saving time is taken as the system
// takes it, and one repeated, as the first.
static i64 os_mktime(os *ctx, i32 const tm[6])
{
    (void)ctx;
    i64 t = uz_timegm(tm);  // normalized, as though UTC
    i32 n[6];
    zip_gmtime(t, n);
    if (n[0]<1601 || n[0]>30827) {
        return t;
    }
    systemtime loc = {0};
    loc.year   = (u16)n[0];
    loc.month  = (u16)n[1];
    loc.day    = (u16)n[2];
    loc.hour   = (u16)n[3];
    loc.minute = (u16)n[4];
    loc.second = (u16)n[5];
    systemtime utc = {0};
    if (!TzSpecificLocalTimeToSystemTime(0, &loc, &utc)) {
        return t;
    }
    i32 u[6] = {utc.year, utc.month, utc.day, utc.hour, utc.minute,
                utc.second};
    return uz_timegm(u);
}

static b32 os_mkdir(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    c16 *wpath = winpath(&scratch, path);
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);  // as in os_open
        return 0;
    }
    return CreateDirectoryW(wpath, 0);
}

// A FILETIME from Unix seconds, as a FILE_BASIC_INFO time, where zero
// means unchanged: times before 1601 become its first tick.
static i64 filetime(i64 t)
{
    i64 lo = -11644473600;
    i64 hi = (0x7fffffffffffffff / 10000000) + lo;
    t = t<lo ? lo : t>hi ? hi : t;
    i64 ft = (t - lo) * 10000000;
    return ft ? ft : 1;
}

// The times and attributes of attrs as one FILE_BASIC_INFO, its zero
// fields unchanged. Times include the creation time with OS_ACTIME, and
// leave the access time unchanged with OS_AKEEPA, as the port's
// SetFileTime gives a UT field's (win32.c, close_outfile and
// set_direc_attribs). Attributes, for a file only, are read-only,
// hidden, system, and archive, as Info-ZIP's port sets them (win32.c,
// close_outfile, through mapattr), without the others of the low byte
// that it passes, which files cannot have (directory, volume label).
static basic_info basic(osattrs *attrs, i32 flags)
{
    basic_info info = {0};
    if (flags & OS_ATIMES) {
        info.written  = filetime(attrs->mtime);
        if (!(attrs->flags & OS_AKEEPA)) {
            info.accessed = filetime(attrs->atime);
        }
        if (attrs->flags & OS_ACTIME) {
            info.created = filetime(attrs->ctime);
        }
    }
    if (flags & OS_AMODE) {
        u32 keep = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
                   FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
        info.attributes = attrs->dosattr & keep;
        info.attributes = info.attributes ? info.attributes
                                          : FILE_ATTRIBUTE_NORMAL;
    }
    return info;
}

// The reason for the last failure, copied, for each flag in failed.
static void failure(os *ctx, i32 failed, s8 *why, arena *a)
{
    s8 r = JOIN(a, os_error(ctx));
    for (i32 b = 0; b < OS_AWHY; b++) {
        why[b] = failed & 1<<b ? r : why[b];
    }
}

// Times and attributes in one call, on the handle that wrote the file,
// still delete-pending: Windows takes read-only on such a handle, and
// os_keep may still clear the deletion. The mode is ignored, as the
// owner is (unzip refuses -X here). Should the call fail, times and
// attributes are tried apart, to tell which failed.
static i32 os_setattrs(os *ctx, i32 fd, osattrs *attrs, s8 *why, arena *a)
{
    iptr h     = ctx->handles[fd];
    i32  flags = attrs->flags & (OS_AMODE|OS_ATIMES);
    if (!flags) {
        return 0;
    }
    basic_info info = basic(attrs, flags);
    if (SetFileInformationByHandle(h, FileBasicInfo, &info, sizeof(info))) {
        return 0;
    }
    i32 failed = 0;
    for (i32 f = OS_AMODE; f <= OS_ATIMES; f <<= 1) {
        info = basic(attrs, f);
        if ((flags & f) &&
            !SetFileInformationByHandle(h, FileBasicInfo, &info,
                                        sizeof(info))) {
            failure(ctx, f, why, a);
            failed |= f;
        }
    }
    return failed;
}

// A directory's times, as Info-ZIP's port sets them (win32.c,
// set_direc_attribs), which sets no attributes on directories, through
// a handle to the directory itself, refusing one that is a link,
// junction, or any other reparse point, which is no directory made by
// unzip.
static i32 os_setdirattrs(os *ctx, s8 path, osattrs *attrs, s8 *why,
                          arena *a)
{
    i32 flags = attrs->flags & OS_ATIMES;
    if (!flags) {
        return 0;
    }
    arena tmp   = *a;
    c16  *wpath = winpath(&tmp, path);
    iptr  h     = INVALID_HANDLE_VALUE;
    if (!wpath) {
        SetLastError(ERROR_INVALID_NAME);
    } else {
        h = CreateFileW(wpath, FILE_WRITE_ATTRIBUTES, FILE_SHARE_ALL, 0,
                        OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE, 0);
    }
    if (h == INVALID_HANDLE_VALUE) {
        failure(ctx, flags, why, a);
        return flags;
    }
    attribute_tag_info tag = {0};
    b32 ok = GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag,
                                          sizeof(tag));
    if (ok && (tag.attributes & FILE_ATTRIBUTE_REPARSE)) {
        ok = 0;
        SetLastError(ERROR_CANT_RESOLVE_FILENAME);
    } else if (ok && !(tag.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        ok = 0;
        SetLastError(ERROR_PATH_NOT_FOUND);
    }
    if (ok) {
        basic_info info = basic(attrs, flags);
        ok = SetFileInformationByHandle(h, FileBasicInfo, &info,
                                        sizeof(info));
    }
    if (!ok) {
        failure(ctx, flags, why, a);
    }
    CloseHandle(h);
    return ok ? 0 : flags;
}

// Links need privileges on Windows, which Info-ZIP's port does not
// make: as it extracts them, a link becomes a regular file holding its
// target, every byte of it (empty for an empty target), created where
// nothing is, as a link would be. It is written to completion before it
// is kept, so that a failure leaves nothing.
static i32 os_symlink(os *ctx, s8 target, s8 path, osattrs *attrs, s8 *why,
                      arena *a)
{
    (void)attrs;
    i32 fd = os_open(ctx, path, OS_CREATE, *a);
    b32 ok = fd >= 0;
    ok = ok && os_write(ctx, fd, target.s, target.len) && os_keep(ctx, fd);
    if (fd >= 0) {
        u32 err = GetLastError();
        if (!os_close(ctx, fd) && ok) {
            ok = 0;
            os_remove(ctx, path, *a);
            err = GetLastError();
        }
        SetLastError(err);
    }
    if (!ok) {
        failure(ctx, OS_ALINK, why, a);
        return OS_ALINK;
    }
    return 0;
}

void mainCRTStartup(void)
{
    os ctx = {0};
    os_init(&ctx);
    reserve(&ctx);
    unzipconfig conf = {0};
    conf.perm    = (arena){ctx.lo, ctx.lo, &ctx, 0};
    conf.scratch = (arena){ctx.hi, ctx.hi, &ctx, 1};
    conf.windows = 1;

    // Wildcards in member names are unzip's own, matched against entries,
    // and in the archive's name expanded by unzip (expand_wild)
    i32 argc = 0;
    s8 *argv = os_args(&conf.perm, &argc);
    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = argv + (argc>0);

    conf.unzipenv = getenv8(&conf.perm, L"UNZIP");
    conf.unzipopt = getenv8(&conf.perm, L"UNZIPOPT");
    os_exit(&ctx, unzip_main(&conf));
}
