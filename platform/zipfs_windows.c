// CRT-free Win32 file system functions for tugz zip and unzip: the
// arena's reservation, examining files, positioned reads, code pages,
// the environment, local time. Included by the programs' platform
// layers, after windows.c.

typedef struct {
    uptr internal, internal_high;
    u32  offset, offset_high;
    uptr event;
} overlapped;

typedef struct {
    u16 year, month, weekday, day, hour, minute, second, ms;
} systemtime;

typedef struct {
    u64 volume;
    u64 id[2];  // 128 bits
} file_id_info;

W32(u32)  GetEnvironmentVariableW(c16 *, c16 *, u32);
W32(i32)  MultiByteToWideChar(u32, u32, u8 const *, i32, c16 *, i32);
W32(b32)  SystemTimeToTzSpecificLocalTime(uptr, systemtime *, systemtime *);

#define CP_ACP                     0u
#define CP_OEMCP                   1u
#define MB_ERR_INVALID_CHARS       0x8u
#define ERROR_ENVVAR_NOT_FOUND     203u

enum { FileIdInfo = 18 };

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
// entry's name, when a link or a junction, or any other reparse point
// that names another file (reparse_link), is FT_LINK. Other reparse
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
        // The console (CON), unlike other DOS devices (NUL, CONIN$),
        // refuses an open that would neither read nor write, but is
        // there to refuse it: special, as they are, rather than missing
        b32 dosdev = wpath[0]=='\\' && wpath[1]=='\\' && wpath[2]=='.';
        if (!dosdev || GetLastError()!=ERROR_INVALID_PARAMETER) {
            return 0;
        }
        *info = (os_info){0};
        info->type = FT_OTHER;
        return 1;
    }
    b32 ok = handle_info(h, info);
    attribute_tag_info tag = {0};
    if (ok && !follow && (info->attr & FILE_ATTRIBUTE_REPARSE) &&
        GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag,
                                     sizeof(tag)) &&
        reparse_link(tag)) {
        info->type = FT_LINK;
    }
    CloseHandle(h);
    return ok;
}

static b32 os_fstat(os *ctx, i32 fd, os_info *info)
{
    return handle_info(ctx->handles[fd], info);
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
