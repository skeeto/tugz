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
#include "../src/zipinfo.c"
#include "../src/unzipcli.c"

#include "windows.c"
#include "zipfs_windows.c"
#include "unzipos_windows.c"

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
    conf.argv0 = argc>0 ? argv[0] : (s8){0};  // a copy named zipinfo.exe
    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = argv + (argc>0);

    conf.unzipenv   = getenv8(&conf.perm, L"UNZIP");
    conf.unzipopt   = getenv8(&conf.perm, L"UNZIPOPT");
    conf.zipinfoenv = getenv8(&conf.perm, L"ZIPINFO");
    conf.zipinfoopt = getenv8(&conf.perm, L"ZIPINFOOPT");
    os_exit(&ctx, unzip_main(&conf));
}
