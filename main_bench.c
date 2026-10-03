// Benchmark platform layer: in-memory gzip compression and decompression
// throughput versus zlib and libdeflate, all producing the gzip format.
// $ cc -O2 -o bench main_bench.c -lz -ldeflate
// $ ./bench [-l LEVELS] [-t SECONDS] FILE...
// LEVELS is a comma-separated list (default 1,6,9). Decompression speed
// is measured by each decoder on the same zlib level 6 stream.
#include "src/base.c"
#include "src/crc32.c"
#include "src/inflate.c"
#include "src/deflate.c"
#include "src/gzip.c"

#include <libdeflate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

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

static void os_keep(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    __builtin_trap();
}

static b32 os_isatty(os *ctx, i32 fd)
{
    (void)ctx; (void)fd;
    __builtin_trap();
}

static void os_copymeta(os *ctx, i32 from, i32 to)
{
    (void)ctx; (void)from; (void)to;
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

static void os_fail(os *ctx)
{
    (void)ctx;
    abort();
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}

typedef struct {
    os    ctx;
    arena perm;
    u8   *in;
    iz    len;
    u8   *out;
    iz    cap;
    u8   *back;  // decompression output
} bench;

enum { OURS, ZLIB, LIBDEFLATE, NCODECS };
static char const *const codec_names[] = {"ours", "zlib", "libdeflate"};

static iz compress_with(bench *b, i32 codec, i32 level)
{
    switch (codec) {
    case OURS:
        b->ctx.in = b->in;
        b->ctx.inlen = b->len;
        b->ctx.inoff = 0;
        b->ctx.out = b->out;
        b->ctx.outlen = 0;
        b->ctx.outcap = b->cap;
        if (gzip_compress(0, 1, level, b->perm)) {
            abort();
        }
        return b->ctx.outlen;
    case ZLIB: {
        z_stream z = {0};
        deflateInit2(&z, level, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
        z.next_in = b->in;
        z.avail_in = (u32)b->len;
        z.next_out = b->out;
        z.avail_out = (u32)b->cap;
        if (deflate(&z, Z_FINISH) != Z_STREAM_END) {
            abort();
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
            abort();
        }
        return (iz)r;
    }
    }
    abort();
}

static void decompress_with(bench *b, i32 codec, u8 const *z, iz zlen)
{
    switch (codec) {
    case OURS:
        b->ctx.in = z;
        b->ctx.inlen = zlen;
        b->ctx.inoff = 0;
        b->ctx.out = b->back;
        b->ctx.outlen = 0;
        b->ctx.outcap = b->len;
        if (gzip_decompress(0, 1, b->perm) || b->ctx.outlen!=b->len) {
            abort();
        }
        break;
    case ZLIB: {
        z_stream s = {0};
        inflateInit2(&s, 31);
        s.next_in = (u8 *)z;
        s.avail_in = (u32)zlen;
        s.next_out = b->back;
        s.avail_out = (u32)b->len;
        if (inflate(&s, Z_FINISH)!=Z_STREAM_END || s.total_out!=(uLong)b->len) {
            abort();
        }
        inflateEnd(&s);
        break;
    }
    case LIBDEFLATE: {
        struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
        uz actual;
        if (libdeflate_gzip_decompress(d, z, (uz)zlen, b->back, (uz)b->len,
                                       &actual) != LIBDEFLATE_SUCCESS) {
            abort();
        }
        libdeflate_free_decompressor(d);
        break;
    }
    }
}

static double mbps(iz len, double secs)
{
    return (double)len / secs / 1e6;
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

int main(int argc, char **argv)
{
    i32 levels[10] = {1, 6, 9};
    i32 nlevels = 3;
    double mintime = 0.5;

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
        } else {
            fprintf(stderr, "usage: bench [-l LEVELS] [-t SECONDS] FILE...\n");
            return 1;
        }
    }

    bench b = {0};
    iz cap = (iz)1 << 26;
    b.perm.beg = malloc((uz)cap);
    b.perm.end = b.perm.beg + cap;
    b.perm.ctx = &b.ctx;

    double ctime[10][NCODECS] = {{0}};
    double dtime[NCODECS] = {0};
    iz csize[10][NCODECS] = {{0}};
    iz total = 0;

    printf("%-12s %10s", "file", "size");
    for (i32 l = 0; l < nlevels; l++) {
        for (i32 c = 0; c < NCODECS; c++) {
            printf("  %c%d:%-10.10s", "OZL"[c], levels[l], codec_names[c]);
        }
    }
    printf("   decompress MB/s (O/Z/L)\n");

    for (; i < argc; i++) {
        b.in = loadfile(argv[i], &b.len);
        if (!b.in) {
            fprintf(stderr, "bench: cannot read %s\n", argv[i]);
            return 1;
        }
        b.cap = b.len + b.len/8 + (1<<16);
        b.out = malloc((uz)b.cap);
        b.back = malloc((uz)b.len + 1);
        total += b.len;

        char const *name = strrchr(argv[i], '/');
        name = name ? name+1 : argv[i];
        printf("%-12.12s %10td", name, b.len);
        fflush(stdout);

        for (i32 l = 0; l < nlevels; l++) {
            for (i32 c = 0; c < NCODECS; c++) {
                iz zlen = 0;
                double best;
                TIMEIT(best, mintime, zlen = compress_with(&b, c, levels[l]));
                ctime[l][c] += best;
                csize[l][c] += zlen;
                printf("  %5.1f%% %5.0f", 100.0*(double)zlen/(double)b.len,
                       mbps(b.len, best));
                fflush(stdout);
            }
        }

        // Decompression: every decoder on the same zlib -6 stream
        iz zlen = compress_with(&b, ZLIB, 6);
        u8 *z = malloc((uz)zlen);
        memcpy(z, b.out, (uz)zlen);
        printf("  ");
        for (i32 c = 0; c < NCODECS; c++) {
            double best;
            TIMEIT(best, mintime, decompress_with(&b, c, z, zlen));
            if (memcmp(b.back, b.in, (uz)b.len)) {
                abort();
            }
            dtime[c] += best;
            printf(" %6.0f", mbps(b.len, best));
        }
        printf("\n");
        free(z);
        free(b.in);
        free(b.out);
        free(b.back);
    }

    printf("%-12s %10td", "TOTAL", total);
    for (i32 l = 0; l < nlevels; l++) {
        for (i32 c = 0; c < NCODECS; c++) {
            printf("  %5.1f%% %5.0f",
                   100.0*(double)csize[l][c]/(double)total,
                   mbps(total, ctime[l][c]));
        }
    }
    printf("  ");
    for (i32 c = 0; c < NCODECS; c++) {
        printf(" %6.0f", mbps(total, dtime[c]));
    }
    printf("\n");
    return 0;
}
