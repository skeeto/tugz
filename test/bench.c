// Benchmark platform layer: in-memory gzip compression and decompression
// throughput versus zlib and libdeflate, all producing the gzip format.
// $ cc -O2 -o bench test/bench.c -lz -ldeflate
// $ cc -O2 -fno-builtin -DBENCH_REF=0 -o bench test/bench.c  # tugz only
// $ ./bench [-o] [-d] [-l LEVELS] [-s SIZES] [-t SECONDS] FILE...
//
// LEVELS is a comma-separated list (default 1,6,9). -o times only our
// codec. -d times only decompression: each stream is still made once,
// and checked, but its compression is not timed. Built with BENCH_REF=0 there is no zlib or libdeflate at all,
// so it builds anywhere the codec does (w64devkit: cc -O2 -fno-builtin
// -o bench.exe test/bench.c). BENCH_REF defaults to 1 only where both
// zlib.h and libdeflate.h can be included.
// Each figure is the best single run of at least three that together
// take at least SECONDS (default 0.5).
//
// Every level's output of ours is checked by a round trip through our
// inflate. Decompression is timed on these streams:
//   z6  zlib's level 6 output, by each decoder (only with references)
//   o1  our level 1 output, by ours (only when level 1 is requested)
//   o6  our level 6 output, by ours (always)
//   o9  our level 9 output, by ours (only when level 9 is requested)
//
// Besides the human table, it prints machine-readable lines, each a
// '#' word followed by space-separated fields:
//   #crc FILE LEVEL SIZE CRC          our output for a file and level:
//                                     its size and CRC-32 (8 hex digits)
//   #total cLEVEL CODEC IN OUT SECS   compression of all files: input and
//                                     output bytes, summed best seconds
//   #total dSRC CODEC BYTES SECS      decompression of all files' SRC
//                                     streams (above): output bytes, secs
// CODEC is ours, zlib or libdeflate. Speed is bytes/secs of input for
// compression, and of output for decompression.
//
// -s SIZES, a comma-separated list of sizes, times small streams of
// ours instead, and nothing else: each file is cut into pieces of each
// size (the last one shorter), each piece compressed at each level as a
// gzip stream of its own by one encoder, reset between streams as the
// gzip program resets its own across files, and the streams decoded by
// one decoder, reset between streams as gzip and unzip reset theirs.
// The lines it prints instead, with N the size:
//   #crc FILE:sN LEVEL SIZE CRC       the file's streams back to back
//   #total cLEVELsN ours IN OUT SECS  compression of all files' pieces
//   #total doLEVELsN ours BYTES SECS  decompression of all their streams
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/adler32.c"
#include "../src/inflate.c"
#include "../src/deflate.c"
#include "../src/gzip.c"
#include "../src/io.c"
#include "../src/gzipio.c"

#ifndef BENCH_REF
#  if __has_include(<zlib.h>) && __has_include(<libdeflate.h>)
#    define BENCH_REF 1
#  else
#    define BENCH_REF 0
#  endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if BENCH_REF
#  include <libdeflate.h>
#  include <zlib.h>
#endif
#ifndef _WIN32
#  include <time.h>
#endif

struct os {
    u8 const *in;
    iz        inlen;
    iz        inoff;
    u8       *out;
    iz        outlen;
    iz        outcap;
};

static i32 os_open(os *ctx, s8 path, i32 mode, arena scratch)
{
    (void)ctx; (void)path; (void)mode; (void)scratch;
    __builtin_trap();
}

static b32 os_close(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    __builtin_trap();
}

static b32 os_keep(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    __builtin_trap();
}

static b32 os_isatty(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    __builtin_trap();
}

static i64 os_now(os *ctx)
{
    (void)ctx;
    __builtin_trap();  // gzip's writers are never a terminal's
}

static b32 os_missing(os *ctx)
{
    (void)ctx;
    __builtin_trap();
}

static s8 os_error(os *ctx)
{
    (void)ctx;
    __builtin_trap();
}

static b32 os_pipeclosed(os *ctx)
{
    (void)ctx;
    __builtin_trap();
}

static osmeta *os_getmeta(os *ctx, i32 fd, arena *a)
{
    (void)ctx; (void)fd; (void)a;
    __builtin_trap();
}

static b32 os_setmeta(os *ctx, i32 fd, osmeta *m)
{
    (void)ctx; (void)fd; (void)m;
    __builtin_trap();
}

static b32 os_remove(os *ctx, s8 path, arena scratch)
{
    (void)ctx; (void)path; (void)scratch;
    __builtin_trap();
}

static iz os_read(os *ctx, i32 fd, u8 *buf, iz cap)
{
    (void)fd;
    iz n = MIN(cap, ctx->inlen - ctx->inoff);
    memcpy(buf, ctx->in + ctx->inoff, (uz)n);
    ctx->inoff += n;
    return n;
}

static b32 os_write(os *ctx, i32 fd, u8 *buf, iz len)
{
    if (fd == 2) {
        fwrite(buf, 1, (uz)len, stderr);
        return 1;
    } else if (len > ctx->outcap - ctx->outlen) {
        return 0;
    }
    memcpy(ctx->out + ctx->outlen, buf, (uz)len);
    ctx->outlen += len;
    return 1;
}

static void os_exit(os *ctx, i32 status)
{
    (void)ctx;
    (void)status;
    abort();
}

#ifdef _WIN32
__declspec(dllimport) i32 __stdcall QueryPerformanceCounter(i64 *);
__declspec(dllimport) i32 __stdcall QueryPerformanceFrequency(i64 *);

static double now(void)
{
    i64 t, f;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t / (double)f;
}
#else
static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}
#endif

[[noreturn]] static void fail(char const *what, char const *file, i32 level)
{
    fflush(stdout);
    fprintf(stderr, "\nbench: %s: %s, level %d\n", what, file, level);
    abort();
}

typedef struct {
    os          ctx;
    arena       perm;
    u8         *in;
    iz          len;
    u8         *out;
    iz          cap;
    u8         *back;  // decompression output
    char const *name;
} bench;

enum { OURS, ZLIB, LIBDEFLATE, NCODECS };
static char const *const codec_names[] = {"ours", "zlib", "libdeflate"};

static iz compress_with(bench *b, i32 codec, i32 level)
{
    switch (codec) {
    case OURS: {
        b->ctx.in = b->in;
        b->ctx.inlen = b->len;
        b->ctx.inoff = 0;
        b->ctx.out = b->out;
        b->ctx.outlen = 0;
        b->ctx.outcap = b->cap;
        arena a = b->perm;
        if (gzip_compress(gzip_encoder(&a, level), 0, 1, level, a)) {
            fail("compression failed", b->name, level);
        }
        return b->ctx.outlen;
    }
#if BENCH_REF
    case ZLIB: {
        z_stream z = {0};
        deflateInit2(&z, level, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
        z.next_in = b->in;
        z.avail_in = (u32)b->len;
        z.next_out = b->out;
        z.avail_out = (u32)b->cap;
        if (deflate(&z, Z_FINISH) != Z_STREAM_END) {
            fail("zlib compression failed", b->name, level);
        }
        iz r = (iz)z.total_out;
        deflateEnd(&z);
        return r;
    }
    case LIBDEFLATE: {
        struct libdeflate_compressor *c = libdeflate_alloc_compressor(level);
        uz r = libdeflate_gzip_compress(c, b->in, (uz)b->len, b->out,
                                        (uz)b->cap);
        libdeflate_free_compressor(c);
        if (!r) {
            fail("libdeflate compression failed", b->name, level);
        }
        return (iz)r;
    }
#endif
    }
    abort();
}

// Decompress into b->back, which must then match the input.
static void decompress_with(bench *b, i32 codec, u8 const *z, iz zlen)
{
    switch (codec) {
    case OURS: {
        b->ctx.in = z;
        b->ctx.inlen = zlen;
        b->ctx.inoff = 0;
        b->ctx.out = b->back;
        b->ctx.outlen = 0;
        b->ctx.outcap = b->len;
        arena a = b->perm;
        if (stream_decompress(stream_decoder(&a, FMT_GZIP), 0, 1, 0, a) ||
            b->ctx.outlen!=b->len) {
            fail("decompression failed", b->name, 0);
        }
        break;
    }
#if BENCH_REF
    case ZLIB: {
        z_stream s = {0};
        inflateInit2(&s, 31);
        s.next_in = (u8 *)z;
        s.avail_in = (u32)zlen;
        s.next_out = b->back;
        s.avail_out = (u32)b->len;
        if (inflate(&s, Z_FINISH)!=Z_STREAM_END || s.total_out!=(uLong)b->len) {
            fail("zlib decompression failed", b->name, 0);
        }
        inflateEnd(&s);
        break;
    }
    case LIBDEFLATE: {
        struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
        uz actual;
        if (libdeflate_gzip_decompress(d, z, (uz)zlen, b->back, (uz)b->len,
                                       &actual) != LIBDEFLATE_SUCCESS) {
            fail("libdeflate decompression failed", b->name, 0);
        }
        libdeflate_free_decompressor(d);
        break;
    }
#endif
    default:
        abort();
    }
}

static double mbps(double len, double secs)
{
    return len / secs / 1e6;
}

// Best time of repeated runs totaling at least mintime seconds.
#define TIMEIT(best, mintime, stmt) \
    do { \
        best = 1e30; \
        double total_ = 0; \
        for (i32 n_ = 0; n_<3 || total_<(mintime); n_++) { \
            double t0_ = now(); \
            stmt; \
            double dt_ = now() - t0_; \
            total_ += dt_; \
            best = dt_<best ? dt_ : best; \
        } \
    } while (0)

static u8 *loadfile(char const *path, iz *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *p = malloc((uz)*len + 1);
    *len = (iz)fread(p, 1, (uz)*len, f);
    fclose(f);
    return p;
}

// Small streams (-s): one file's pieces of a size, their gzip streams
// back to back.
typedef struct {
    u8 *z;
    iz *off;   // stream k is z[off[k]] up to z[off[k+1]]
    iz  n;     // pieces
    iz  size;  // of each piece but the last
    iz  cap;
} pieces;

// Compress every piece as a stream of its own at a level, with an
// encoder that gzip_compress resets for each.
static void compress_pieces(bench *b, encoder *e, arena scratch, pieces *p,
                            i32 level)
{
    iz zlen = 0;
    p->off[0] = 0;
    for (iz k = 0; k < p->n; k++) {
        iz start = k * p->size;
        b->ctx.in = b->in + start;
        b->ctx.inlen = MIN(p->size, b->len - start);
        b->ctx.inoff = 0;
        b->ctx.out = p->z + zlen;
        b->ctx.outlen = 0;
        b->ctx.outcap = p->cap - zlen;
        if (gzip_compress(e, 0, 1, level, scratch)) {
            fail("compression failed", b->name, level);
        }
        zlen += b->ctx.outlen;
        p->off[k+1] = zlen;
    }
}

// Decompress every stream into b->back, with a decoder that
// stream_decompress resets for each.
static void decompress_pieces(bench *b, decoder *d, arena scratch,
                              pieces *p)
{
    for (iz k = 0; k < p->n; k++) {
        iz start = k * p->size;
        iz len = MIN(p->size, b->len - start);
        b->ctx.in = p->z + p->off[k];
        b->ctx.inlen = p->off[k+1] - p->off[k];
        b->ctx.inoff = 0;
        b->ctx.out = b->back + start;
        b->ctx.outlen = 0;
        b->ctx.outcap = len;
        if (stream_decompress(d, 0, 1, 0, scratch) || b->ctx.outlen!=len) {
            fail("decompression failed", b->name, 0);
        }
    }
}

static i32 small_streams(char **files, i32 nfiles, i32 const *levels,
                         i32 nlevels, iz const *sizes, i32 nsizes,
                         double mintime, b32 dectimeonly)
{
    bench b = {0};
    iz cap = (iz)1 << 26;
    b.perm.beg = malloc((uz)cap);
    b.perm.end = b.perm.beg + cap;
    b.perm.ctx = &b.ctx;
    arena a = b.perm;
    encoder *e = gzip_encoder(&a, levels[0]);
    decoder *d = stream_decoder(&a, FMT_GZIP);

    enum { MAXL = 10, MAXS = 8 };
    double ctime[MAXL][MAXS] = {{0}};
    double dtime[MAXL][MAXS] = {{0}};
    double csize[MAXL][MAXS] = {{0}};
    double total = 0;

    printf("%-12s %10s", "file", "size");
    for (i32 l = 0; l < nlevels; l++) {
        for (i32 j = 0; j < nsizes; j++) {
            char col[32];
            snprintf(col, sizeof(col), "%ds%lld", levels[l],
                     (long long)sizes[j]);
            printf("  %-18s", col);
        }
    }
    printf("  (ratio, compress and decompress MB/s)\n");

    for (i32 f = 0; f < nfiles; f++) {
        b.in = loadfile(files[f], &b.len);
        if (!b.in) {
            fprintf(stderr, "bench: cannot read %s\n", files[f]);
            return 1;
        }
        b.back = malloc((uz)b.len + 1);
        total += (double)b.len;
        char const *name = files[f];
        for (char const *q = files[f]; *q; q++) {
            if (*q=='/' || *q=='\\') {
                name = q + 1;
            }
        }
        b.name = name;
        printf("%-12.12s %10.0f", name, (double)b.len);
        fflush(stdout);

        u32 crcs[MAXL][MAXS];
        iz  zsize[MAXL][MAXS];
        for (i32 l = 0; l < nlevels; l++) {
            for (i32 j = 0; j < nsizes; j++) {
                pieces p = {0};
                p.size = sizes[j];
                p.n = (b.len + p.size - 1) / p.size;
                p.cap = b.len + 64*p.n + (1<<16);
                p.z = malloc((uz)p.cap);
                p.off = malloc(sizeof(*p.off) * (uz)(p.n + 1));

                double best = 0;
                if (dectimeonly) {
                    compress_pieces(&b, e, a, &p, levels[l]);
                } else {
                    TIMEIT(best, mintime,
                           compress_pieces(&b, e, a, &p, levels[l]));
                }
                ctime[l][j] += best;
                iz zlen = p.off[p.n];
                csize[l][j] += (double)zlen;
                i32 cpu = 0;
                crcs[l][j] = crc32_update(0, p.z, zlen, &cpu);
                zsize[l][j] = zlen;

                double dbest;
                TIMEIT(dbest, mintime, decompress_pieces(&b, d, a, &p));
                if (memcmp(b.back, b.in, (uz)b.len)) {
                    fail("small-stream round trip mismatch", name, levels[l]);
                }
                dtime[l][j] += dbest;
                printf("  %5.1f%% %5.0f %5.0f",
                       100.0*(double)zlen/(double)b.len,
                       dectimeonly ? 0.0 : mbps((double)b.len, best),
                       mbps((double)b.len, dbest));
                fflush(stdout);
                free(p.off);
                free(p.z);
            }
        }
        printf("\n");
        for (i32 l = 0; l < nlevels; l++) {
            for (i32 j = 0; j < nsizes; j++) {
                printf("#crc %s:s%lld %d %.0f %08x\n", name,
                       (long long)sizes[j], levels[l],
                       (double)zsize[l][j], (unsigned)crcs[l][j]);
            }
        }
        free(b.in);
        free(b.back);
    }

    for (i32 l = 0; l < nlevels; l++) {
        for (i32 j = 0; j < nsizes; j++) {
            if (!dectimeonly) {
                printf("#total c%ds%lld ours %.0f %.0f %.6f\n", levels[l],
                       (long long)sizes[j], total, csize[l][j],
                       ctime[l][j]);
            }
        }
    }
    for (i32 l = 0; l < nlevels; l++) {
        for (i32 j = 0; j < nsizes; j++) {
            printf("#total do%ds%lld ours %.0f %.6f\n", levels[l],
                   (long long)sizes[j], total, dtime[l][j]);
        }
    }
    return 0;
}

// A decompression source: a stream, and the decoders that time it.
typedef struct {
    char  name[4];
    i32   level;    // 0 for zlib's level 6 stream
    i32   ncodecs;
    u8   *z;
    iz    zlen;
    double time[NCODECS];
} source;

int main(int argc, char **argv)
{
    i32 levels[10] = {1, 6, 9};
    i32 nlevels = 3;
    double mintime = 0.5;
    b32 oursonly = 0;
    b32 dectimeonly = 0;
    iz  sizes[8];
    i32 nsizes = 0;

    i32 i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-l") && i+1<argc) {
            nlevels = 0;
            for (char *p = argv[++i]; *p && nlevels<10;) {
                levels[nlevels++] = (i32)strtol(p, &p, 10);
                p += *p == ',';
            }
        } else if (!strcmp(argv[i], "-t") && i+1<argc) {
            mintime = strtod(argv[++i], 0);
        } else if (!strcmp(argv[i], "-o")) {
            oursonly = 1;
        } else if (!strcmp(argv[i], "-d")) {
            dectimeonly = 1;
        } else if (!strcmp(argv[i], "-s") && i+1<argc) {
            nsizes = 0;
            for (char *p = argv[++i]; *p && nsizes<8;) {
                sizes[nsizes++] = (iz)strtoll(p, &p, 10);
                p += *p == ',';
            }
        } else {
            fprintf(stderr, "usage: bench [-o] [-d] [-l LEVELS] [-s SIZES] "
                    "[-t SECONDS] FILE...\n");
            return 1;
        }
    }
    for (i32 j = 0; j < nsizes; j++) {
        if (sizes[j] < 1) {
            fprintf(stderr, "bench: invalid size %lld\n",
                    (long long)sizes[j]);
            return 1;
        }
    }
    for (i32 l = 0; l < nlevels; l++) {
        if (levels[l]<1 || levels[l]>9) {
            fprintf(stderr, "bench: invalid level %d\n", levels[l]);
            return 1;
        }
    }
    if (nsizes) {
        return small_streams(argv+i, argc-i, levels, nlevels, sizes, nsizes,
                             mintime, dectimeonly);
    }
    i32 ncodecs = BENCH_REF && !oursonly ? NCODECS : 1;

    source srcs[4] = {0};
    i32 nsrcs = 0;
    if (BENCH_REF) {
        srcs[nsrcs++] = (source){"z6", 0, ncodecs, 0, 0, {0}};
    }
    for (i32 l = 0; l < nlevels; l++) {
        if (levels[l] == 1) {
            srcs[nsrcs++] = (source){"o1", 1, 1, 0, 0, {0}};
            break;
        }
    }
    srcs[nsrcs++] = (source){"o6", 6, 1, 0, 0, {0}};
    for (i32 l = 0; l < nlevels; l++) {
        if (levels[l] == 9) {
            srcs[nsrcs++] = (source){"o9", 9, 1, 0, 0, {0}};
            break;
        }
    }

    bench b = {0};
    iz cap = (iz)1 << 26;
    b.perm.beg = malloc((uz)cap);
    b.perm.end = b.perm.beg + cap;
    b.perm.ctx = &b.ctx;

    double ctime[10][NCODECS] = {{0}};
    double csize[10][NCODECS] = {{0}};
    u32    crcs[10] = {0};  // per file: our output's CRC and size
    iz     osize[10] = {0};
    double total = 0;

    printf("%-12s %10s", "file", "size");
    for (i32 l = 0; l < nlevels; l++) {
        for (i32 c = 0; c < ncodecs; c++) {
            char col[16];
            snprintf(col, sizeof(col), "%c%d:%s",
                     "OZL"[c], levels[l], codec_names[c]);
            printf("  %-12.12s", col);
        }
    }
    printf("  ");
    for (i32 s = 0; s < nsrcs; s++) {
        for (i32 c = 0; c < srcs[s].ncodecs; c++) {
            printf(" %4s:%c", srcs[s].name, "OZL"[c]);
        }
    }
    printf("  (decompress MB/s)\n");

    for (; i < argc; i++) {
        b.in = loadfile(argv[i], &b.len);
        if (!b.in) {
            fprintf(stderr, "bench: cannot read %s\n", argv[i]);
            return 1;
        }
        b.cap = b.len + b.len/8 + (1<<16);
        b.out = malloc((uz)b.cap);
        b.back = malloc((uz)b.len + 1);
        total += (double)b.len;

        char const *name = argv[i];
        for (char const *p = argv[i]; *p; p++) {
            if (*p=='/' || *p=='\\') {
                name = p + 1;
            }
        }
        b.name = name;
        printf("%-12.12s %10.0f", name, (double)b.len);
        fflush(stdout);

        for (i32 l = 0; l < nlevels; l++) {
            for (i32 c = 0; c < ncodecs; c++) {
                iz zlen = 0;
                double best = 0;
                if (dectimeonly) {
                    zlen = compress_with(&b, c, levels[l]);
                } else {
                    TIMEIT(best, mintime,
                           zlen = compress_with(&b, c, levels[l]));
                }
                ctime[l][c] += best;
                csize[l][c] += (double)zlen;
                printf("  %5.1f%% %5.0f", 100.0*(double)zlen/(double)b.len,
                       dectimeonly ? 0.0 : mbps((double)b.len, best));
                fflush(stdout);
                if (c != OURS) {
                    continue;
                }

                // Identity, a round trip, and a copy to time decoding
                i32 cpu = 0;
                crcs[l] = crc32_update(0, b.out, zlen, &cpu);
                osize[l] = zlen;
                decompress_with(&b, OURS, b.out, zlen);
                if (memcmp(b.back, b.in, (uz)b.len)) {
                    fail("round trip mismatch", name, levels[l]);
                }
                for (i32 s = 0; s < nsrcs; s++) {
                    if (srcs[s].level==levels[l] && !srcs[s].z) {
                        srcs[s].z = malloc((uz)zlen);
                        srcs[s].zlen = zlen;
                        memcpy(srcs[s].z, b.out, (uz)zlen);
                    }
                }
            }
        }

        // Streams for decompression not already made above
        for (i32 s = 0; s < nsrcs; s++) {
            if (!srcs[s].z) {
                i32 codec = srcs[s].level ? OURS : ZLIB;
                i32 level = srcs[s].level ? srcs[s].level : 6;
                srcs[s].zlen = compress_with(&b, codec, level);
                srcs[s].z = malloc((uz)srcs[s].zlen);
                memcpy(srcs[s].z, b.out, (uz)srcs[s].zlen);
            }
        }

        printf("  ");
        for (i32 s = 0; s < nsrcs; s++) {
            for (i32 c = 0; c < srcs[s].ncodecs; c++) {
                double best;
                TIMEIT(best, mintime,
                       decompress_with(&b, c, srcs[s].z, srcs[s].zlen));
                if (memcmp(b.back, b.in, (uz)b.len)) {
                    fail("decompression mismatch", name, srcs[s].level);
                }
                srcs[s].time[c] += best;
                printf(" %6.0f", mbps((double)b.len, best));
                fflush(stdout);
            }
            free(srcs[s].z);
            srcs[s].z = 0;
        }
        printf("\n");

        for (i32 l = 0; l < nlevels; l++) {
            printf("#crc %s %d %.0f %08x\n", name, levels[l],
                   (double)osize[l], (unsigned)crcs[l]);
        }
        free(b.in);
        free(b.out);
        free(b.back);
    }

    printf("%-12s %10.0f", "TOTAL", total);
    for (i32 l = 0; l < nlevels; l++) {
        for (i32 c = 0; c < ncodecs; c++) {
            printf("  %5.1f%% %5.0f", 100.0*csize[l][c]/total,
                   dectimeonly ? 0.0 : mbps(total, ctime[l][c]));
        }
    }
    printf("  ");
    for (i32 s = 0; s < nsrcs; s++) {
        for (i32 c = 0; c < srcs[s].ncodecs; c++) {
            printf(" %6.0f", mbps(total, srcs[s].time[c]));
        }
    }
    printf("\n");

    for (i32 l = 0; l < nlevels && !dectimeonly; l++) {
        for (i32 c = 0; c < ncodecs; c++) {
            printf("#total c%d %s %.0f %.0f %.6f\n", levels[l],
                   codec_names[c], total, csize[l][c], ctime[l][c]);
        }
    }
    for (i32 s = 0; s < nsrcs; s++) {
        for (i32 c = 0; c < srcs[s].ncodecs; c++) {
            printf("#total d%s %s %.0f %.6f\n", srcs[s].name,
                   codec_names[c], total, srcs[s].time[c]);
        }
    }
    return 0;
}
