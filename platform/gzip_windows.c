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

// Windows has no meaningful equivalent of mode bits here: new files
// inherit their directory's access control, as they would from GNU gzip.
// Copy the timestamps.
static void os_copymeta(os *ctx, i32 from, i32 to)
{
    basic_info info = {0};
    if (GetFileInformationByHandleEx(ctx->handles[from], FileBasicInfo,
                                     &info, sizeof(info))) {
        basic_info set = {0};
        set.accessed = info.accessed;
        set.written  = info.written;
        SetFileInformationByHandle(ctx->handles[to], FileBasicInfo,
                                   &set, sizeof(set));
    }
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
