// CRT-free Win32 platform layer for tugz gzip
// $ cc -O2 -nostartfiles -o gzip.exe platform/gzip_windows.c -lmemory
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/adler32.c"
#include "../src/inflate.c"
#include "../src/deflate.c"
#include "../src/gzip.c"
#include "../src/io.c"
#include "../src/gzipio.c"
#include "../src/cli.c"

#include "windows.c"

static b32 os_isatty(os *ctx, i32 fd)
{
    return (u32)fd<3 && ctx->consoles>>fd & 1;
}

#define ERROR_FILE_NOT_FOUND    2u
#define ERROR_PATH_NOT_FOUND    3u
#define ERROR_LOCK_VIOLATION    33u
#define ERROR_HANDLE_DISK_FULL  39u
#define ERROR_DISK_FULL         112u
#define ERROR_ALREADY_EXISTS    183u
#define ERROR_NO_DATA           232u

static b32 os_missing(os *ctx)
{
    (void)ctx;
    u32 err = GetLastError();
    return err==ERROR_FILE_NOT_FOUND || err==ERROR_PATH_NOT_FOUND;
}

// The common errors, worded as the C runtime words the errno values it
// maps them to, as zip words them, but for a closed pipe.
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
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_FULL:
        return S("No space left on device");
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return S("File exists");
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
        return S("Broken pipe");
    case ERROR_INVALID_NAME:
        return S("Invalid argument");
    case ERROR_TOO_MANY_OPEN_FILES:
        return S("Too many open files");
    }
    return S("");
}

// A pipe whose reader has gone, which has no SIGPIPE here: writes fail
// with "the pipe is being closed", or else "the pipe has been ended".
static b32 os_pipeclosed(os *ctx)
{
    (void)ctx;
    u32 err = GetLastError();
    return err==ERROR_NO_DATA || err==ERROR_BROKEN_PIPE;
}

struct osmeta {
    basic_info info;
};

static osmeta *os_getmeta(os *ctx, i32 fd, arena *a)
{
    osmeta *m = new(a, 1, osmeta);
    b32 ok = GetFileInformationByHandleEx(ctx->handles[fd], FileBasicInfo,
                                          &m->info, sizeof(m->info));
    return ok ? m : 0;
}

#define FILE_ATTRIBUTE_HIDDEN      0x02u
#define FILE_ATTRIBUTE_SYSTEM      0x04u
#define FILE_ATTRIBUTE_ARCHIVE     0x20u
#define FILE_ATTRIBUTE_NOT_INDEXED 0x2000u

// Copy the timestamps, and the attributes that describe a file as the
// mode does on POSIX: read-only, the nearest thing to a missing write
// bit, and as zip keeps a replaced archive's, hidden, system, and not
// indexed, with the archive bit set, as on any new file. Access control
// is inherited from the directory, as new files' would be from GNU gzip.
static b32 os_setmeta(os *ctx, i32 fd, osmeta *m)
{
    u32 kept = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
               FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_INDEXED;
    basic_info set = {0};
    set.accessed = m->info.accessed;
    set.written  = m->info.written;
    if (m->info.attributes & kept) {
        set.attributes = (m->info.attributes & kept) | FILE_ATTRIBUTE_ARCHIVE;
    }
    return SetFileInformationByHandle(ctx->handles[fd], FileBasicInfo,
                                      &set, sizeof(set));
}

void mainCRTStartup(void)
{
    os ctx = {0};
    os_init(&ctx);
    iz    cap = (iz)1 << 25;
    byte *mem = VirtualAlloc(0, cap, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        os_oom(&ctx);
    }
    config conf = {0};
    conf.perm = (arena){mem, mem+cap, &ctx, 0};

    i32 argc = 0;
    s8 *argv = os_args(&conf.perm, &argc);
    if (argc > 0) {
        // Drop the directory and an .exe extension
        s8 name = argv[0];
        for (iz i = name.len; i > 0; i--) {
            if (name.s[i-1]=='/' || name.s[i-1]=='\\' || name.s[i-1]==':') {
                name.s += i;
                name.len -= i;
                break;
            }
        }
        if (name.len > 4) {
            s8 ext = {name.s + name.len - 4, 4};
            name.len -= ascii_iequals(ext, S(".exe")) ? 4 : 0;
        }
        conf.name = name;
    }
    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = argv + (argc>0);
    os_exit(&ctx, gzip_main(&conf));
}
