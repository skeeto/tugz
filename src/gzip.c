#include "gzip.h"
#include "deflate.h"
#include "inflate.h"
#include "crc32.h"

static const u8 gz_header[10] = {
    0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3
};

#define RDBUF (1 << 18)
#define WRBUF (1 << 20)
#define CHUNK (1 << 20)

b32 gzip_compress_handle(iptr in, iptr out, i32 level)
{
    arena_mark m = arena_save();

    reader rd;
    writer wr;
    reader_init(&rd, in, arena_alloc(RDBUF, 64), RDBUF);
    writer_init(&wr, out, arena_alloc(WRBUF, 64), WRBUF, 0);

    deflate *d = deflate_create(level, &wr);
    u8 *chunk = arena_alloc(CHUNK, 64);

    writer_write(&wr, gz_header, sizeof(gz_header));

    u32 crc = 0xffffffffu;
    u64 total = 0;
    for (;;) {
        iz n = reader_read(&rd, chunk, CHUNK);
        if (n == 0) {
            break;
        }
        crc = crc32_update(crc, chunk, n);
        total += (u64)n;
        deflate_push(d, chunk, n);
    }

    deflate_end(d);

    u8 trailer[8];
    u32 c = ~crc;
    trailer[0] = (u8)(c);
    trailer[1] = (u8)(c >> 8);
    trailer[2] = (u8)(c >> 16);
    trailer[3] = (u8)(c >> 24);
    trailer[4] = (u8)(total);
    trailer[5] = (u8)(total >> 8);
    trailer[6] = (u8)(total >> 16);
    trailer[7] = (u8)(total >> 24);
    writer_write(&wr, trailer, 8);
    writer_flush(&wr);

    b32 ok = !wr.err && !rd.err;
    arena_restore(m);
    return ok;
}

static b32 read_exact(reader *r, u8 *buf, iz n)
{
    return reader_read(r, buf, n) == n;
}

static b32 skip_until_zero(reader *r)
{
    for (;;) {
        i32 c = reader_byte(r);
        if (c < 0) {
            return 0;
        }
        if (c == 0) {
            return 1;
        }
    }
}

b32 gzip_decompress_handle(iptr in, iptr out, b32 verify_only)
{
    arena_mark m = arena_save();

    reader rd;
    writer wr;
    reader_init(&rd, in, arena_alloc(RDBUF, 64), RDBUF);
    writer_init(&wr, out, arena_alloc(WRBUF, 64), WRBUF, verify_only);

    b32 ok = 1;
    b32 any = 0;

    for (;;) {
        u8 hdr[10];
        iz got = reader_read(&rd, hdr, 10);
        if (got == 0) {
            break;
        }
        if (got < 10) {
            if (got >= 2 && hdr[0] == 0x1f && hdr[1] == 0x8b) {
                ok = 0;
            }
            break;
        }
        if (hdr[0] != 0x1f || hdr[1] != 0x8b) {
            if (any) {
                break;
            }
            ok = 0;
            break;
        }
        if (hdr[2] != 8) {
            ok = 0;
            break;
        }

        u8 flg = hdr[3];
        if (flg & 4) {
            u8 x[2];
            if (!read_exact(&rd, x, 2)) {
                ok = 0;
                break;
            }
            u32 xlen = (u32)x[0] | ((u32)x[1] << 8);
            for (u32 i = 0; i < xlen; i++) {
                if (reader_byte(&rd) < 0) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                break;
            }
        }
        if (flg & 8) {
            if (!skip_until_zero(&rd)) {
                ok = 0;
                break;
            }
        }
        if (flg & 16) {
            if (!skip_until_zero(&rd)) {
                ok = 0;
                break;
            }
        }
        if (flg & 2) {
            u8 x[2];
            if (!read_exact(&rd, x, 2)) {
                ok = 0;
                break;
            }
        }

        arena_mark mm = arena_save();
        inflate_state *s = inflate_create(&rd, &wr);
        b32 mok = inflate_stream(s);
        mok = inflate_finish(s) && mok;

        u8 tr[8];
        b32 have_trailer = 1;
        for (i32 i = 0; i < 8; i++) {
            i32 c = inflate_read_byte(s);
            if (c < 0) {
                have_trailer = 0;
                break;
            }
            tr[i] = (u8)c;
        }

        if (mok && have_trailer) {
            u32 crc = (u32)tr[0] | ((u32)tr[1] << 8) |
                      ((u32)tr[2] << 16) | ((u32)tr[3] << 24);
            u32 isize = (u32)tr[4] | ((u32)tr[5] << 8) |
                        ((u32)tr[6] << 16) | ((u32)tr[7] << 24);
            if (crc != inflate_crc(s) || isize != (u32)inflate_total(s)) {
                mok = 0;
            }
        } else {
            mok = 0;
        }
        arena_restore(mm);

        if (!mok) {
            ok = 0;
            break;
        }
        any = 1;
    }

    if (!any) {
        ok = 0;
    }
    if (!verify_only) {
        ok = writer_flush(&wr) && ok;
    }

    arena_restore(m);
    return ok;
}