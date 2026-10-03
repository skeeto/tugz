// gzip core: raw DEFLATE decoder (RFC 1951)
//
// Accepts exactly the streams zlib accepts: incomplete Huffman codes are
// rejected except for a lone 1-bit code, and an empty distance code is
// only an error if a distance is actually decoded.

#define INF_WSIZE   32768
#define INF_WSIZE2  65536

enum {
    HUFF_CODELEN,
    HUFF_LITLEN,
    HUFF_DIST,
};

typedef struct {
    u16 count[16];
    u16 symbol[288];
} hdecode;

typedef struct {
    reader *in;
    writer *out;

    u64 bitbuf;
    i32 bitcnt;
    i32 err;

    u8 *win;
    iz  wpos;
    iz  wflushed;

    u32 crc;
    u64 total;

    hdecode fixlit;
    hdecode fixdist;
} inflate;

static u16 const inf_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static u8 const inf_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static u16 const inf_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static u8 const inf_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 13
};
static u8 const inf_cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// Build a canonical decoder from code lengths, returning false if the
// lengths do not describe a code that zlib would accept.
static b32 hdecode_build(hdecode *h, u16 const *lens, i32 n, i32 kind)
{
    for (i32 b = 0; b < 16; b++) {
        h->count[b] = 0;
    }
    i32 max = 0;
    for (i32 i = 0; i < n; i++) {
        h->count[lens[i]]++;
        max = MAX(max, lens[i]);
    }
    h->count[0] = 0;

    if (!max) {
        // Nothing can be decoded, which is only allowed for distances
        return kind == HUFF_DIST;
    }

    i32 left = 1;
    for (i32 b = 1; b < 16; b++) {
        left = (left << 1) - h->count[b];
        if (left < 0) {
            return 0;  // over-subscribed
        }
    }
    if (left>0 && (kind==HUFF_CODELEN || max!=1)) {
        return 0;  // incomplete
    }

    u16 offs[16];
    offs[1] = 0;
    for (i32 b = 1; b < 15; b++) {
        offs[b+1] = (u16)(offs[b] + h->count[b]);
    }
    for (i32 i = 0; i < n; i++) {
        if (lens[i]) {
            h->symbol[offs[lens[i]]++] = (u16)i;
        }
    }
    return 1;
}

static inflate *inflate_new(arena *a, reader *in, writer *out)
{
    inflate *s = new(a, 1, inflate);
    s->in  = in;
    s->out = out;
    s->win = newbytes(a, INF_WSIZE2);

    u16 lens[288];
    for (i32 i = 0; i < 288; i++) {
        lens[i] = (u16)(i<144 ? 8 : i<256 ? 9 : i<280 ? 7 : 8);
    }
    hdecode_build(&s->fixlit, lens, 288, HUFF_LITLEN);
    for (i32 i = 0; i < 30; i++) {
        lens[i] = 5;
    }
    // Only 30 of the 32 fixed distance codes are valid, making it
    // incomplete, but zlib treats it as a complete code.
    s->fixdist.count[5] = 32;  // pretend all 32 exist
    for (i32 i = 0; i < 32; i++) {
        s->fixdist.symbol[i] = (u16)i;  // 30 and 31 rejected on decode
    }
    return s;
}

static void inf_fail(inflate *s)
{
    if (!s->err) {
        s->err = s->in->err ? GZ_EREAD : GZ_ETRUNC;
    }
}

static u32 inf_bits(inflate *s, i32 n)
{
    while (s->bitcnt < n) {
        i32 c = reader_byte(s->in);
        if (c < 0) {
            inf_fail(s);
            return 0;
        }
        s->bitbuf |= (u64)c << s->bitcnt;
        s->bitcnt += 8;
    }
    u32 v = (u32)s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

// Read the next byte-aligned byte following the stream, or -1.
static i32 inflate_byte(inflate *s)
{
    i32 drop = s->bitcnt & 7;
    s->bitbuf >>= drop;
    s->bitcnt -= drop;
    if (s->bitcnt) {
        i32 b = (i32)(s->bitbuf & 0xff);
        s->bitbuf >>= 8;
        s->bitcnt -= 8;
        return b;
    }
    return reader_byte(s->in);
}

static void inf_flush(inflate *s)
{
    iz len = s->wpos - s->wflushed;
    s->crc = crc32_update(s->crc, s->win+s->wflushed, len);
    writer_write(s->out, s->win+s->wflushed, len);
    s->wflushed = s->wpos;
}

static void out_byte(inflate *s, u8 b)
{
    if (s->wpos == INF_WSIZE2) {
        inf_flush(s);
        bytemove(s->win, s->win+INF_WSIZE, INF_WSIZE);
        s->wflushed = s->wpos = INF_WSIZE;
    }
    s->win[s->wpos++] = b;
    s->total++;
}

static i32 hdecode_sym(inflate *s, hdecode const *h)
{
    i32 code  = 0;
    i32 first = 0;
    i32 index = 0;
    for (i32 len = 1; len < 16; len++) {
        code |= (i32)inf_bits(s, 1);
        if (s->err) {
            return -1;
        }
        i32 count = h->count[len];
        if (code-first < count) {
            return h->symbol[index + code - first];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->err = GZ_EDATA;
    return -1;
}

static void decode_symbols(inflate *s, hdecode const *lit, hdecode const *dist)
{
    for (;;) {
        i32 sym = hdecode_sym(s, lit);
        if (sym < 0) {
            return;
        } else if (sym < 256) {
            out_byte(s, (u8)sym);
            continue;
        } else if (sym == 256) {
            return;
        }

        sym -= 257;
        if (sym >= 29) {
            s->err = GZ_EDATA;
            return;
        }
        i32 len = inf_len_base[sym] + (i32)inf_bits(s, inf_len_extra[sym]);

        i32 dsym = hdecode_sym(s, dist);
        if (dsym < 0) {
            return;
        } else if (dsym >= 30) {
            s->err = GZ_EDATA;
            return;
        }
        i32 d = inf_dist_base[dsym] + (i32)inf_bits(s, inf_dist_extra[dsym]);
        if (s->err) {
            return;
        } else if ((u64)d > s->total) {
            s->err = GZ_EDATA;
            return;
        }

        for (; len; len--) {
            out_byte(s, s->win[s->wpos - d]);
        }
    }
}

static void stored_block(inflate *s)
{
    inf_bits(s, s->bitcnt & 7);
    u32 len  = inf_bits(s, 16);
    u32 nlen = inf_bits(s, 16);
    if (s->err) {
        return;
    } else if (len != (~nlen & 0xffff)) {
        s->err = GZ_EDATA;
        return;
    }
    for (; len; len--) {
        i32 c = reader_byte(s->in);
        if (c < 0) {
            inf_fail(s);
            return;
        }
        out_byte(s, (u8)c);
    }
}

static void dynamic_block(inflate *s)
{
    i32 hlit  = (i32)inf_bits(s, 5) + 257;
    i32 hdist = (i32)inf_bits(s, 5) + 1;
    i32 hclen = (i32)inf_bits(s, 4) + 4;
    if (s->err) {
        return;
    } else if (hlit>286 || hdist>30) {
        s->err = GZ_EDATA;
        return;
    }

    u16 lens[286+30] = {0};
    for (i32 i = 0; i < hclen; i++) {
        lens[inf_cl_order[i]] = (u16)inf_bits(s, 3);
    }
    if (s->err) {
        return;
    }
    hdecode cl;
    if (!hdecode_build(&cl, lens, 19, HUFF_CODELEN)) {
        s->err = GZ_EDATA;
        return;
    }

    i32 total = hlit + hdist;
    for (i32 n = 0; n < total;) {
        i32 sym = hdecode_sym(s, &cl);
        if (sym < 0) {
            return;
        } else if (sym < 16) {
            lens[n++] = (u16)sym;
            continue;
        }

        u16 fill = 0;
        i32 repeat = 0;
        switch (sym) {
        case 16:
            if (!n) {
                s->err = GZ_EDATA;
                return;
            }
            fill = lens[n-1];
            repeat = 3 + (i32)inf_bits(s, 2);
            break;
        case 17:
            repeat = 3 + (i32)inf_bits(s, 3);
            break;
        case 18:
            repeat = 11 + (i32)inf_bits(s, 7);
            break;
        }
        if (s->err) {
            return;
        } else if (repeat > total-n) {
            s->err = GZ_EDATA;
            return;
        }
        for (; repeat; repeat--) {
            lens[n++] = fill;
        }
    }

    hdecode lit, dist;
    if (!lens[256] ||
        !hdecode_build(&lit, lens, hlit, HUFF_LITLEN) ||
        !hdecode_build(&dist, lens+hlit, hdist, HUFF_DIST)) {
        s->err = GZ_EDATA;
        return;
    }
    decode_symbols(s, &lit, &dist);
}

// Decode a complete stream, then flush output. Returns a GZ_* status.
static i32 inflate_run(inflate *s)
{
    for (b32 final = 0; !final && !s->err;) {
        final = (b32)inf_bits(s, 1);
        i32 type = (i32)inf_bits(s, 2);
        if (s->err) {
            break;
        }
        switch (type) {
        case 0:  stored_block(s);                              break;
        case 1:  decode_symbols(s, &s->fixlit, &s->fixdist);   break;
        case 2:  dynamic_block(s);                             break;
        default: s->err = GZ_EDATA;
        }
    }
    inf_flush(s);
    if (!s->err && s->out->err) {
        s->err = GZ_EWRITE;
    }
    return s->err;
}
