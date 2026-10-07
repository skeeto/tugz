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

static b32 os_missing(os *ctx)
{
    (void)ctx;
    u32 err = GetLastError();
    return err==ERROR_FILE_NOT_FOUND || err==ERROR_PATH_NOT_FOUND;
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

typedef struct {
    arena *perm;
    struct {
        s8 *data;
        iz  len;
        iz  cap;
    } args;
} expansion;

static b32 add_arg(void *data, s8 path, os_dirent *entry, arena scratch)
{
    (void)entry;
    (void)scratch;
    expansion *e = data;
    s8 copy = {newstr(e->perm, path.len), path.len};
    bytecopy(copy.s, path.s, path.len);
    *push(e->perm, &e->args) = copy;
    return 1;
}

// Expand wildcards in arguments, which Windows shells leave to programs,
// as zip expands them (src/dir.c) and as a POSIX shell would: one with *
// or ? becomes the names it matches, in order, but for hidden and system
// files, as a shell leaves out dotfiles, or if none, stays as it is.
// The arguments go to perm, and directory listings to scratch.
static s8 *expand_args(os *ctx, arena *perm, s8 *args, i32 *nargs,
                       arena scratch)
{
    expansion e = {perm, {0}};
    for (i32 i = 0; i < *nargs; i++) {
        if (!zip_haswild(args[i], 0) ||
            !expand_wild(ctx, args[i], 0, 0, add_arg, &e, scratch)) {
            *push(perm, &e.args) = args[i];
        }
    }
    *nargs = (i32)e.args.len;
    return e.args.data;
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
