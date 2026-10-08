// tugz core: zlib (RFC 1950) and gzip (RFC 1952) containers around raw
// DEFLATE, as streaming decoders and encoders
//
// The gzip decoder handles one member at a time: after a member ends,
// the next call begins parsing another. Bytes that are not a gzip header
// produce GZ_ENOTGZ, leaving trailing data policy to the caller. As in
// GNU gzip, a header may begin with the old magic, 1f 9e, as well as 1f 8b.

enum {
    FMT_RAW,
    FMT_ZLIB,
    FMT_GZIP,
};

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

static u32 get32be(u8 const *p)
{
    return (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | (u32)p[3];
}

// The check of more data, with crc32_update's cache of the CPU's answer.
static u32 check_update(i32 format, u32 check, u8 const *p, iz len,
                        i32 *cpu)
{
    switch (format) {
    case FMT_ZLIB: return adler32_update(check, p, len);
    case FMT_GZIP: return crc32_update(check, p, len, cpu);
    }
    return 0;
}

static u32 check_init(i32 format)
{
    return format==FMT_ZLIB ? 1 : 0;
}

enum {
    DEC_FIXED,    // gzip: first 10 bytes; zlib: 2, and any dictionary ID
    DEC_XLEN,
    DEC_EXTRA,
    DEC_NAME,
    DEC_COMMENT,
    DEC_HCRC,
    DEC_BODY,
    DEC_TRAILER,
    DEC_DONE,
};

typedef struct {
    inflator *inf;
    i32 format;
    i32 state;
    i32 err;      // sticky container error
    u32 check;    // of the output so far
    u64 total;
    u8  buf[10];  // header or trailer bytes
    i32 len;      // bytes in buf
    i32 hpos;     // header bytes consumed, saturated at 2
    u32 hcrc;
    i32 flg;
    i32 xlen;
    i32 cpu;      // for crc32_update, kept across resets
} decoder;

static iz decoder_memsize(void)
{
    return (iz)sizeof(decoder) + 64 + inflate_memsize();
}

// Start a new stream (or gzip member) in the same format, from any state.
static void decoder_reset(decoder *z)
{
    inflate_reset(z->inf);
    z->state = z->format==FMT_RAW ? DEC_BODY : DEC_FIXED;
    z->check = check_init(z->format);
    z->total = 0;
    z->len   = 0;
    z->hpos  = 0;
    z->hcrc  = 0;
    z->err   = 0;
}

static decoder *decoder_new(arena *a, i32 format)
{
    decoder *z = new(a, 1, decoder);
    z->inf = inflate_new(a);
    z->format = format;
    decoder_reset(z);
    return z;
}

// Advance the gzip header state past fields the flags say are absent.
static void gzip_skip_absent(decoder *z)
{
    for (;;) {
        switch (z->state) {
        case DEC_XLEN:    if (z->flg & FEXTRA)   return; break;
        case DEC_EXTRA:   if (z->xlen)           return; break;
        case DEC_NAME:    if (z->flg & FNAME)    return; break;
        case DEC_COMMENT: if (z->flg & FCOMMENT) return; break;
        case DEC_HCRC:    if (z->flg & FHCRC)    return; break;
        default:          return;
        }
        z->state++;
    }
}

// Consume one byte of a fixed-size gzip header field: the first 10
// bytes, XLEN, or the header CRC. The header CRC is computed only when
// FHCRC asks for it to be checked.
static i32 gzip_header_byte(decoder *z, u8 c)
{
    z->hpos += z->hpos < 2;
    switch (z->state) {
    case DEC_FIXED:
        // Each field is validated as soon as it is complete, like zlib.
        // As in GNU gzip, the old magic, 1f 9e, also begins a member.
        z->buf[z->len++] = c;
        if (z->len==2 && (z->buf[0]!=0x1f ||
                          (z->buf[1]!=0x8b && z->buf[1]!=0x9e))) {
            return GZ_ENOTGZ;
        } else if (z->len==4 && z->buf[2]!=8) {
            return GZ_EMETHOD;
        } else if (z->len==4 && (z->buf[3] & 0xe0)) {
            return GZ_EFLAGS;
        } else if (z->len < 10) {
            return GZ_OK;
        }
        z->flg = z->buf[3];
        if (z->flg & FHCRC) {
            z->hcrc = crc32_update(0, z->buf, 10, &z->cpu);
        }
        z->len = 0;
        z->xlen = 0;
        z->state = DEC_XLEN;
        break;
    case DEC_XLEN:
        if (z->flg & FHCRC) {
            z->hcrc = crc32_update(z->hcrc, &c, 1, &z->cpu);
        }
        z->xlen |= c << 8*z->len++;
        if (z->len < 2) {
            return GZ_OK;
        }
        z->len = 0;
        z->state = DEC_EXTRA;
        break;
    case DEC_HCRC:
        z->buf[z->len++] = c;
        if (z->len < 2) {
            return GZ_OK;
        } else if ((z->buf[0] | z->buf[1]<<8) != (i32)(z->hcrc & 0xffff)) {
            return GZ_EHCRC;
        }
        z->state = DEC_BODY;
        break;
    }
    gzip_skip_absent(z);
    return GZ_OK;
}

// Consume as much of a variable gzip header field (the extra field, the
// name, or the comment) as b->in holds, at least a byte, in one span
// rather than byte by byte, as these may be large.
static void gzip_header_span(decoder *z, zbuf *b)
{
    iz  n = 0;
    b32 done;
    if (z->state == DEC_EXTRA) {
        n = MIN(z->xlen, b->inlen);
        z->xlen -= (i32)n;
        done = !z->xlen;
    } else {
        for (; n<b->inlen && b->in[n]; n++) {}
        done = n < b->inlen;
        n += done;  // the terminating zero
    }
    if (z->flg & FHCRC) {
        z->hcrc = crc32_update(z->hcrc, b->in, n, &z->cpu);
    }
    z->hpos = (i32)MIN(z->hpos+MIN(n, 2), 2);  // fields may pass 2^31
    b->in += n;
    b->inlen -= n;
    if (done) {
        z->state++;
        gzip_skip_absent(z);
    }
}

// Consume one zlib header byte. Validation follows zlib's order. Preset
// dictionaries (FDICT) are unsupported, but like zlib, which then asks
// for the dictionary, the decoder first reads its 4-byte ID, so that
// input ending sooner is truncation, as there.
static i32 zlib_header_byte(decoder *z, u8 c)
{
    z->hpos += z->hpos < 2;
    z->buf[z->len++] = c;
    if (z->len != 2) {
        return z->len<6 ? GZ_OK : GZ_EHEADER;  // CMF, or the dictionary ID
    }
    u32 cmf = z->buf[0];
    u32 flg = z->buf[1];
    if ((cmf<<8 | flg) % 31) {
        return GZ_EHEADER;
    } else if ((cmf & 15) != 8) {
        return GZ_EMETHOD;
    } else if ((cmf >> 4) > 7) {
        return GZ_EHEADER;  // window larger than 32 KiB
    } else if (flg & 0x20) {
        return GZ_OK;  // a preset dictionary: its ID follows
    }
    z->len = 0;
    z->state = DEC_BODY;
    return GZ_OK;
}

// Verify each trailer field as soon as it is complete, like zlib.
static i32 trailer_check(decoder *z)
{
    if (z->len != 4) {
        return get32le(z->buf+4)==(u32)z->total ? GZ_OK : GZ_ELEN;
    } else if (z->format == FMT_ZLIB) {
        return get32be(z->buf)==z->check ? GZ_OK : GZ_ECRC;
    }
    return get32le(z->buf)==z->check ? GZ_OK : GZ_ECRC;
}

// Consume only header bytes from b->in, so that a caller can learn
// whether a stream begins as it should before decoding any of it.
// Returns GZ_OK once the body is next, GZ_NEEDIN, or an error, which is
// sticky. decoder_run continues from there.
static i32 decoder_header(decoder *z, zbuf *b)
{
    while (!z->err && z->state<DEC_BODY) {
        if (!b->inlen) {
            return GZ_NEEDIN;
        } else if (z->state>=DEC_EXTRA && z->state<=DEC_COMMENT) {
            gzip_header_span(z, b);  // only gzip has these
            continue;
        }
        u8 c = *b->in++;
        b->inlen--;
        z->err = z->format==FMT_ZLIB ? zlib_header_byte(z, c)
                                     : gzip_header_byte(z, c);
    }
    return z->err;
}

// Decode from b->in into b->out, advancing both. Returns GZ_OK at the
// end of the stream (or gzip member), with b->in just past it. Calling
// again after a gzip member begins the next. Otherwise returns
// GZ_NEEDIN, GZ_NEEDOUT, or an error, which is sticky.
static i32 decoder_run(decoder *z, zbuf *b)
{
    if (z->err) {
        return z->err;
    } else if (z->state==DEC_DONE && z->format==FMT_GZIP) {
        decoder_reset(z);
    }

    for (;;) {
        switch (z->state) {
        case DEC_DONE:
            return GZ_OK;

        case DEC_BODY: {
            // Lengths come from the counts: both pointers may be null
            u8 *out   = b->out;
            iz  avail = b->outlen;
            i32 r = inflate_stream(z->inf, b);
            iz  n = avail - b->outlen;
            if (z->format != FMT_RAW) {
                z->check = check_update(z->format, z->check, out, n,
                                        &z->cpu);
            }
            z->total += (u64)n;
            if (r != GZ_OK) {
                return r;
            }
            z->state = z->format==FMT_RAW ? DEC_DONE : DEC_TRAILER;
            z->len = 0;
        } break;

        case DEC_TRAILER: {
            i32 need = z->format==FMT_ZLIB ? 4 : 8;
            while (z->len < need) {
                if (!b->inlen) {
                    return GZ_NEEDIN;
                }
                z->buf[z->len++] = *b->in++;
                b->inlen--;
                if (z->len==4 || z->len==8) {
                    z->err = trailer_check(z);
                    if (z->err) {
                        return z->err;
                    }
                }
            }
            z->state = DEC_DONE;
        } break;

        default: {
            i32 r = decoder_header(z, b);
            if (r != GZ_OK) {
                return r;
            }
        } break;
        }
    }
}

typedef struct {
    deflator *def;
    i32 format;
    u32 check;    // of the input so far
    u64 total;
    b32 done;     // trailer written
    i32 cpu;      // for crc32_update, kept across resets
} encoder;

static iz encoder_memsize(void)
{
    return (iz)sizeof(encoder) + 64 + deflate_memsize();
}

// Start a new stream in the same format at a level, which may differ
// from the previous stream's. Like deflate_reset, this costs time in
// proportion to the previous stream.
static void encoder_reset(encoder *e, i32 level)
{
    level = MAX(1, MIN(level, 9));
    deflate_reset(e->def);
    deflate_setlevel(e->def, level);
    e->check = check_init(e->format);
    e->total = 0;
    e->done  = 0;

    switch (e->format) {
    case FMT_GZIP: {
        // XFL marks the fastest and the smallest levels, as GNU gzip and
        // zlib mark them
        u8 header[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 3};
        header[8] = level==1 ? 4 : level==9 ? 2 : 0;
        deflate_bytes(e->def, header, countof(header));
    } break;
    case FMT_ZLIB: {
        // Same header as zlib for a 32 KiB window at this level
        u32 flevel = level<2 ? 0 : level<6 ? 1 : level==6 ? 2 : 3;
        u32 cmf = 0x78;
        u32 flg = flevel << 6;
        flg |= 31 - (cmf<<8 | flg)%31;
        u8 header[2] = {(u8)cmf, (u8)flg};
        deflate_bytes(e->def, header, countof(header));
    } break;
    }
}

static encoder *encoder_new(arena *a, i32 format, i32 level)
{
    encoder *e = new(a, 1, encoder);
    e->def = deflate_new(a, level);
    e->format = format;
    encoder_reset(e, level);
    return e;
}

// Compress from b->in into b->out with a DEF_* flush mode, appending the
// trailer once the raw stream has finished. Returns as deflate_stream.
//
// The trailer is staged as soon as the final block is, though output may
// remain, so that a caller draining output gets both together: the step
// that staged the block had room for more than a trailer.
static i32 encoder_run(encoder *e, zbuf *b, i32 flush)
{
    deflator *d  = e->def;
    u8 const *in = b->in;
    iz avail = b->inlen;
    i32 r = deflate_stream(d, b, flush);
    iz n = avail - b->inlen;  // not b->in-in: both may be null
    if (e->format != FMT_RAW) {
        e->check = check_update(e->format, e->check, in, n, &e->cpu);
    }
    e->total += (u64)n;

    if (d->flushing==DEF_FINISH && d->flushed && !e->done) {
        u8 trailer[8];
        switch (e->format) {
        case FMT_GZIP:
            put32le(trailer+0, e->check);
            put32le(trailer+4, (u32)e->total);
            deflate_bytes(d, trailer, 8);
            break;
        case FMT_ZLIB:
            for (i32 i = 0; i < 4; i++) {
                trailer[i] = (u8)(e->check >> (24 - 8*i));
            }
            deflate_bytes(d, trailer, 4);
            break;
        }
        e->done = 1;
        r = deflate_stream(d, b, flush);
    }
    return r;
}

