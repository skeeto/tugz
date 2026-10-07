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
#include "../src/wild.c"
#include "../src/dir.c"

#include "windows.c"

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

// A match: its path packed after the last, with a terminator, in the
// arena that data is.
static b32 add_arg(void *data, s8 path, os_dirent *entry, arena scratch)
{
    (void)entry;
    (void)scratch;
    u8 *copy = newstr(data, path.len+1);
    bytecopy(copy, path.s, path.len);
    copy[path.len] = 0;
    return 1;
}

// Expand wildcards in arguments, which Windows shells leave to programs,
// as zip expands them (src/dir.c) and as a POSIX shell would: one with *
// or ? becomes the names it matches, in order, but for hidden and system
// files, as a shell leaves out dotfiles, or if none, stays as it is.
// The arguments go to perm, and directory listings to scratch. Matches
// are packed first, then the arguments made all at once, since an array
// doubled as it grew between them would leave each smaller copy behind.
static s8 *expand_args(os *ctx, arena *perm, s8 *args, i32 *nargs,
                       arena scratch)
{
    iz *matches = new(perm, *nargs, iz);
    u8 *p       = (u8 *)perm->beg;  // where they begin
    iz  total   = 0;
    for (i32 i = 0; i < *nargs; i++) {
        if (zip_haswild(args[i], 0)) {
            matches[i] = expand_wild(ctx, args[i], 0, WILD_WINDOWS, add_arg,
                                     perm, scratch);
        }
        total += matches[i] ? matches[i] : 1;
    }

    s8 *r = new(perm, total, s8);
    iz  n = 0;
    for (i32 i = 0; i < *nargs; i++) {
        if (!matches[i]) {
            r[n++] = args[i];
        }
        for (iz k = 0; k < matches[i]; k++, n++) {
            r[n].s = p;
            for (; p[r[n].len]; r[n].len++) {}
            p += r[n].len + 1;
        }
    }
    *nargs = (i32)total;
    return r;
}

void mainCRTStartup(void)
{
    os ctx = {0};
    os_init(&ctx);
    ctx.wildnames = 1;  // as after a shell (expand_args)
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

    // Listings take the top three quarters of the memory left, which
    // the expanded arguments, below them, then leave free
    arena scratch  = conf.perm;
    scratch.beg   += (scratch.end - scratch.beg) / 4;
    conf.perm.end  = scratch.beg;
    conf.args      = expand_args(&ctx, &conf.perm, conf.args, &conf.nargs,
                                 scratch);
    conf.perm.end  = scratch.end;
    os_exit(&ctx, gzip_main(&conf));
}
