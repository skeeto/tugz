// libFuzzer harness: the unzip program with arbitrary bytes as its archive
// Runs unzip_main in memory (test/unzipos.c) over a small tree: a.zip, a
// sentinel tree outside the -d directory (outside/), and in it (out/)
// files to overwrite and links planted to lead outside, as another
// program may have left them, with dlink, a link to out, as one -d.
// The input is a header, then the archive:
// - how: the mode, its arguments (low four bits), Windows conventions
//   (bit 4), and a fault (bits 5-7): the archive shrinking as it is
//   opened, or shrinking or failing to read once extraction began;
//   writes failing; the nth directory, file, or keep failing, and
//   attributes or links.
// - at: where or when the fault strikes.
// - cfg: the umask (bits 0-1), standard error a terminal (bit 2), the
//   archive on standard input a regular file (bit 3), and the answers
//   to the prompts (bits 4-7), unless given:
// - n, then n bytes: the answers, if n is not zero.
// Whatever unzip does, it must exit with a status UnZip may; change
// nothing outside the -d directory (nothing at all but to extract), nor
// write over any file there (only replace it); leave every file it
// extracted, by the name it maps, equal to an independent reading of an
// entry of that name (src/zip.c's parsing of every central header there
// is, its data at any local header, then zlib's inflation and the CRC),
// or for a link not yet made, empty; pipe (-p) no more than the entries'
// sizes; leave nothing open; claim no memory once it has changed the
// file system (unzipos.c checks that, and that nothing is written
// through a link); and, after a run without faults or errors, run again
// with -n, change nothing.
// With FUZZ_UNZIP_SHOW in the environment, each run's status and output
// are shown, as when reproducing a crash.
// $ clang -g -O1 -fsanitize=fuzzer,address,undefined test/fuzz_unzip.c -lz
// $ ./a.out -max_len=8192 corpus/
#include "unzipos.c"

#include <stdint.h>
#include <zlib.h>

#define MAXIN ((iz)1 << 20)

// The arguments, chosen by the low four bits of the first byte, and the
// -d directory each extracts to, as unzip's root and its canonical name
static struct {
    char *argv[8];
    char *dest;
    char *canon;
    b32   rerun;  // -n afterwards changes nothing
} const modes[16] = {
    {{"-o", "a.zip", "-d", "out"}, "out/", "out", 1},
    {{"a.zip", "-d", "out"}, "out/", "out", 1},  // asking
    {{"-n", "a.zip", "-d", "out"}, "out/", "out", 1},
    {{"-oj", "a.zip", "-d", "out"}, "out/", "out", 1},
    {{"-o", "a.zip", "-x", "*b*", "-d", "out"}, "out/", "out", 1},
    {{"-fo", "a.zip", "-d", "out"}, "out/", "out", 0},
    {{"-uo", "a.zip", "-d", "out"}, "out/", "out", 0},
    {{"-oV", "a.zip", "*", "-dout/sub"}, "out/sub/", "out/sub", 1},
    {{"-o", "a.zip", "-d", "dlink"}, "dlink/", "out", 1},
    {{"-oKX", "a.zip", "-d", "out"}, "out/", "out", 1},
    {{"-oDDC", "a.zip", "[A-M]*", "-d", "out"}, "out/", "out", 1},
    {{"-l", "a.zip"}, 0, 0, 0},
    {{"-v", "a.zip", "-x", "*a*"}, 0, 0, 0},
    {{"-t", "a.zip"}, 0, 0, 0},
    {{"-p", "a.zip"}, 0, 0, 0},
    {{"-o", "-", "-d", "out"}, "out/", "out", 1},
};
enum { MODE_ASK = 1, MODE_PIPE = 14, MODE_STDIN = 15 };

// Answers to the prompts
static char const *const answers[16] = {
    "", "y\n", "n\n", "A\n", "N\n", "r\nren\n", "r\n../up/x\nA\n",
    "x\n\ny\nn\nr\n\nr2\nN\n", "y\nn\ny\nn\ny\nn\ny\nn\n", "r\nd/\nA\n",
    "r\nesc/x\nr\nabs/x\nr\nfesc\nA\n", "r\n", "y\ny\ny\nr\nren\nr\nren\nN\n",
    "A", "N", "r\n\x01\n",
};

static b32 same_bytes(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !memcmp(a.s, b.s, (uz)a.len));
}

static b32 equals_z(s8 a, char const *z)
{
    return same_bytes(a, cstrs8(z));
}

// An entry as read independently: its central header, and its name,
// in Unicode if it has one
typedef struct {
    s8      name;
    zentry  e;
} xentry_;

// The entries, and where the data of every local header there begins
typedef struct {
    xentry_ *data;
    iz       len;
    i64     *at;
    iz       nat;
} xentries;

// Decode an entry's data at off, within the archive, by zlib: the
// stored bytes, or a raw deflate stream ending within them; null if not.
static s8 decode(s8 z, i64 off, zentry *e, u8 *out, iz cap)
{
    if (off<0 || off>z.len || e->csize>z.len-off) {
        return (s8){0};
    }
    s8 in = {z.s+off, (iz)e->csize};
    if (e->method == ZIP_STORE) {
        return in;
    } else if (e->method != ZIP_DEFLATE) {
        return (s8){0};
    }
    z_stream s = {0};
    CHECK(inflateInit2(&s, -15) == Z_OK);
    s.next_in   = in.s;
    s.avail_in  = (uInt)in.len;
    s.next_out  = out;
    s.avail_out = (uInt)cap;
    i32 r = inflate(&s, Z_FINISH);
    s8 got = {out, cap - (iz)s.avail_out};
    inflateEnd(&s);
    return r==Z_STREAM_END ? got : (s8){0};
}

// Every central header anywhere in the archive, and every local header,
// whose data an entry may be, wherever UnZip finds it (shifted by its
// extra bytes, or not, or by data before the archive).
static xentries read_entries(os *ctx, s8 z, b32 windows, arena *a)
{
    xentries r = {0};
    r.data = new(a, z.len/ZIP_CENTRAL_LEN + 1, xentry_);
    r.at   = new(a, z.len/ZIP_LOCAL_LEN + 1, i64);
    // Names decode through scratch apart from where they are kept, as
    // the test layer's os_fromcp checks (a 64 KiB name as UTF-16, twice)
    arena scratch = subarena(a, (iz)1 << 19);
    for (iz i = 0; i+4 <= z.len; i++) {
        zentry e = {0};
        u32    sig = get32(z.s+i);
        if (sig==ZIP_LOCAL_SIG && i+ZIP_LOCAL_LEN<=z.len &&
            zip_local_varlen(z.s+i)>=0) {
            r.at[r.nat++] = i + ZIP_LOCAL_LEN + zip_local_varlen(z.s+i);
        } else if (sig==ZIP_CENTRAL_SIG &&
                   zip_parse_header(z.s+i, z.len-i, (i64)1<<60, &e)) {
            xentry_ *x = r.data + r.len++;
            s8 stale = {0};
            s8 un    = zar_uname(ctx, &e, windows, &(i32){0}, &stale, a, scratch);
            x->e    = e;
            x->name = un.s ? un : e.name;
        }
    }
    return r;
}

// Whether a file extracted to rel (below the -d directory) is an entry's
// as it maps there: its data, or for a link not yet made, empty; with
// any, for a file renamed at the prompt, any entry's.
static b32 is_extracted(os *ctx, s8 z, xentries *xs, s8 rel, mfile *f,
                        i32 opts, b32 renamed, arena scratch)
{
    static u8 *buf;
    iz         max = (iz)1 << 26;
    if (!buf) {
        buf = malloc((uz)max + 1);
        CHECK(buf);
    }
    s8 data = {f->data ? f->data : (u8 *)"", f->len};
    for (iz i = 0; i < xs->len; i++) {
        xentry_ *x  = xs->data + i;
        arena    t  = scratch;
        uzpath   mp = uz_mapname(x->name, x->e.made, opts, &t);
        if ((mp.flags & (UZ_DIR|UZ_FAILED)) ||
            (!renamed && !mfs_eq(ctx, mp.path, rel))) {
            continue;
        }
        uzmode um     = uz_mode(x->e.extattr, x->e.made, x->e.name,
                                x->e.cextra, 0);
        b32    holder = um.symlink && f->type==FT_FILE && !f->len;
        if (holder) {
            return 1;
        }

        // Its data, at any local header, which must check out
        i64 size = x->e.method==ZIP_STORE ? x->e.csize : x->e.usize;
        iz  cap  = (iz)MIN(MAX(size, 0), max);
        for (iz k = 0; k < xs->nat; k++) {
            s8 want = decode(z, xs->at[k], &x->e, buf, cap+1);
            if (!want.s || want.len>cap ||
                crc32_update(0, want.s, want.len, &(i32){0})!=x->e.crc) {
                continue;
            }
            if (um.symlink && f->type==FT_LINK) {
                // A target up to a NUL, as symlink(2) takes it
                iz n = 0;
                for (; n<want.len && want.s[n]; n++) {}
                want.len = n;
            }
            if (same_bytes(want, data)) {
                return 1;
            }
        }
    }
    return 0;
}

int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size)
{
    static os *ctx;
    static u8 *mem;
    iz cap = (iz)1 << 28;
    if (!ctx) {
        ctx = unzipos_new((iz)1 << 28);
        mem = malloc((uz)cap);
    }
    if (size<4 || (iz)size-4-data[3]<0 || (iz)size>MAXIN) {
        return 0;
    }
    u8  how     = data[0];
    i64 at      = data[1];
    u8  cfg     = data[2];
    iz  nscript = data[3];
    i32 mode    = how & 15;
    b32 windows = how>>4 & 1;
    s8  script  = {(u8 *)data+4, nscript};
    s8  in      = {(u8 *)data+4+nscript, (iz)size-4-nscript};
    if (!nscript) {
        script = cstrs8(answers[cfg>>4]);
    }

    mfs_reset(ctx);
    ctx->windows = windows;
    ctx->umask   = (u32[]){022, 077, 0, 027}[cfg & 3];
    ctx->tty[2]  = cfg>>2 & 1;
    if (mode == MODE_STDIN) {
        ctx->archive   = "-";
        ctx->in        = in.s;
        ctx->inlen     = in.len;
        ctx->stdinfile = cfg>>3 & 1;
    } else {
        ctx->in    = script.s;
        ctx->inlen = script.len;
    }
    mfs_put(ctx, "a.zip", FT_FILE, in.s, in.len, 1600000000);
    mfs_put(ctx, "outside", FT_DIR, 0, 0, 1);
    mfs_put(ctx, "outside/f", FT_FILE, "sentinel\n", 9, 1);
    mfs_put(ctx, "outside/d", FT_DIR, 0, 0, 1);
    mfs_put(ctx, "dlink", FT_LINK, "out", 3, 1);
    mfs_put(ctx, "out", FT_DIR, 0, 0, 1);
    mfs_put(ctx, "out/a", FT_FILE, "old a\n", 6, 1);
    mfs_put(ctx, "out/d", FT_DIR, 0, 0, 1);
    mfs_put(ctx, "out/d/x", FT_FILE, "old x\n", 6, 1);
    mfs_put(ctx, "out/esc", FT_LINK, "../outside", 10, 1);
    mfs_put(ctx, "out/abs", FT_LINK, "/outside", 8, 1);
    mfs_put(ctx, "out/fesc", FT_LINK, "../outside/f", 12, 1);
    mfs_put(ctx, "out/new", FT_LINK, "../outside/new", 14, 1);
    mfs_put(ctx, "out/loop", FT_LINK, "loop", 4, 1);
    switch (how>>5 & 7) {
    case 1: case 2:
        ctx->when     = how>>5 & 1 ? FAULT_OPEN : FAULT_CHANGE;
        ctx->shrinkto = in.len * at / 256;
        break;
    case 3:
        ctx->when       = FAULT_CHANGE;
        ctx->failreadat = in.len * at / 256;
        break;
    case 4:
        ctx->failwriteat = at * 64;
        break;
    case 5:
        ctx->failmkdir = (i32)(at&7) + 1;
        ctx->failattrs = (i32)(at>>3 & 7);
        break;
    case 6:
        ctx->failcreate  = (i32)(at&7) + 1;
        ctx->failsymlink = at>>3 & 1;
        break;
    case 7:
        ctx->failkeep = (i32)(at&7) + 1;
        break;
    }
    s8 canon = modes[mode].canon ? cstrs8(modes[mode].canon) : (s8){0};
    ctx->dest = modes[mode].dest ? cstrs8(modes[mode].dest) : S("");

    msnap before = mfs_snapshot(ctx);
    char **argv  = (char **)modes[mode].argv;
    i32    argc  = 0;
    for (; argc<countof(modes[0].argv) && argv[argc]; argc++) {}
    i32 open   = 0;
    i32 status = unzipos_run(ctx, argv, argc, 0, &open);
    if (getenv("FUZZ_UNZIP_SHOW")) {
        s8 out = unzipos_output(ctx, 3);
        fprintf(stderr, "mode %d: status %d, %d changes\n%.*s", mode, status,
                ctx->nchanges, (int)MIN(out.len, 1<<12), out.s);
    }
    CHECK(!open);
    CHECK(status==0  || status==1  || status==2  || status==3  ||
          status==9  || status==10 || status==11 || status==12 ||
          status==50 || status==51 || status==81);
    CHECK(status!=10 || (windows && mode==9));
    b32 renamed = ctx->renamed;
    b32 faulted = ctx->faulted;

    // Nothing changed outside the -d directory, nor anywhere but to
    // extract, and the archive is as it was (or as it was cut short)
    b32 cut = ctx->armed && ctx->shrinkto>=0 && mode!=MODE_STDIN;
    for (i32 i = 0; i < before.n && cut; i++) {
        mfile *f = before.files + i;
        f->len = equals_z(f->name, "a.zip") ? MIN(f->len, (iz)ctx->shrinkto)
                                            : f->len;
    }
    CHECK(mfs_unchanged(ctx, before, canon));
    CHECK(canon.s || !ctx->nchanges);

    // Piped, no entry gives more than its size
    arena a = {(byte *)mem, (byte *)mem+cap, ctx, 0};
    if (mode == MODE_PIPE) {
        xentries xs  = read_entries(ctx, in, windows, &a);
        i64      max = 0;
        for (iz i = 0; i < xs.len; i++) {
            zentry *e = &xs.data[i].e;
            i64 size = e->method==ZIP_STORE ? e->csize : e->usize;
            max = size>INT64_MAX-max ? INT64_MAX : max+size;
        }
        CHECK(unzipos_output(ctx, 1).len <= max);
    }

    // Every file there now, but those that were, is an entry's
    if (canon.s && ctx->nchanges) {
        xentries xs = read_entries(ctx, in, windows, &a);
        i32 opts = (windows ? UZ_WINDOWS : 0) |
                   (mode==3 ? UZ_JUNK : 0) | (mode==7 ? UZ_KEEPVER : 0);
        for (i32 i = 0; i < ctx->nfiles; i++) {
            mfile *f = ctx->files + i;
            if (!f->live || f->name.len<=canon.len ||
                !mfs_within(ctx, f->name, canon)) {
                continue;
            }
            mfile *old = 0;
            for (i32 k = 0; k<before.n && !old; k++) {
                old = before.files[k].ino==f->ino ? before.files+k : 0;
            }
            if (old) {
                // Not written over, though attributes may be set
                CHECK(f->len==old->len && (!f->len ||
                      !memcmp(f->data, old->data, (uz)f->len)));
                continue;
            } else if (f->type == FT_DIR) {
                continue;
            }
            CHECK(f->type==FT_FILE || (f->type==FT_LINK && !windows));
            s8 rel = {f->name.s+canon.len+1, f->name.len-canon.len-1};
            CHECK(is_extracted(ctx, in, &xs, rel, f, opts, renamed, a));
        }
    }
    mfs_snapfree(before);

    // Again, with -n, changes nothing (but where faults or errors left
    // work undone: a damaged entry's file is gone, though it replaced one)
    b32 clean = status==0 || status==1 || status==11 || status==81;
    if (!modes[mode].rerun || faulted || !clean) {
        return 0;
    }
    char *again[8] = {0};
    i32   n        = 0;
    again[n++] = "-n";
    for (i32 i = 0; i < argc; i++) {
        if (argv[i][0]=='-' && argv[i][1]!='d' && argv[i][1]!='x' &&
            argv[i][1]) {
            continue;  // the options, but -d, -x, and - for stdin
        }
        again[n++] = argv[i];
    }
    if (mode==3 || mode==7 || mode==10) {
        again[0] = mode==3 ? "-nj" : mode==7 ? "-nV" : "-nDDC";
    }
    ctx->when = 0;
    ctx->shrinkto = ctx->failreadat = ctx->failwriteat = -1;
    ctx->failmkdir = ctx->failcreate = ctx->failkeep = ctx->failattrs = 0;
    ctx->failsymlink = 0;
    ctx->inoff = 0;
    before = mfs_snapshot(ctx);
    status = unzipos_run(ctx, again, n, 0, &open);
    if (getenv("FUZZ_UNZIP_SHOW")) {
        s8 out = unzipos_output(ctx, 3);
        fprintf(stderr, "again: status %d, %d changes\n%.*s", status,
                ctx->nchanges, (int)MIN(out.len, 1<<12), out.s);
    }
    CHECK(!open);
    CHECK(mfs_unchanged(ctx, before, (s8){0}));
    mfs_snapfree(before);
    return 0;
}
