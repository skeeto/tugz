// POSIX platform layer for tugz unzip
// $ cc -O2 -o unzip platform/unzip_posix.c
#define _POSIX_C_SOURCE 200809L  // sigaction, pread, O_NOFOLLOW, ...
#define _DARWIN_C_SOURCE         // macOS hides O_NOFOLLOW otherwise
#define _FILE_OFFSET_BITS 64     // large files on 32-bit hosts
#define _TIME_BITS 64            // and times past 2038 (glibc)
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
#include "posix.c"
#include "zipfs_posix.c"

int main(int argc, char **argv)
{
    os ctx = {0};
    ctx.outfd = -1;
    reserve_stdfds(&ctx, S("error:  cannot open /dev/null\n"), PK_PARAM);
    install_signals();

    unzipconfig conf = {0};
    reserve(&ctx, S("error:  not enough memory (opening /dev/zero)\n"),
            PK_MEM);
    conf.perm    = (arena){ctx.lo, ctx.lo, &ctx, 0};
    conf.scratch = (arena){ctx.hi, ctx.hi, &ctx, 1};

    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = new(&conf.perm, conf.nargs, s8);
    for (i32 i = 0; i < conf.nargs; i++) {
        conf.args[i] = cstr(argv[i+1]);
    }
    char *unzipenv = getenv("UNZIP");
    char *unzipopt = getenv("UNZIPOPT");
    conf.unzipenv = unzipenv ? cstr(unzipenv) : (s8){0};
    conf.unzipopt = unzipopt ? cstr(unzipopt) : (s8){0};
    return unzip_main(&conf);
}
