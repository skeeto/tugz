// gzip core: gzip container format (RFC 1952)
//
// Decompression handles concatenated members. Following GNU gzip, data
// after the last member is ignored with a warning (GZ_TRAILING) unless it
// starts with the gzip magic, in which case it must be a valid member.

#define GZ_RDBUF  (1 << 18)
#define GZ_WRBUF  (1 << 20)

enum {
    FTEXT    = 1 << 0,
    FHCRC    = 1 << 1,
    FEXTRA   = 1 << 2,
    FNAME    = 1 << 3,
    FCOMMENT = 1 << 4,
};

static void put32le(u8 *p, u32 v)
{
    p[0] = (u8)(v >>  0);
    p[1] = (u8)(v >>  8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 get32le(u8 const *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}

static i32 gzip_compress(i32 in, i32 out, i32 level, arena scratch)
{
    reader  *r = newreader(&scratch, in, GZ_RDBUF);
    writer  *w = newwriter(&scratch, out, GZ_WRBUF);
    deflator *d = deflate_new(&scratch, level, w);

    static u8 const header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
    writer_write(w, header, countof(header));

    u32 crc = 0;
    u64 total = 0;
    while (reader_fill(r)) {
        u8 *p = r->buf + r->off;
        iz len = r->len - r->off;
        crc = crc32_update(crc, p, len);
        total += (u64)len;
        deflate_push(d, p, len);
        r->off = r->len;
    }
    deflate_finish(d);

    u8 trailer[8];
    put32le(trailer+0, crc);
    put32le(trailer+4, (u32)total);
    writer_write(w, trailer, countof(trailer));
    writer_flush(w);

    return r->err ? GZ_EREAD : w->err ? GZ_EWRITE : GZ_OK;
}

typedef struct {
    reader *r;
    u32     crc;
    b32     eof;
} hdrreader;

static i32 hdr_byte(hdrreader *h)
{
    i32 c = reader_byte(h->r);
    if (c < 0) {
        h->eof = 1;
        return 0;
    }
    u8 b = (u8)c;
    h->crc = crc32_update(h->crc, &b, 1);
    return c;
}

// Parse the remainder of a member header after the first 10 bytes.
static i32 gzip_header(reader *r, u8 *hdr)
{
    if (hdr[2] != 8) {
        return GZ_EMETHOD;
    }
    u8 flg = hdr[3];
    if (flg & 0xe0) {
        return GZ_EFLAGS;
    }

    hdrreader h = {r, crc32_update(0, hdr, 10), 0};
    if (flg & FEXTRA) {
        i32 xlen = hdr_byte(&h);
        xlen |= hdr_byte(&h) << 8;
        for (; xlen && !h.eof; xlen--) {
            hdr_byte(&h);
        }
    }
    if (flg & FNAME) {
        while (hdr_byte(&h) && !h.eof) {}
    }
    if (flg & FCOMMENT) {
        while (hdr_byte(&h) && !h.eof) {}
    }
    if (flg & FHCRC) {
        u32 want = h.crc & 0xffff;
        u32 got  = (u32)hdr_byte(&h);
        got |= (u32)hdr_byte(&h) << 8;
        if (!h.eof && got!=want) {
            return GZ_EHCRC;
        }
    }
    if (h.eof) {
        return r->err ? GZ_EREAD : GZ_ETRUNC;
    }
    return GZ_OK;
}

// Decompress all members. A negative output descriptor only verifies.
static i32 gzip_decompress(i32 in, i32 out, arena scratch)
{
    reader *r = newreader(&scratch, in, GZ_RDBUF);
    writer *w = newwriter(&scratch, out, GZ_WRBUF);

    i32 status = GZ_OK;
    for (b32 first = 1;; first = 0) {
        u8 hdr[10];
        iz got = reader_read(r, hdr, 10);
        b32 magic = got>=2 && hdr[0]==0x1f && hdr[1]==0x8b;
        if (r->err) {
            status = GZ_EREAD;
            break;
        } else if (!first && !magic) {
            status = got ? GZ_TRAILING : GZ_OK;
            break;
        } else if (first && !got) {
            status = GZ_ETRUNC;
            break;
        } else if (!magic) {
            status = GZ_ENOTGZ;
            break;
        } else if (got < 10) {
            status = GZ_ETRUNC;
            break;
        }

        status = gzip_header(r, hdr);
        if (status) {
            break;
        }

        arena temp = scratch;
        inflator *s = inflate_new(&temp, r, w);
        status = inflate_run(s);
        if (status) {
            break;
        }

        u8 trailer[8];
        i32 i = 0;
        for (; i < 8; i++) {
            i32 c = inflate_byte(s);
            if (c < 0) {
                break;
            }
            trailer[i] = (u8)c;
        }
        if (i < 8) {
            status = r->err ? GZ_EREAD : GZ_ETRUNC;
            break;
        } else if (get32le(trailer+0) != s->crc) {
            status = GZ_ECRC;
            break;
        } else if (get32le(trailer+4) != (u32)s->total) {
            status = GZ_ELEN;
            break;
        }
    }

    if (!writer_flush(w) && (!status || status==GZ_TRAILING)) {
        status = GZ_EWRITE;
    }
    return status;
}
