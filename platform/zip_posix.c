// POSIX platform layer for tugz zip
// $ cc -O2 -o zip platform/zip_posix.c
#define _POSIX_C_SOURCE 200809L  // sigaction, pread, O_NOFOLLOW, ...
#define _DARWIN_C_SOURCE         // macOS hides O_NOFOLLOW otherwise
#define _FILE_OFFSET_BITS 64     // large files on 32-bit hosts
#define _TIME_BITS 64            // and times past 2038 (glibc)
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/deflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/wild.c"
#include "../src/dir.c"
#include "../src/zipin.c"
#include "../src/zipcli.c"
#include "posix.c"
#include "zipfs_posix.c"

#include <stdio.h>  // rename

static s8 os_readlink(os *ctx, s8 path, arena *a)
{
    (void)ctx;
    char *cpath = tocstr(a, path);
    for (iz cap = 256;; cap *= 2) {
        u8 *buf = newstr(a, cap);
        iz  n   = readlink(cpath, (char *)buf, (uz)cap);
        if (n < 0) {
            return (s8){0};
        } else if (n < cap) {
            return (s8){buf, n};
        }
    }
}

// Links are read one at a time, so that a dangling one leads where it
// points, a relative target being relative to the link's directory. But
// only links the system itself would follow are: not a loop, nor one
// that Linux's protected_symlinks refuses in a sticky directory, nor an
// empty one. So a chain may be as long as stat follows, up to Linux's 40
// links (others allow fewer), leaving one more read to find the file at
// its end.
static s8 os_resolve(os *ctx, s8 path, arena *perm, arena scratch)
{
    struct stat st;
    if (stat(tocstr(&scratch, path), &st) && errno!=ENOENT) {
        return (s8){0};
    }
    for (i32 links = 0;; links++) {
        s8 target = os_readlink(ctx, path, &scratch);
        if (!target.s) {
            // Not a link (EINVAL), or nothing there
            return errno==EINVAL || errno==ENOENT ? path : (s8){0};
        } else if (!target.len) {
            errno = ENOENT;  // as stat finds it (BSD, macOS; Linux has none)
            return (s8){0};
        } else if (links == 40) {
            errno = ELOOP;  // a chain grown since stat followed it
            return (s8){0};
        }
        iz cut = path.len;
        if (target.len && target.s[0]=='/') {
            cut = 0;
        }
        for (; cut>0 && path.s[cut-1]!='/'; cut--) {}
        s8 next = {newstr(perm, cut+target.len), cut+target.len};
        bytecopy(next.s, path.s, cut);
        bytecopy(next.s+cut, target.s, target.len);
        path = next;
    }
}

// Judged by the system, as when Info-ZIP opens the archive to update
// it: root, ACLs, flags, and read-only file systems all count.
static b32 os_writable(os *ctx, s8 path, arena scratch)
{
    (void)ctx;
    return !access(tocstr(&scratch, path), W_OK);
}

static b32 os_writeat(os *ctx, i32 fd, u8 *buf, iz len, i64 off)
{
    (void)ctx;
    while (len) {
        iz r = pwrite(fd, buf, (uz)MIN(len, 1<<30), (off_t)off);
        if (r < 0 && errno == EINTR) {
            continue;
        } else if (r <= 0) {
            return 0;
        }
        buf += r;
        len -= r;
        off += r;
    }
    return 1;
}

static b32 os_truncate(os *ctx, i32 fd, i64 len)
{
    (void)ctx;
    return !ftruncate(fd, (off_t)len);
}

// Move a file to a path where nothing was, failing if anything has
// appeared there since: by linking it there, then unlinking it, or where
// the file system has no hard links (FAT, some network file systems), by
// renaming it, the best that POSIX offers.
static b32 place(char *src, char *dst)
{
    if (!link(src, dst)) {
        unlink(src);
        return 1;
    }
    return errno!=EEXIST && !rename(src, dst);
}

static i32 os_commit(os *ctx, i32 fd, s8 temp, s8 path, b32 replace,
                     arena scratch)
{
    (void)temp;  // pending_output
    char *dst = tocstr(&scratch, path);
    struct stat st;
    if (!stat(dst, &st)) {
        // The old group, where the user may give it (a member, or root),
        // else its permissions would go to another group (the user's, or
        // on BSD the directory's), which then gets only those of others.
        // Set-ID and sticky bits too, as Info-ZIP keeps them, but only
        // while the owner and group are the old ones: otherwise set-ID
        // bits would run the program as someone the old file did not.
        // Should the system refuse them (BSD's sticky files), the rest.
        mode_t      mode  = st.st_mode & 0777;
        struct stat self;
        b32         known = !fstat(fd, &self);
        if (known && self.st_gid!=st.st_gid &&
                !fchown(fd, (uid_t)-1, st.st_gid)) {
            self.st_gid = st.st_gid;
        }
        if (!known || self.st_gid!=st.st_gid) {
            mode = (mode_t)((mode & 0707) | (mode & 07)<<3);
        } else if (self.st_uid == st.st_uid) {
            mode = st.st_mode & 07777;
        }
        if (fchmod(fd, mode)) {
            fchmod(fd, mode & 0777);
        }
    } else if (!ctx->defperms && os_missing(ctx)) {
        // The archive existed at startup, so the temp file is owner-only,
        // but it has since gone and this becomes a new file. Give it the
        // usual 0666 less the umask. Unlike OS_DEFPERMS, this ignores a
        // default ACL, and the target may still change before the rename.
        // Should the archive be there but not examined (an I/O error),
        // its mode is unknown, and owner-only is the safe guess, as in
        // Info-ZIP, which then leaves its temporary file's mode too.
        mode_t mask = umask(0);
        umask(mask);
        fchmod(fd, 0666 & ~mask);
    }

    // Close before replacing anything, since deferred write errors (NFS,
    // quotas) may surface only now. Until the rename, the file is still
    // discarded on failure or interruption. As in Info-ZIP there is no
    // fsync: on an SD card, waiting for the device turned a 0.85 s run
    // storing 256 MiB into 20-40 s, adding only durability across a
    // crash and the rare device error that nothing else reports.
    b32 ok = !close(fd) || errno==EINTR;
    ctx->outfd = -1;
    i32 r = ok ? COMMIT_OK : COMMIT_ECLOSE;

    sigset_t old = block_signals();
    if (ok && replace) {
        ok = !rename(pending_output, dst);
    } else if (ok) {
        ok = place(pending_output, dst);
    }
    r = r ? r : ok ? COMMIT_OK : COMMIT_EREPLACE;
    int err = errno;  // why, rather than why the discarding failed
    release_output(ok);
    restore_signals(old);
    errno = err;
    return r;
}

static s8 os_fullpath(os *ctx, s8 path, arena *perm, arena scratch)
{
    (void)ctx;
    (void)path;
    (void)perm;
    (void)scratch;
    return (s8){0};  // never needed: every file has an inode number
}

int main(int argc, char **argv)
{
    os ctx = {0};
    ctx.outfd = -1;
    reserve_stdfds(&ctx, S("\nzip error: Could not open /dev/null\n"),
                   ZE_TEMP);
    install_signals();

    zipconfig conf = {0};
    // As Info-ZIP words what it was doing when memory ran out
    reserve(&ctx, S("\nzip error: Out of memory (opening /dev/zero)\n"),
            ZE_MEM);
    conf.perm    = (arena){ctx.lo, ctx.lo, &ctx, 0};
    conf.scratch = (arena){ctx.hi, ctx.hi, &ctx, 1};

    conf.nargs = argc>0 ? argc-1 : 0;
    conf.args  = new(&conf.perm, conf.nargs, s8);
    for (i32 i = 0; i < conf.nargs; i++) {
        conf.args[i] = cstr(argv[i+1]);
    }
    char *epoch  = getenv("SOURCE_DATE_EPOCH");
    char *zipopt = getenv("ZIPOPT");
    char *zipenv = getenv("ZIP");
    conf.epoch  = epoch  ? cstr(epoch)  : (s8){0};
    conf.zipopt = zipopt ? cstr(zipopt) : (s8){0};
    conf.zipenv = zipenv ? cstr(zipenv) : (s8){0};
    return zip_main(&conf);
}
