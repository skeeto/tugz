// tugz gzip program I/O: descriptor drivers for the codec

static b32  os_isatty(os *, i32 fd);
// Best effort: give an open output file the input file's permissions,
// ownership, and timestamps, as far as the platform supports.
static void os_copymeta(os *, i32 from, i32 to);

static void os_oom(os *ctx)
{
    s8 msg = S("gzip: out of memory\n");
    os_write(ctx, 2, msg.s, msg.len);
    os_exit(ctx, 1);
}

// The arena is fixed in size, ample for the codecs.
static void os_extend(os *ctx, arena *a, iz need)
{
    (void)a;
    (void)need;
    os_oom(ctx);
}

// Decoded output not yet handed out, for copy-free delivery.
static s8 decoder_pending(decoder *z)
{
    return inflate_pending(z->inf);
}

static void decoder_consume(decoder *z, iz n)
{
    s8 p = inflate_pending(z->inf);
    if (z->format != FMT_RAW) {
        z->check = check_update(z->format, z->check, p.s, n);
    }
    z->total += (u64)n;
    inflate_consume(z->inf, n);
}

static s8 encoder_pending(encoder *e)
{
    return deflate_pending(e->def);
}

static void encoder_consume(encoder *e, iz n)
{
    deflate_consume(e->def, n);
}

#define IO_RDBUF  (1 << 18)

// An encoder for gzip_compress, which can reuse it for any number of
// streams in turn.
static encoder *gzip_encoder(arena *perm, i32 level)
{
    arena a = subarena(perm, encoder_memsize());
    return encoder_new(&a, FMT_GZIP, level);
}

// Compress a descriptor into a descriptor as a new stream at a level,
// first resetting the encoder: for small inputs this costs a fraction
// of a new encoder, so one serves every file.
static i32 gzip_compress(encoder *e, i32 in, i32 out, i32 level,
                         arena scratch)
{
    encoder_reset(e, level);
    reader *r = newreader(&scratch, in, IO_RDBUF);
    b32 werr = 0;
    for (;;) {
        b32  more = reader_fill(r);
        zbuf b    = {r->buf+r->off, r->len-r->off, 0, 0};
        i32 status = encoder_run(e, &b, more ? DEF_NONE : DEF_FINISH);
        r->off = r->len - b.inlen;
        if (status != GZ_NEEDIN) {
            s8 p = encoder_pending(e);
            put_pending(r->ctx, out, p, &werr);
            encoder_consume(e, p.len);
        }
        if (status == GZ_OK) {
            break;
        }
    }
    return r->err ? GZ_EREAD : werr ? GZ_EWRITE : GZ_OK;
}

// Decompress a FMT_* stream, or for gzip all members, from a descriptor
// into a descriptor. A negative output descriptor only verifies.
//
// Following GNU gzip, data after the last gzip member is ignored with a
// warning (GZ_TRAILING) unless it starts with the gzip magic, in which
// case it must be a valid member.
static i32 stream_decompress(i32 in, i32 out, i32 format, arena scratch)
{
    reader  *r = newreader(&scratch, in, IO_RDBUF);
    arena    a = subarena(&scratch, decoder_memsize());
    decoder *z = decoder_new(&a, format);
    b32 werr = 0;
    i32 status;
    for (b32 first = 1;; first = 0) {
        for (;;) {
            b32  more = reader_fill(r);
            zbuf b    = {r->buf+r->off, r->len-r->off, 0, 0};
            status = decoder_run(z, &b);
            r->off = r->len - b.inlen;
            s8 p = decoder_pending(z);
            put_pending(r->ctx, out, p, &werr);
            decoder_consume(z, p.len);
            if ((status!=GZ_NEEDIN || !more) && status!=GZ_NEEDOUT) {
                break;
            }
        }

        if (status == GZ_NEEDIN) {
            // Input ended inside a stream or member
            if (r->err) {
                status = GZ_EREAD;
            } else if (format != FMT_GZIP) {
                status = GZ_ETRUNC;
            } else if (!first && z->hpos<2) {
                status = z->hpos ? GZ_TRAILING : GZ_OK;
            } else {
                status = first && z->hpos==1 ? GZ_ENOTGZ : GZ_ETRUNC;
            }
            break;
        } else if (status==GZ_ENOTGZ && !first) {
            status = GZ_TRAILING;
            break;
        } else if (status!=GZ_OK || format!=FMT_GZIP) {
            break;
        } else if (!reader_fill(r)) {
            status = r->err ? GZ_EREAD : GZ_OK;
            break;
        }
    }

    if (werr && (status==GZ_OK || status==GZ_TRAILING)) {
        status = GZ_EWRITE;
    }
    return status;
}

static i32 gzip_decompress(i32 in, i32 out, arena scratch)
{
    return stream_decompress(in, out, FMT_GZIP, scratch);
}
