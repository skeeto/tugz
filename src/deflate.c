#include "deflate.h"

#define WSIZE 32768
#define WMASK (WSIZE - 1)
#define MIN_MATCH 3
#define MAX_MATCH 258
#define HASH_LEN 4
#ifndef D3_MAX
#define D3_MAX 4096
#endif
#define HASH_BITS 16
#define HASH_SIZE (1 << HASH_BITS)
#define CHUNK_SIZE (1 << 20)
#define WIN_CAP (CHUNK_SIZE + WSIZE)
#define TOK_CAP 65535
#define NLIT 286
#define NDIST 30
#define NCL 19

typedef struct {
    u16 litlen;
    u16 dist;
} token;

typedef struct {
    i32 depth, nice, lazy, good, depth3;
} lvl;

static const lvl levels[10] = {
    {0, 0, 0, 0, 0},
    {4, 16, 0, 4, 4},
    {8, 24, 0, 4, 8},
    {16, 32, 4, 4, 16},
    {32, 48, 8, 4, 32},
    {48, 64, 8, 8, 32},
    {128, 258, 2, 16, 32},
    {256, 258, 8, 8, 64},
    {512, 258, 8, 32, 128},
    {1024, 258, 16, 32, 256},
};

struct deflate {
    writer *out;
    i32 level;
    i32 depth, nice, lazy, good, depth3;

    u8 *win;
    iz win_len;
    u64 base;
    iz ins;

    u32 *head;
    u32 *prev;
    u32 *head3;
    u32 *prev3;
    i32 use3;
    i32 sampled;

    token *toks;
    iz ntok, tok_cap;

    u64 blk_start;
    u64 blk_len;
    u64 pend_start;
    iz pend_len;

    u32 lit_freq[NLIT];
    u32 dist_freq[NDIST];

    u64 bitbuf;
    i32 bitcnt;
};

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

static u8 length_code_tab[259];
static u8 dist_code_tab[32769];
static u16 fx_code[288];
static u8 fx_len[288];
static u16 fx_dcode[32];

static u32 bit_reverse(u32 v, i32 n)
{
    u32 r = 0;
    for (i32 i = 0; i < n; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

void deflate_init_tables(void)
{
    for (i32 i = 0; i < 29; i++) {
        u16 lo = len_base[i];
        u16 hi = (i == 28) ? 258 : (u16)(len_base[i + 1] - 1);
        for (u16 l = lo; l <= hi; l++) {
            length_code_tab[l] = (u8)i;
        }
    }
    for (i32 i = 0; i < 30; i++) {
        u32 lo = dist_base[i];
        u32 hi = (i == 29) ? 32768u : (u32)(dist_base[i + 1] - 1);
        for (u32 d = lo; d <= hi; d++) {
            dist_code_tab[d] = (u8)i;
        }
    }

    for (i32 s = 0; s < 288; s++) {
        i32 l = s <= 143 ? 8 : s <= 255 ? 9 : s <= 279 ? 7 : 8;
        fx_len[s] = (u8)l;
    }
    u32 bl_count[16] = {0};
    for (i32 s = 0; s < 288; s++) bl_count[fx_len[s]]++;
    u32 next[16];
    u32 code = 0;
    for (i32 b = 1; b <= 15; b++) {
        code = (code + bl_count[b - 1]) << 1;
        next[b] = code;
    }
    for (i32 s = 0; s < 288; s++) {
        fx_code[s] = (u16)bit_reverse(next[fx_len[s]]++, fx_len[s]);
    }
    for (i32 d = 0; d < 32; d++) {
        fx_dcode[d] = (u16)bit_reverse((u32)d, 5);
    }
}

deflate *deflate_create(i32 level, writer *out)
{
    deflate *d = arena_alloc(sizeof(deflate), 16);
    memset(d, 0, sizeof(*d));
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    d->out = out;
    d->level = level;
    d->depth = levels[level].depth;
    d->nice = levels[level].nice;
    d->lazy = levels[level].lazy;
    d->good = levels[level].good;
    d->depth3 = levels[level].depth3;
    d->win = arena_alloc(WIN_CAP, 64);
    d->head = arena_zalloc(HASH_SIZE * (iz)sizeof(u32), 64);
    d->prev = arena_alloc(WSIZE * (iz)sizeof(u32), 64);
    d->head3 = arena_zalloc(HASH_SIZE * (iz)sizeof(u32), 64);
    d->prev3 = arena_alloc(WSIZE * (iz)sizeof(u32), 64);
    d->toks = arena_alloc(TOK_CAP * (iz)sizeof(token), 64);
    d->tok_cap = TOK_CAP;
    return d;
}

static void bw_put(deflate *d, u32 v, i32 n)
{
    u64 mask = (n >= 32) ? 0xffffffffull : ((1ull << n) - 1);
    d->bitbuf |= ((u64)v & mask) << d->bitcnt;
    d->bitcnt += n;
    while (d->bitcnt >= 8) {
        writer_byte(d->out, (u8)d->bitbuf);
        d->bitbuf >>= 8;
        d->bitcnt -= 8;
    }
}

static void bw_align(deflate *d)
{
    if (d->bitcnt) {
        bw_put(d, 0, 8 - d->bitcnt);
    }
}

static void bw_bytes(deflate *d, const u8 *p, iz n)
{
    writer_write(d->out, p, n);
}

typedef struct {
    u16 len[288];
    u16 code[288];
} htree;

static void huff_build(htree *t, const u32 *freq_in, i32 n, i32 maxlen, i32 minsyms)
{
    u32 f[288];
    i32 syms[288];
    i32 m = 0;

    for (i32 i = 0; i < n; i++) {
        f[i] = freq_in[i];
        t->len[i] = 0;
        t->code[i] = 0;
        if (f[i]) {
            syms[m++] = i;
        }
    }
    for (i32 i = 0; i < n && m < minsyms; i++) {
        if (f[i] == 0) {
            f[i] = 1;
            syms[m++] = i;
        }
    }
    for (i32 i = 1; i < m; i++) {
        i32 k = syms[i];
        i32 j = i - 1;
        while (j >= 0 && f[syms[j]] < f[k]) {
            syms[j + 1] = syms[j];
            j--;
        }
        syms[j + 1] = k;
    }
    if (m == 0) {
        return;
    }
    if (m == 1) {
        t->len[syms[0]] = 1;
        return;
    }

    u64 nf[2 * 288];
    i32 par[2 * 288];
    for (i32 i = 0; i < m; i++) {
        nf[i] = f[syms[i]];
        par[i] = -1;
    }
    for (i32 k = m; k < 2 * m - 1; k++) {
        i32 a = -1, b = -1;
        for (i32 j = 0; j < k; j++) {
            if (par[j] < 0) {
                if (a < 0 || nf[j] < nf[a]) {
                    b = a;
                    a = j;
                } else if (b < 0 || nf[j] < nf[b]) {
                    b = j;
                }
            }
        }
        nf[k] = nf[a] + nf[b];
        par[a] = k;
        par[b] = k;
        par[k] = -1;
    }

    i32 bl_count[16] = {0};
    i32 overflow = 0;
    for (i32 i = 0; i < 2 * m - 1; i++) {
        i32 depth = 0;
        for (i32 j = i; par[j] >= 0; j = par[j]) {
            depth++;
        }
        if (depth > maxlen) {
            depth = maxlen;
            overflow++;
        }
        if (i < m) {
            bl_count[depth]++;
        }
    }
    while (overflow > 0) {
        i32 bits = maxlen - 1;
        while (bits > 0 && bl_count[bits] == 0) {
            bits--;
        }
        if (bits == 0) {
            break;
        }
        bl_count[bits]--;
        bl_count[bits + 1] += 2;
        bl_count[maxlen]--;
        overflow -= 2;
    }
    i32 idx = 0;
    for (i32 bits = 1; bits <= maxlen; bits++) {
        for (i32 j = 0; j < bl_count[bits]; j++) {
            if (idx < m) {
                t->len[syms[idx++]] = (u16)bits;
            }
        }
    }
    for (; idx < m; idx++) {
        t->len[syms[idx]] = (u16)maxlen;
    }
}

static void huff_codes(htree *t, i32 n)
{
    u32 bl_count[16] = {0};
    for (i32 i = 0; i < n; i++) {
        if (t->len[i]) {
            bl_count[t->len[i]]++;
        }
    }
    u32 next[16];
    u32 code = 0;
    for (i32 b = 1; b <= 15; b++) {
        code = (code + bl_count[b - 1]) << 1;
        next[b] = code;
    }
    for (i32 i = 0; i < n; i++) {
        i32 l = t->len[i];
        t->code[i] = l ? (u16)bit_reverse(next[l]++, l) : 0;
    }
}

static i32 cl_extra_bits(i32 s)
{
    return s == 16 ? 2 : s == 17 ? 3 : s == 18 ? 7 : 0;
}

static i32 rle_lengths(const u16 *lens, i32 n, u8 *sym, u8 *extra)
{
    i32 m = 0;
    i32 i = 0;
    while (i < n) {
        i32 l = lens[i];
        i32 j = i + 1;
        while (j < n && lens[j] == l) {
            j++;
        }
        i32 run = j - i;
        if (l == 0) {
            while (run >= 11) {
                i32 r = run > 138 ? 138 : run;
                sym[m] = 18;
                extra[m] = (u8)(r - 11);
                m++;
                run -= r;
            }
            while (run >= 3) {
                i32 r = run > 10 ? 10 : run;
                sym[m] = 17;
                extra[m] = (u8)(r - 3);
                m++;
                run -= r;
            }
            while (run > 0) {
                sym[m] = 0;
                extra[m] = 0;
                m++;
                run--;
            }
        } else {
            sym[m] = (u8)l;
            extra[m] = 0;
            m++;
            run--;
            while (run >= 3) {
                i32 r = run > 6 ? 6 : run;
                sym[m] = 16;
                extra[m] = (u8)(r - 3);
                m++;
                run -= r;
            }
            while (run > 0) {
                sym[m] = (u8)l;
                extra[m] = 0;
                m++;
                run--;
            }
        }
        i = j;
    }
    return m;
}

typedef struct {
    htree lt, dt, cl;
    i32 hlit, hdist, hclen;
    i32 nrle;
    u8 rsym[768], rextra[768];
} dynblock;

#ifdef GZ_DEBUG_HUFF
#include <stdio.h>
static void dbg_tree(const char *tag, const htree *t, i32 n, i32 maxlen,
                     const u32 *freq)
{
    u32 kraft = 0;
    i32 used = 0;
    for (i32 i = 0; i < n; i++) {
        if (t->len[i]) {
            kraft += 1u << (15 - t->len[i]);
            used++;
        }
    }
    if (used && kraft != 32768) {
        fprintf(stderr, "%s INVALID kraft=%u used=%d maxlen=%d\n",
                tag, kraft, used, maxlen);
        for (i32 i = 0; i < n; i++) {
            if (t->len[i]) {
                fprintf(stderr, "  sym=%d freq=%u len=%d\n", i, freq[i], t->len[i]);
            }
        }
    }
}
#endif

static void build_dyn(deflate *d, dynblock *b)
{
    u32 lf[NLIT], df[NDIST];
    for (i32 i = 0; i < NLIT; i++) lf[i] = d->lit_freq[i];
    for (i32 i = 0; i < NDIST; i++) df[i] = d->dist_freq[i];
    lf[256] += 1;

    huff_build(&b->lt, lf, NLIT, 15, 2);
    huff_build(&b->dt, df, NDIST, 15, 1);
    huff_codes(&b->lt, NLIT);
    huff_codes(&b->dt, NDIST);

    i32 hlit = NLIT;
    while (hlit > 257 && b->lt.len[hlit - 1] == 0) hlit--;
    i32 hdist = NDIST;
    while (hdist > 1 && b->dt.len[hdist - 1] == 0) hdist--;
    b->hlit = hlit;
    b->hdist = hdist;

    u16 lens[NLIT + NDIST];
    for (i32 i = 0; i < hlit; i++) lens[i] = b->lt.len[i];
    for (i32 i = 0; i < hdist; i++) lens[hlit + i] = b->dt.len[i];
    b->nrle = rle_lengths(lens, hlit + hdist, b->rsym, b->rextra);

    u32 clf[NCL] = {0};
    for (i32 i = 0; i < b->nrle; i++) clf[b->rsym[i]]++;
    huff_build(&b->cl, clf, NCL, 7, 2);
    huff_codes(&b->cl, NCL);

#ifdef GZ_DEBUG_HUFF
    dbg_tree("lt", &b->lt, NLIT, 15, lf);
    dbg_tree("dt", &b->dt, NDIST, 15, df);
    dbg_tree("cl", &b->cl, NCL, 7, clf);
#endif

    i32 hclen = NCL;
    while (hclen > 4 && b->cl.len[cl_order[hclen - 1]] == 0) hclen--;
    b->hclen = hclen;
}

static u64 cost_dyn(deflate *d, dynblock *b)
{
    u64 bits = 3 + 5 + 5 + 4 + (u64)3 * b->hclen;
    for (i32 i = 0; i < b->nrle; i++) {
        bits += b->cl.len[b->rsym[i]] + cl_extra_bits(b->rsym[i]);
    }
    for (iz i = 0; i < d->ntok; i++) {
        token *t = &d->toks[i];
        if (t->dist == 0) {
            bits += b->lt.len[t->litlen];
        } else {
            i32 lc = length_code_tab[t->litlen];
            i32 dc = dist_code_tab[t->dist];
            bits += b->lt.len[257 + lc] + len_extra[lc] + b->dt.len[dc] + dist_extra[dc];
        }
    }
    bits += b->lt.len[256];
    return bits;
}

static u64 cost_fixed(deflate *d)
{
    u64 bits = 3;
    for (iz i = 0; i < d->ntok; i++) {
        token *t = &d->toks[i];
        if (t->dist == 0) {
            bits += fx_len[t->litlen];
        } else {
            i32 lc = length_code_tab[t->litlen];
            i32 dc = dist_code_tab[t->dist];
            bits += fx_len[257 + lc] + len_extra[lc] + 5 + dist_extra[dc];
        }
    }
    bits += fx_len[256];
    return bits;
}

static u64 cost_stored(deflate *d)
{
    u64 n = d->blk_len;
    u64 nchunks = (n + 65534) / 65535;
    u64 pad = (8 - ((u64)d->bitcnt + 3) % 8) % 8;
    return 3 + pad + 32 + 8 * n + (nchunks - 1) * 40;
}

static void emit_stored_chunk(deflate *d, iz start, iz k, b32 final)
{
    bw_put(d, final ? 1 : 0, 1);
    bw_put(d, 0, 2);
    bw_align(d);
    bw_put(d, (u32)k & 0xff, 8);
    bw_put(d, ((u32)k >> 8) & 0xff, 8);
    u16 nl = (u16)~(u16)k;
    bw_put(d, nl & 0xff, 8);
    bw_put(d, (nl >> 8) & 0xff, 8);
    bw_bytes(d, d->win + start, k);
}

static void flush_pending(deflate *d, b32 final)
{
    if (!d->pend_len) {
        return;
    }
    iz start = (iz)(d->pend_start - d->base);
    iz n = d->pend_len;
    d->pend_len = 0;
    iz off = 0;
    while (off < n) {
        iz k = n - off;
        if (k > 65535) {
            k = 65535;
        }
        emit_stored_chunk(d, start + off, k, final && off + k == n);
        off += k;
    }
}

static void emit_stored(deflate *d, b32 final)
{
    u64 s = d->blk_start;
    iz n = (iz)d->blk_len;
    if (d->pend_len) {
        if (d->pend_start + d->pend_len == s) {
            s = d->pend_start;
            n += d->pend_len;
        } else {
            flush_pending(d, 0);
        }
        d->pend_len = 0;
    }
    iz start = (iz)(s - d->base);
    iz off = 0;
    while (n - off >= 65535) {
        emit_stored_chunk(d, start + off, 65535, 0);
        off += 65535;
    }
    iz rem = n - off;
    if (rem) {
        if (final) {
            emit_stored_chunk(d, start + off, rem, 1);
        } else {
            d->pend_start = s + (u64)off;
            d->pend_len = rem;
        }
    }
}

static void emit_fixed(deflate *d, b32 final)
{
    bw_put(d, final ? 1 : 0, 1);
    bw_put(d, 1, 2);
    for (iz i = 0; i < d->ntok; i++) {
        token *t = &d->toks[i];
        if (t->dist == 0) {
            bw_put(d, fx_code[t->litlen], fx_len[t->litlen]);
        } else {
            i32 lc = length_code_tab[t->litlen];
            i32 dc = dist_code_tab[t->dist];
            bw_put(d, fx_code[257 + lc], fx_len[257 + lc]);
            if (len_extra[lc]) bw_put(d, t->litlen - len_base[lc], len_extra[lc]);
            bw_put(d, fx_dcode[dc], 5);
            if (dist_extra[dc]) bw_put(d, t->dist - dist_base[dc], dist_extra[dc]);
        }
    }
    bw_put(d, fx_code[256], fx_len[256]);
}

static void emit_dynamic(deflate *d, b32 final, dynblock *b)
{
    bw_put(d, final ? 1 : 0, 1);
    bw_put(d, 2, 2);
    bw_put(d, (u32)(b->hlit - 257), 5);
    bw_put(d, (u32)(b->hdist - 1), 5);
    bw_put(d, (u32)(b->hclen - 4), 4);
    for (i32 k = 0; k < b->hclen; k++) {
        bw_put(d, b->cl.len[cl_order[k]], 3);
    }
    for (i32 i = 0; i < b->nrle; i++) {
        i32 s = b->rsym[i];
        bw_put(d, b->cl.code[s], b->cl.len[s]);
        i32 eb = cl_extra_bits(s);
        if (eb) bw_put(d, b->rextra[i], eb);
    }
    for (iz i = 0; i < d->ntok; i++) {
        token *t = &d->toks[i];
        if (t->dist == 0) {
            bw_put(d, b->lt.code[t->litlen], b->lt.len[t->litlen]);
        } else {
            i32 lc = length_code_tab[t->litlen];
            i32 dc = dist_code_tab[t->dist];
            bw_put(d, b->lt.code[257 + lc], b->lt.len[257 + lc]);
            if (len_extra[lc]) bw_put(d, t->litlen - len_base[lc], len_extra[lc]);
            bw_put(d, b->dt.code[dc], b->dt.len[dc]);
            if (dist_extra[dc]) bw_put(d, t->dist - dist_base[dc], dist_extra[dc]);
        }
    }
    bw_put(d, b->lt.code[256], b->lt.len[256]);
}

static void flush_block(deflate *d, b32 final)
{
    if (d->ntok == 0) {
        if (final) {
            if (d->pend_len) {
                flush_pending(d, 1);
            } else {
                bw_put(d, 1, 1);
                bw_put(d, 1, 2);
                bw_put(d, fx_code[256], fx_len[256]);
            }
        }
        return;
    }

    dynblock b;
    build_dyn(d, &b);

    u64 cd = cost_dyn(d, &b);
    u64 cf = cost_fixed(d);
    b32 can_store = d->blk_start >= d->base &&
        (d->blk_start - d->base) + d->blk_len <= d->win_len;
    u64 cs = can_store ? cost_stored(d) : ~(u64)0;

    if (cs <= cd && cs <= cf) {
        emit_stored(d, final);
    } else {
        flush_pending(d, 0);
        if (cf <= cd) {
            emit_fixed(d, final);
        } else {
            emit_dynamic(d, final, &b);
        }
    }

    d->ntok = 0;
    d->blk_len = 0;
    memset(d->lit_freq, 0, sizeof(d->lit_freq));
    memset(d->dist_freq, 0, sizeof(d->dist_freq));
}

static inline void tok_lit(deflate *d, u8 c, u64 g)
{
    if (d->ntok == 0) {
        d->blk_start = g;
    }
    d->blk_len += 1;
    token *t = &d->toks[d->ntok++];
    t->litlen = c;
    t->dist = 0;
    d->lit_freq[c]++;
    if (d->ntok == d->tok_cap) {
        flush_block(d, 0);
    }
}

static inline void tok_match(deflate *d, i32 len, u64 dist, u64 g)
{
    if (d->ntok == 0) {
        d->blk_start = g;
    }
    d->blk_len += (u64)len;
    token *t = &d->toks[d->ntok++];
    t->litlen = (u16)len;
    t->dist = (u16)dist;
    d->lit_freq[257 + length_code_tab[len]]++;
    d->dist_freq[dist_code_tab[dist]]++;
    if (d->ntok == d->tok_cap) {
        flush_block(d, 0);
    }
}

static u32 hash_insert(deflate *d, iz p)
{
    u32 v;
    __builtin_memcpy(&v, d->win + p, 4);
    u32 g = (u32)(d->base + (u64)p);
    u32 h = (v * 2654435761u) >> (32 - HASH_BITS);
    d->prev[g & WMASK] = d->head[h];
    d->head[h] = g + 1;
    u32 prev3 = 0;
    if (d->use3) {
        u32 h3 = ((v & 0xffffffu) * 2654435761u) >> (32 - HASH_BITS);
        prev3 = d->head3[h3];
        d->prev3[g & WMASK] = prev3;
        d->head3[h3] = g + 1;
    }
    return prev3;
}

static u32 ensure_insert(deflate *d, iz q)
{
    u32 c3 = 0;
    if (q >= d->ins) {
        iz lim = d->win_len >= HASH_LEN ? d->win_len - (HASH_LEN - 1) : 0;
        for (iz x = d->ins; x <= q && x < lim; x++) {
            c3 = hash_insert(d, x);
        }
        d->ins = q + 1;
    }
    return c3;
}

static iz match_len(const u8 *a, const u8 *b, iz max)
{
    iz l = 0;
    while (l + 8 <= max) {
        u64 x, y;
        __builtin_memcpy(&x, a + l, 8);
        __builtin_memcpy(&y, b + l, 8);
        if (x != y) {
            return l + (iz)(__builtin_ctzll(x ^ y) >> 3);
        }
        l += 8;
    }
    while (l < max && a[l] == b[l]) {
        l++;
    }
    return l;
}

static void find_match(deflate *d, iz p, u32 cand, u32 cand3, i32 depth,
                       i32 *out_len, u64 *out_dist)
{
    iz maxlen = d->win_len - p;
    if (maxlen > MAX_MATCH) {
        maxlen = MAX_MATCH;
    }
    i32 best = MIN_MATCH - 1;
    u32 best_g = 0;
    u32 pg = (u32)(d->base + (u64)p);
    u32 base = (u32)d->base;

    while (cand && depth-- > 0) {
        u32 g = cand - 1;
        if (g < base) {
            break;
        }
        iz c = (iz)(g - base);
        if (p - c > WSIZE) {
            break;
        }
        cand = d->prev[g & WMASK];
        __builtin_prefetch(d->win + c);
        if (d->win[c + best] == d->win[p + best] && d->win[c] == d->win[p]) {
            iz l = match_len(d->win + c, d->win + p, maxlen);
            if ((i32)l > best) {
                best = (i32)l;
                best_g = g;
                if (best >= d->nice || (iz)best == maxlen) {
                    break;
                }
                if (best >= d->good && depth > 8) {
                    depth = 8;
                }
            }
        }
    }

    i32 d3 = best < MIN_MATCH ? d->depth3 : 0;
    while (cand3 && d3-- > 0) {
        u32 g = cand3 - 1;
        if (g < base) {
            break;
        }
        iz c = (iz)(g - base);
        if (p - c > D3_MAX) {
            break;
        }
        if (d->win[c] == d->win[p] && d->win[c + 1] == d->win[p + 1] &&
            d->win[c + 2] == d->win[p + 2]) {
            best = MIN_MATCH;
            best_g = g;
            break;
        }
        cand3 = d->prev3[g & WMASK];
    }

    if (best >= MIN_MATCH) {
        *out_len = best;
        *out_dist = pg - best_g;
    } else {
        *out_len = 0;
        *out_dist = 0;
    }
}

static void parse(deflate *d, iz start, iz end)
{
    iz p = start;
    while (p < end) {
        i32 len = 0;
        u64 dist = 0;

        u32 c3 = ensure_insert(d, p);
        if (p + HASH_LEN <= d->win_len) {
            find_match(d, p, d->prev[(u32)(d->base + (u64)p) & WMASK],
                       c3, d->depth, &len, &dist);
        } else if (d->use3 && p + MIN_MATCH <= d->win_len) {
            u32 v = (u32)d->win[p] | ((u32)d->win[p + 1] << 8) |
                    ((u32)d->win[p + 2] << 16);
            u32 h3 = (v * 2654435761u) >> (32 - HASH_BITS);
            find_match(d, p, 0, d->head3[h3], d->depth, &len, &dist);
        }

        if (len >= MIN_MATCH) {
            for (i32 step = 0; step < d->lazy && len >= MIN_MATCH &&
                 len < d->good && p + 1 < end; step++) {
                i32 l2 = 0;
                u64 d2 = 0;
                u32 c3b = ensure_insert(d, p + 1);
                if (p + 1 + HASH_LEN <= d->win_len) {
                    i32 depth = d->depth >> 1;
                    find_match(d, p + 1,
                               d->prev[(u32)(d->base + (u64)(p + 1)) & WMASK],
                               c3b, depth, &l2, &d2);
                } else if (d->use3 && p + 1 + MIN_MATCH <= d->win_len) {
                    u32 v = (u32)d->win[p + 1] | ((u32)d->win[p + 2] << 8) |
                            ((u32)d->win[p + 3] << 16);
                    u32 h3 = (v * 2654435761u) >> (32 - HASH_BITS);
                    find_match(d, p + 1, 0, d->head3[h3], d->depth, &l2, &d2);
                }
                if (l2 > len) {
                    tok_lit(d, d->win[p], d->base + (u64)p);
                    p++;
                    len = l2;
                    dist = d2;
                } else {
                    break;
                }
            }
        }

        if (len >= MIN_MATCH) {
            tok_match(d, len, dist, d->base + (u64)p);
            ensure_insert(d, p + (iz)len - 1);
            p += len;
        } else {
            tok_lit(d, d->win[p], d->base + (u64)p);
            p++;
        }
    }
}

b32 deflate_push(deflate *d, const u8 *data, iz len)
{
    if (!d->sampled) {
        d->sampled = 1;
        iz n = len < 32768 ? len : 32768;
        if (n >= 64) {
            u32 hits = 0;
            for (iz i = 0; i + 4 <= n; i++) {
                u32 v;
                __builtin_memcpy(&v, data + i, 4);
                u32 h = (v * 2654435761u) >> (32 - HASH_BITS);
                if (d->head3[h]) {
                    hits++;
                }
                d->head3[h] = 1;
            }
            iz total = n - 3;
            memset(d->head3, 0, HASH_SIZE * (iz)sizeof(u32));
            d->use3 = hits * 10 < (u32)total * 7;
        }
    }
    iz hist = d->win_len < WSIZE ? d->win_len : WSIZE;
    iz shift = d->win_len - hist;
    if (d->pend_len && d->pend_start < d->base + (u64)shift) {
        flush_pending(d, 0);
    }
    if (hist) {
        memmove(d->win, d->win + shift, hist);
    }
    d->base += (u64)shift;
    d->win_len = hist;
    d->ins = d->ins > shift ? d->ins - shift : 0;
    memcpy(d->win + hist, data, len);
    d->win_len = hist + len;
    iz end = (len < CHUNK_SIZE || d->win_len <= MAX_MATCH)
        ? d->win_len
        : d->win_len - MAX_MATCH;
    if (end > d->ins) {
        parse(d, d->ins, end);
    }
    return !d->out->err;
}

b32 deflate_end(deflate *d)
{
    if (d->win_len > d->ins) {
        parse(d, d->ins, d->win_len);
    }
    flush_block(d, 1);
    bw_align(d);
    return !d->out->err;
}