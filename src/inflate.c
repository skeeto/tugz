#include "inflate.h"
#include "crc32.h"

#define WSIZE 32768
#define WSIZE2 65536
#define MAX_MATCH 258
#define NLIT 286
#define NDIST 30
#define NCL 19

struct inflate_state {
    reader *in;
    writer *out;

    u64 bitbuf;
    i32 bitcnt;
    b32 err;

    u8 *win;
    iz wpos;
    iz wflushed;

    u32 crc;
    u64 total;
};

typedef struct {
    u16 count[16];
    u16 symbol[288];
} hdecode;

static const u16 len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const u8 len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static const u16 dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const u8 dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 13
};
static const u8 cl_order[NCL] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

inflate_state *inflate_create(reader *in, writer *out)
{
    inflate_state *s = arena_alloc(sizeof(inflate_state), 16);
    memset(s, 0, sizeof(*s));
    s->in = in;
    s->out = out;
    s->win = arena_alloc(WSIZE2, 64);
    s->crc = 0xffffffffu;
    return s;
}

static u32 inf_bits(inflate_state *s, i32 n)
{
    while (s->bitcnt < n) {
        i32 c = reader_byte(s->in);
        if (c < 0) {
            s->err = 1;
            return 0;
        }
        s->bitbuf |= (u64)(u8)c << s->bitcnt;
        s->bitcnt += 8;
    }
    u32 v = (u32)s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void inf_align(inflate_state *s)
{
    i32 drop = s->bitcnt & 7;
    s->bitbuf >>= drop;
    s->bitcnt -= drop;
}

static i32 inf_byte(inflate_state *s)
{
    if (s->bitcnt >= 8) {
        i32 b = (i32)(s->bitbuf & 0xff);
        s->bitbuf >>= 8;
        s->bitcnt -= 8;
        return b;
    }
    return reader_byte(s->in);
}

i32 inflate_read_byte(inflate_state *s)
{
    return inf_byte(s);
}

static void out_flush_range(inflate_state *s, iz from, iz to)
{
    iz n = to - from;
    if (!n) {
        return;
    }
    s->crc = crc32_update(s->crc, s->win + from, n);
    if (!s->out->null) {
        writer_write(s->out, s->win + from, n);
    }
}

static void out_byte(inflate_state *s, u8 b)
{
    if (s->wpos == WSIZE2) {
        out_flush_range(s, s->wflushed, s->wpos);
        memmove(s->win, s->win + WSIZE, WSIZE);
        s->wflushed = WSIZE;
        s->wpos = WSIZE;
    }
    s->win[s->wpos++] = b;
    s->total++;
}

static void out_copy(inflate_state *s, i32 dist)
{
    u8 b = s->win[s->wpos - (iz)dist];
    out_byte(s, b);
}

static i32 hdecode_build(hdecode *h, const u16 *lens, i32 n)
{
    for (i32 b = 0; b < 16; b++) {
        h->count[b] = 0;
    }
    for (i32 i = 0; i < n; i++) {
        if (lens[i] > 15) {
            return -1;
        }
        h->count[lens[i]]++;
    }
    h->count[0] = 0;
    i32 left = 1;
    for (i32 b = 1; b <= 15; b++) {
        left <<= 1;
        left -= h->count[b];
        if (left < 0) {
            return -1;
        }
    }
    u16 offs[16];
    offs[1] = 0;
    for (i32 b = 1; b < 15; b++) {
        offs[b + 1] = (u16)(offs[b] + h->count[b]);
    }
    for (i32 i = 0; i < n; i++) {
        if (lens[i]) {
            h->symbol[offs[lens[i]]++] = (u16)i;
        }
    }
    return 0;
}

static i32 hdecode_sym(inflate_state *s, const hdecode *h)
{
    i32 code = 0;
    i32 first = 0;
    i32 index = 0;
    for (i32 len = 1; len <= 15; len++) {
        code |= (i32)inf_bits(s, 1);
        if (s->err) {
            return -1;
        }
        i32 count = h->count[len];
        if (code - first < count) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static void decode_symbols(inflate_state *s, const hdecode *lit, const hdecode *dist)
{
    for (;;) {
        i32 sym = hdecode_sym(s, lit);
        if (sym < 0) {
            return;
        }
        if (sym < 256) {
            out_byte(s, (u8)sym);
            continue;
        }
        if (sym == 256) {
            return;
        }
        sym -= 257;
        if (sym > 28) {
            s->err = 1;
            return;
        }
        i32 len = (i32)len_base[sym] + (i32)inf_bits(s, len_extra[sym]);
        i32 dsym = hdecode_sym(s, dist);
        if (dsym < 0) {
            return;
        }
        if (dsym > 29) {
            s->err = 1;
            return;
        }
        i32 d = (i32)dist_base[dsym] + (i32)inf_bits(s, dist_extra[dsym]);
        if (s->err || (u64)d > s->total) {
            s->err = 1;
            return;
        }
        while (len-- > 0) {
            out_copy(s, d);
        }
    }
}

static void stored_block(inflate_state *s)
{
    inf_align(s);
    u32 len = 0, nlen = 0;
    for (i32 i = 0; i < 2; i++) {
        i32 c = inf_byte(s);
        if (c < 0) {
            s->err = 1;
            return;
        }
        len |= (u32)c << (8 * i);
    }
    for (i32 i = 0; i < 2; i++) {
        i32 c = inf_byte(s);
        if (c < 0) {
            s->err = 1;
            return;
        }
        nlen |= (u32)c << (8 * i);
    }
    if (len != ((~nlen) & 0xffff)) {
        s->err = 1;
        return;
    }
    while (len-- > 0) {
        i32 c = inf_byte(s);
        if (c < 0) {
            s->err = 1;
            return;
        }
        out_byte(s, (u8)c);
    }
}

static void fixed_block(inflate_state *s)
{
    static hdecode lit, dist;
    static b32 ready;
    if (!ready) {
        u16 ll[288];
        for (i32 i = 0; i < 288; i++) {
            ll[i] = (u16)(i <= 143 ? 8 : i <= 255 ? 9 : i <= 279 ? 7 : 8);
        }
        hdecode_build(&lit, ll, 288);
        u16 dl[30];
        for (i32 i = 0; i < 30; i++) {
            dl[i] = 5;
        }
        hdecode_build(&dist, dl, 30);
        ready = 1;
    }
    decode_symbols(s, &lit, &dist);
}

static void dynamic_block(inflate_state *s)
{
    i32 hlit = (i32)inf_bits(s, 5) + 257;
    i32 hdist = (i32)inf_bits(s, 5) + 1;
    i32 hclen = (i32)inf_bits(s, 4) + 4;
    if (s->err) {
        return;
    }
    if (hlit > NLIT || hdist > NDIST) {
        s->err = 1;
        return;
    }

    u16 cl_lens[NCL] = {0};
    for (i32 i = 0; i < hclen; i++) {
        cl_lens[cl_order[i]] = (u16)inf_bits(s, 3);
    }
    hdecode cl;
    if (hdecode_build(&cl, cl_lens, NCL) < 0) {
        s->err = 1;
        return;
    }

    u16 lens[NLIT + NDIST];
    i32 total = hlit + hdist;
    i32 n = 0;
    while (n < total) {
        i32 sym = hdecode_sym(s, &cl);
        if (sym < 0) {
            return;
        }
        if (sym < 16) {
            lens[n++] = (u16)sym;
        } else if (sym == 16) {
            if (n == 0) {
                s->err = 1;
                return;
            }
            i32 r = 3 + (i32)inf_bits(s, 2);
            if (n + r > total) {
                s->err = 1;
                return;
            }
            u16 prev = lens[n - 1];
            while (r-- > 0) {
                lens[n++] = prev;
            }
        } else if (sym == 17) {
            i32 r = 3 + (i32)inf_bits(s, 3);
            if (n + r > total) {
                s->err = 1;
                return;
            }
            while (r-- > 0) {
                lens[n++] = 0;
            }
        } else {
            i32 r = 11 + (i32)inf_bits(s, 7);
            if (n + r > total) {
                s->err = 1;
                return;
            }
            while (r-- > 0) {
                lens[n++] = 0;
            }
        }
    }

    hdecode lit, dist;
    if (hdecode_build(&lit, lens, hlit) < 0 ||
        hdecode_build(&dist, lens + hlit, hdist) < 0) {
        s->err = 1;
        return;
    }
    decode_symbols(s, &lit, &dist);
}

b32 inflate_stream(inflate_state *s)
{
    b32 final = 0;
    while (!final && !s->err) {
        final = (b32)inf_bits(s, 1);
        i32 type = (i32)inf_bits(s, 2);
        if (s->err) {
            break;
        }
        switch (type) {
        case 0: stored_block(s); break;
        case 1: fixed_block(s); break;
        case 2: dynamic_block(s); break;
        default: s->err = 1; break;
        }
    }
    return !s->err;
}

b32 inflate_finish(inflate_state *s)
{
    out_flush_range(s, s->wflushed, s->wpos);
    return !s->err && !s->out->err;
}

u32 inflate_crc(inflate_state *s)
{
    return ~s->crc;
}

u64 inflate_total(inflate_state *s)
{
    return s->total;
}