// tugz gzip program I/O: descriptor drivers for the codec

static b32  os_isatty(os *, i32 fd);
// Whether the os_open that just failed (OS_ERR) found no such file.
static b32  os_missing(os *);
// Why the os_* call that just failed did, in the system's words (as C's
// strerror gives them), or an empty string if unknown. The text may last
// only until the next call (the BSDs' strerror reuses one buffer).
static s8   os_error(os *);
// Whether the os_write that just failed found a pipe with no reader.
static b32  os_pipeclosed(os *);
// An open file's permissions, ownership, and timestamps, as far as the
// platform keeps them.
typedef struct osmeta osmeta;
// An input's metadata, taken before reading changes its access time, or
// null if it cannot be read (os_error says why).
static osmeta *os_getmeta(os *, i32 fd, arena *);
// Give an open output file metadata, its ownership only where permitted.
// Returns false if its permissions or timestamps could not be set (with
// os_error saying why), though as much as could be is set.
static b32  os_setmeta(os *, i32 fd, osmeta *);

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
//
// As in GNU gzip, a read or write error stops at once: a stream left
// unfinished by a read error cannot pass for all of the input.
static i32 gzip_compress(encoder *e, i32 in, i32 out, i32 level,
                         arena scratch)
{
    encoder_reset(e, level);
    reader *r = newreader(&scratch, in, IO_RDBUF);
    for (;;) {
        b32 more = reader_fill(r);
        if (r->err) {
            return GZ_EREAD;
        }
        zbuf b = {r->buf+r->off, r->len-r->off, 0, 0};
        i32 status = encoder_run(e, &b, more ? DEF_NONE : DEF_FINISH);
        r->off = r->len - b.inlen;
        if (status != GZ_NEEDIN) {
            s8 p = encoder_pending(e);
            if (!put_pending(r->ctx, out, p)) {
                return GZ_EWRITE;
            }
            encoder_consume(e, p.len);
        }
        if (status == GZ_OK) {
            return GZ_OK;
        }
    }
}

// A decoder in a FMT_* format for stream_decompress, which can reuse it
// for any number of streams in turn.
static decoder *stream_decoder(arena *perm, i32 format)
{
    arena a = subarena(perm, decoder_memsize());
    return decoder_new(&a, format);
}

// Settle input that is not a gzip member, of which the decoder took the
// first bytes (fewer than two at the end of the input), following GNU
// gzip. A lone byte may have begun the magic, so it is a truncation
// unless zero. After a member, zero bytes to the end are padding (as on
// tape), and other data is ignored with a warning.
static i32 not_gzip(decoder *z, reader *r, b32 first)
{
    s8 head = {z->buf, z->len};
    if (head.len<2 && (!head.len || head.s[0])) {
        return GZ_ETRUNC;
    } else if (first) {
        return GZ_ENOTGZ;
    }

    b32 zeros = !head.s[0] && (head.len<2 || !head.s[1]);
    while (zeros && reader_fill(r)) {
        for (; zeros && r->off<r->len; r->off++) {
            zeros = !r->buf[r->off];
        }
    }
    if (!zeros) {
        return GZ_TRAILING;
    }
    return r->err ? GZ_EREAD : GZ_OK;
}

// Copy input that is not a gzip member to the output unchanged, from the
// first bytes, which the decoder took.
static i32 pass_through(decoder *z, reader *r, i32 out)
{
    if (!put_pending(r->ctx, out, (s8){z->buf, z->len})) {
        return GZ_EWRITE;
    }
    while (reader_fill(r)) {
        s8 rest = {r->buf+r->off, r->len-r->off};
        if (!put_pending(r->ctx, out, rest)) {
            return GZ_EWRITE;
        }
        r->off = r->len;
    }
    return r->err ? GZ_EREAD : GZ_OK;
}

// A reader for stream_header and stream_decode.
static reader *stream_reader(arena *scratch, i32 in)
{
    return newreader(scratch, in, IO_RDBUF);
}

// Reset the decoder and read only the header of the stream, or of the
// first gzip member, so that a caller can learn that the input is in the
// format before creating an output, as GNU gzip reads the header before
// creating one. Returns whether the body is next. Either way,
// stream_decode continues, and if not, settles the status.
static b32 stream_header(decoder *z, reader *r)
{
    decoder_reset(z);
    i32 status = GZ_NEEDIN;
    while (status==GZ_NEEDIN && reader_fill(r)) {
        zbuf b = {r->buf+r->off, r->len-r->off, 0, 0};
        status = decoder_header(z, &b);
        r->off = r->len - b.inlen;
    }
    return status == GZ_OK;
}

// Decompress a stream in the decoder's format, or for gzip all members,
// from a reader into a descriptor, continuing from a decoder that is
// reset (decoder_reset), and perhaps past the header (stream_header).
// One decoder so serves every file. A negative output only verifies.
//
// Following GNU gzip, data after the last gzip member is ignored with a
// warning (GZ_TRAILING), unless it is only zero bytes, which is fine, or
// starts with the gzip magic, in which case it must be a valid member.
// With copy, as for GNU's gzip -cdf (zcat -f), data that is not gzip,
// from the start or after a member, is instead copied unchanged.
//
// As in GNU gzip, a read or write error stops at once.
static i32 stream_decode(decoder *z, reader *r, i32 out, b32 copy)
{
    i32 format = z->format;
    for (b32 first = 1;; first = 0) {
        i32 status = GZ_NEEDIN;
        for (;;) {
            // After GZ_NEEDOUT the decoder has output, or an error, to
            // hand over before it needs input
            b32 more = status==GZ_NEEDOUT || reader_fill(r);
            if (r->err) {
                return GZ_EREAD;
            }
            zbuf b = {r->buf+r->off, r->len-r->off, 0, 0};
            status = decoder_run(z, &b);
            r->off = r->len - b.inlen;
            s8 p = decoder_pending(z);
            if (!put_pending(r->ctx, out, p)) {
                return GZ_EWRITE;
            }
            decoder_consume(z, p.len);
            if ((status!=GZ_NEEDIN || !more) && status!=GZ_NEEDOUT) {
                break;
            }
        }

        // Two bytes without the gzip magic, or fewer at the end of input
        b32 ended = status == GZ_NEEDIN;
        if (format==FMT_GZIP && (status==GZ_ENOTGZ || (ended && z->hpos<2))) {
            return copy ? pass_through(z, r, out) : not_gzip(z, r, first);
        } else if (ended) {
            return GZ_ETRUNC;  // inside a stream or member
        } else if (status!=GZ_OK || format!=FMT_GZIP) {
            return status;
        } else if (!reader_fill(r)) {
            return r->err ? GZ_EREAD : GZ_OK;
        }
    }
}

// Decompress from a descriptor, as stream_decode, first resetting the
// decoder.
static i32 stream_decompress(decoder *z, i32 in, i32 out, b32 copy,
                             arena scratch)
{
    decoder_reset(z);
    return stream_decode(z, stream_reader(&scratch, in), out, copy);
}
