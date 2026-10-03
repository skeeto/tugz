// gzip core: raw DEFLATE encoder (RFC 1951)
//
// Input accumulates in a sliding window and is parsed into tokens once
// the window fills, so output does not depend on how input is split
// across deflate_push calls. Hash chains store window-relative positions
// plus one (zero is "none"), rebased whenever the window slides.

#define DEF_WSIZE     32768
#define DEF_WMASK     (DEF_WSIZE - 1)
#define MIN_MATCH     3
#define MAX_MATCH     258
#define HASH_LEN      4
#define HASH_BITS     16
#define HASH_SIZE     (1 << HASH_BITS)
#define D3_MAX        4096
#define LOOKAHEAD     (MAX_MATCH + 32)
#define WIN_CAP       ((1 << 20) + DEF_WSIZE)
#define TOK_CAP       65535
#define NLIT          286
#define NDIST         30
#define NCL           19

typedef struct {
    u16 litlen;
    u16 dist;  // zero for literals
} token;

typedef struct {
    i32 depth, nice, lazy, good, depth3;
} deflate_level;

static deflate_level const deflate_levels[10] = {
    {   0,   0,  0,  0,   0},
    {   4,  16,  0,  4,   4},
    {   8,  24,  0,  4,   8},
    {  16,  32,  4,  4,  16},
    {  32,  48,  8,  4,  32},
    {  48,  64,  8,  8,  32},
    { 128, 258,  2, 16,  32},
    { 256, 258,  8,  8,  64},
    { 512, 258,  8, 32, 128},
    {1024, 258, 16, 32, 256},
};

typedef struct {
    u8  len[288];
    u16 code[288];
} htree;

typedef struct {
    writer *out;
    deflate_level lvl;

    u8 *win;
    iz  win_len;
    u64 base;     // stream offset of win[0]
    iz  pos;      // next position to parse
    iz  ins;      // next position to insert into hash chains
    b32 sampled;
    b32 use3;

    u32 *head;
    u32 *prev;
    u32 *head3;
    u32 *prev3;

    token *toks;
    iz     ntok;

    u64 blk_start;
    u64 blk_len;
    u64 pend_start;
    iz  pend_len;

    u32 lit_freq[NLIT];
    u32 dist_freq[NDIST];

    u64 bitbuf;
    i32 bitcnt;

    htree fixlit;
    htree fixdist;
} deflate;

static u16 const def_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
    59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static u8 const def_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
    4, 5, 5, 5, 5, 0
};
static u16 const def_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static u8 const def_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
    10, 10, 11, 11, 12, 12, 13, 13
};
static u8 const def_cl_order[NCL] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

// Length code index (0..28) for a match length in 3..258.
static i32 length_code(i32 len)
{
    assert(len>=MIN_MATCH && len<=MAX_MATCH);
    u32 x = (u32)len - 3;
    if (x < 8) {
        return (i32)x;
    } else if (len == 258) {
        return 28;
    }
    i32 b = 31 - __builtin_clz(x);
    return 4*(b - 1) + (i32)(x>>(b - 2) & 3);
}

// Distance code (0..29) for a distance in 1..32768.
static i32 dist_code(i32 dist)
{
    assert(dist>=1 && dist<=DEF_WSIZE);
    u32 x = (u32)dist - 1;
    if (x < 4) {
        return (i32)x;
    }
    i32 b = 31 - __builtin_clz(x);
    return 2*b + (i32)(x>>(b - 1) & 1);
}

static u32 bit_reverse(u32 v, i32 n)
{
    u32 r = 0;
    for (i32 i = 0; i < n; i++, v >>= 1) {
        r = r<<1 | (v&1);
    }
    return r;
}

// Assign canonical codes from lengths already stored in t.
static void huff_codes(htree *t, i32 n)
{
    u32 count[16] = {0};
    for (i32 i = 0; i < n; i++) {
        count[t->len[i]]++;
    }
    count[0] = 0;
    u32 next[16];
    u32 code = 0;
    for (i32 b = 1; b < 16; b++) {
        code = (code + count[b-1]) << 1;
        next[b] = code;
    }
    for (i32 i = 0; i < n; i++) {
        i32 l = t->len[i];
        t->code[i] = l ? (u16)bit_reverse(next[l]++, l) : 0;
    }
}

// Moffat & Katajainen in-place minimum redundancy code. On input, w holds
// n weights in ascending order. On output it holds code lengths, which
// are therefore in descending order.
static void min_redundancy(u32 *w, i32 n)
{
    if (n == 1) {
        w[0] = 1;
        return;
    }

    w[0] += w[1];
    i32 root = 0;
    i32 leaf = 2;
    for (i32 next = 1; next < n-1; next++) {
        if (leaf>=n || w[root]<w[leaf]) {
            w[next] = w[root];
            w[root++] = (u32)next;
        } else {
            w[next] = w[leaf++];
        }
        if (leaf>=n || (root<next && w[root]<w[leaf])) {
            w[next] += w[root];
            w[root++] = (u32)next;
        } else {
            w[next] += w[leaf++];
        }
    }

    w[n-2] = 0;
    for (i32 next = n-3; next >= 0; next--) {
        w[next] = w[w[next]] + 1;
    }

    i32 avail = 1;
    i32 used  = 0;
    u32 depth = 0;
    root = n - 2;
    for (i32 next = n-1; avail > 0; depth++) {
        for (; root>=0 && w[root]==depth; root--) {
            used++;
        }
        for (; avail > used; avail--) {
            w[next--] = depth;
        }
        avail = 2 * used;
        used = 0;
    }
}

// Build a complete, length-limited Huffman code. At least two symbols
// always get codes, as zlib does, so every code is complete.
static void huff_build(htree *t, u32 const *freq, i32 n, i32 maxlen)
{
    assert(n <= 288);
    i32 syms[288];
    u32 w[288];
    i32 m = 0;

    for (i32 i = 0; i < n; i++) {
        t->len[i] = 0;
        if (freq[i]) {
            syms[m++] = i;
        }
    }
    for (i32 i = 0; m < 2; i++) {
        if (!freq[i]) {
            syms[m++] = i;
        }
    }

    // Insertion sort by weight, ties by symbol
    for (i32 i = 0; i < m; i++) {
        i32 s  = syms[i];
        u32 ws = freq[s] ? freq[s] : 1;
        i32 j  = i;
        for (; j > 0; j--) {
            i32 p  = syms[j-1];
            u32 wp = freq[p] ? freq[p] : 1;
            if (wp<ws || (wp==ws && p<s)) {
                break;
            }
            syms[j] = p;
            w[j] = wp;
        }
        syms[j] = s;
        w[j] = ws;
    }

    min_redundancy(w, m);

    // Clamp to maxlen, then restore the Kraft equality by lengthening
    // shorter codes until the overflow is absorbed.
    u32 count[16] = {0};
    for (i32 i = 0; i < m; i++) {
        count[MIN(w[i], (u32)maxlen)]++;
    }
    u32 total = 0;
    for (i32 b = 1; b <= maxlen; b++) {
        total += count[b] << (maxlen - b);
    }
    for (; total != 1u<<maxlen; total--) {
        count[maxlen]--;
        for (i32 b = maxlen-1; b > 0; b--) {
            if (count[b]) {
                count[b]--;
                count[b+1] += 2;
                break;
            }
        }
    }

    // Lowest weights first, so assign longest lengths first
    i32 k = 0;
    for (i32 b = maxlen; b > 0; b--) {
        for (u32 j = 0; j < count[b]; j++) {
            t->len[syms[k++]] = (u8)b;
        }
    }
    huff_codes(t, n);
}

static deflate *deflate_new(arena *a, i32 level, writer *out)
{
    deflate *d = new(a, 1, deflate);
    d->out   = out;
    d->lvl   = deflate_levels[MAX(1, MIN(level, 9))];
    d->win   = newbytes(a, WIN_CAP);
    d->head  = new(a, HASH_SIZE, u32);
    d->prev  = new(a, DEF_WSIZE, u32);
    d->head3 = new(a, HASH_SIZE, u32);
    d->prev3 = new(a, DEF_WSIZE, u32);
    d->toks  = new(a, TOK_CAP, token);

    for (i32 i = 0; i < 288; i++) {
        d->fixlit.len[i] = (u8)(i<144 ? 8 : i<256 ? 9 : i<280 ? 7 : 8);
    }
    huff_codes(&d->fixlit, 288);
    for (i32 i = 0; i < NDIST; i++) {
        d->fixdist.len[i] = 5;
    }
    huff_codes(&d->fixdist, NDIST);
    return d;
}

static void bw_put(deflate *d, u32 v, i32 n)
{
    d->bitbuf |= (u64)(v & ((1u<<n) - 1)) << d->bitcnt;
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

static i32 cl_extra_bits(i32 s)
{
    return s==16 ? 2 : s==17 ? 3 : s==18 ? 7 : 0;
}

// Run-length encode code lengths into code length code symbols.
static i32 rle_lengths(u8 const *lens, i32 n, u8 *sym, u8 *extra)
{
    i32 m = 0;
    for (i32 i = 0; i < n;) {
        i32 l = lens[i];
        i32 j = i + 1;
        for (; j<n && lens[j]==l; j++) {}
        i32 run = j - i;
        i = j;

        if (!l) {
            for (; run >= 11; m++) {
                i32 r = MIN(run, 138);
                sym[m] = 18;
                extra[m] = (u8)(r - 11);
                run -= r;
            }
            for (; run >= 3; m++) {
                i32 r = MIN(run, 10);
                sym[m] = 17;
                extra[m] = (u8)(r - 3);
                run -= r;
            }
        } else {
            sym[m] = (u8)l;
            extra[m++] = 0;
            run--;
            for (; run >= 3; m++) {
                i32 r = MIN(run, 6);
                sym[m] = 16;
                extra[m] = (u8)(r - 3);
                run -= r;
            }
        }
        for (; run > 0; run--, m++) {
            sym[m] = (u8)l;
            extra[m] = 0;
        }
    }
    return m;
}

typedef struct {
    htree lt, dt, cl;
    i32   hlit, hdist, hclen;
    i32   nrle;
    u8    rsym[NLIT+NDIST];
    u8    rextra[NLIT+NDIST];
} dynblock;

static void build_dyn(deflate *d, dynblock *b)
{
    u32 lf[NLIT];
    for (i32 i = 0; i < NLIT; i++) {
        lf[i] = d->lit_freq[i];
    }
    lf[256] = 1;
    huff_build(&b->lt, lf, NLIT, 15);
    huff_build(&b->dt, d->dist_freq, NDIST, 15);

    i32 hlit = NLIT;
    for (; hlit>257 && !b->lt.len[hlit-1]; hlit--) {}
    i32 hdist = NDIST;
    for (; hdist>1 && !b->dt.len[hdist-1]; hdist--) {}
    b->hlit  = hlit;
    b->hdist = hdist;

    u8 lens[NLIT+NDIST];
    bytecopy(lens, b->lt.len, hlit);
    bytecopy(lens+hlit, b->dt.len, hdist);
    b->nrle = rle_lengths(lens, hlit+hdist, b->rsym, b->rextra);

    u32 clf[NCL] = {0};
    for (i32 i = 0; i < b->nrle; i++) {
        clf[b->rsym[i]]++;
    }
    huff_build(&b->cl, clf, NCL, 7);

    i32 hclen = NCL;
    for (; hclen>4 && !b->cl.len[def_cl_order[hclen-1]]; hclen--) {}
    b->hclen = hclen;
}

static u64 cost_tokens(deflate *d, htree *lt, htree *dt)
{
    u64 bits = lt->len[256];
    for (iz i = 0; i < d->ntok; i++) {
        token t = d->toks[i];
        if (!t.dist) {
            bits += lt->len[t.litlen];
        } else {
            i32 lc = length_code(t.litlen);
            i32 dc = dist_code(t.dist);
            bits += lt->len[257+lc] + def_len_extra[lc];
            bits += dt->len[dc] + def_dist_extra[dc];
        }
    }
    return bits;
}

static u64 cost_dyn(deflate *d, dynblock *b)
{
    u64 bits = 3 + 5 + 5 + 4 + 3*(u64)b->hclen;
    for (i32 i = 0; i < b->nrle; i++) {
        bits += b->cl.len[b->rsym[i]] + (u64)cl_extra_bits(b->rsym[i]);
    }
    return bits + cost_tokens(d, &b->lt, &b->dt);
}

static u64 cost_stored(deflate *d)
{
    u64 n = d->blk_len;
    u64 nchunks = (n + 65534) / 65535;
    u64 pad = (8 - ((u64)d->bitcnt + 3)%8) % 8;
    return 3 + pad + 32 + 8*n + (nchunks - 1)*40;
}

static void emit_stored_chunk(deflate *d, iz start, iz len, b32 final)
{
    assert(len>=0 && len<=65535);
    bw_put(d, (u32)final, 1);
    bw_put(d, 0, 2);
    bw_align(d);
    bw_put(d, (u32)len, 16);
    bw_put(d, ~(u32)len, 16);
    writer_write(d->out, d->win+start, len);
}

static void flush_pending(deflate *d, b32 final)
{
    iz start = (iz)(d->pend_start - d->base);
    iz len = d->pend_len;
    d->pend_len = 0;
    if (!len && final) {
        emit_stored_chunk(d, 0, 0, 1);
    }
    for (iz off = 0; off < len;) {
        iz n = MIN(len-off, 65535);
        emit_stored_chunk(d, start+off, n, final && off+n==len);
        off += n;
    }
}

static void emit_stored(deflate *d, b32 final)
{
    u64 start = d->blk_start;
    iz  len   = (iz)d->blk_len;
    if (d->pend_len) {
        if (d->pend_start+(u64)d->pend_len == start) {
            start = d->pend_start;
            len += d->pend_len;
            d->pend_len = 0;
        } else {
            flush_pending(d, 0);
        }
    }

    // Hold back the final partial chunk so that it may merge with the
    // next stored block.
    iz off = 0;
    for (; len-off > 65535; off += 65535) {
        emit_stored_chunk(d, (iz)(start-d->base)+off, 65535, 0);
    }
    d->pend_start = start + (u64)off;
    d->pend_len = len - off;
    if (final) {
        flush_pending(d, 1);
    }
}

static void emit_tokens(deflate *d, htree *lt, htree *dt)
{
    for (iz i = 0; i < d->ntok; i++) {
        token t = d->toks[i];
        if (!t.dist) {
            bw_put(d, lt->code[t.litlen], lt->len[t.litlen]);
        } else {
            i32 lc = length_code(t.litlen);
            i32 dc = dist_code(t.dist);
            bw_put(d, lt->code[257+lc], lt->len[257+lc]);
            bw_put(d, t.litlen - def_len_base[lc], def_len_extra[lc]);
            bw_put(d, dt->code[dc], dt->len[dc]);
            bw_put(d, t.dist - def_dist_base[dc], def_dist_extra[dc]);
        }
    }
    bw_put(d, lt->code[256], lt->len[256]);
}

static void emit_dynamic(deflate *d, dynblock *b)
{
    bw_put(d, 2, 2);
    bw_put(d, (u32)(b->hlit - 257), 5);
    bw_put(d, (u32)(b->hdist - 1), 5);
    bw_put(d, (u32)(b->hclen - 4), 4);
    for (i32 i = 0; i < b->hclen; i++) {
        bw_put(d, b->cl.len[def_cl_order[i]], 3);
    }
    for (i32 i = 0; i < b->nrle; i++) {
        i32 s = b->rsym[i];
        bw_put(d, b->cl.code[s], b->cl.len[s]);
        bw_put(d, b->rextra[i], cl_extra_bits(s));
    }
    emit_tokens(d, &b->lt, &b->dt);
}

static void flush_block(deflate *d, b32 final)
{
    if (!d->ntok) {
        if (final) {
            if (d->pend_len) {
                flush_pending(d, 1);
            } else {
                bw_put(d, 1, 1);
                bw_put(d, 1, 2);
                bw_put(d, d->fixlit.code[256], d->fixlit.len[256]);
            }
        }
        return;
    }

    dynblock b;
    build_dyn(d, &b);

    u64 cd = cost_dyn(d, &b);
    u64 cf = 3 + cost_tokens(d, &d->fixlit, &d->fixdist);
    b32 can_store = d->blk_start>=d->base &&
                    d->blk_start-d->base+d->blk_len <= (u64)d->win_len;
    u64 cs = can_store ? cost_stored(d) : (u64)-1;

    if (cs<=cd && cs<=cf) {
        emit_stored(d, final);
    } else {
        if (d->pend_len) {
            flush_pending(d, 0);
        }
        bw_put(d, (u32)final, 1);
        if (cf <= cd) {
            bw_put(d, 1, 2);
            emit_tokens(d, &d->fixlit, &d->fixdist);
        } else {
            emit_dynamic(d, &b);
        }
    }

    d->ntok = 0;
    d->blk_len = 0;
    bytefill(d->lit_freq, 0, sizeof(d->lit_freq));
    bytefill(d->dist_freq, 0, sizeof(d->dist_freq));
}

static void tok_lit(deflate *d, iz p)
{
    if (!d->ntok) {
        d->blk_start = d->base + (u64)p;
    }
    u8 c = d->win[p];
    d->blk_len++;
    d->toks[d->ntok++] = (token){c, 0};
    d->lit_freq[c]++;
    if (d->ntok == TOK_CAP) {
        flush_block(d, 0);
    }
}

static void tok_match(deflate *d, iz p, i32 len, i32 dist)
{
    if (!d->ntok) {
        d->blk_start = d->base + (u64)p;
    }
    d->blk_len += (u64)len;
    d->toks[d->ntok++] = (token){(u16)len, (u16)dist};
    d->lit_freq[257+length_code(len)]++;
    d->dist_freq[dist_code(dist)]++;
    if (d->ntok == TOK_CAP) {
        flush_block(d, 0);
    }
}

// Index into prev tables for window position p.
static u32 chain_slot(deflate *d, iz p)
{
    return (u32)(d->base + (u64)p) & DEF_WMASK;
}

static u32 load32(u8 const *p)
{
    u32 v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

static u32 hash4(u32 v)
{
    return (v * 2654435761u) >> (32 - HASH_BITS);
}

static u32 hash3(u32 v)
{
    return ((v & 0xffffff) * 2654435761u) >> (32 - HASH_BITS);
}

// Insert position p, returning the previous 3-byte hash chain head.
static u32 hash_insert(deflate *d, iz p)
{
    u32 v = load32(d->win + p);
    u32 slot = chain_slot(d, p);
    u32 h = hash4(v);
    d->prev[slot] = d->head[h];
    d->head[h] = (u32)p + 1;
    u32 prev3 = 0;
    if (d->use3) {
        u32 h3 = hash3(v);
        prev3 = d->head3[h3];
        d->prev3[slot] = prev3;
        d->head3[h3] = (u32)p + 1;
    }
    return prev3;
}

// Insert all positions through q, returning the 3-byte chain for q.
static u32 ensure_insert(deflate *d, iz q)
{
    u32 c3 = 0;
    if (q >= d->ins) {
        iz lim = d->win_len - (HASH_LEN - 1);
        for (iz x = d->ins; x<=q && x<lim; x++) {
            c3 = hash_insert(d, x);
        }
        d->ins = q + 1;
    }
    return c3;
}

static i32 match_len(u8 const *a, u8 const *b, i32 max)
{
    i32 len = 0;
    for (; len+8 <= max; len += 8) {
        u64 x, y;
        __builtin_memcpy(&x, a+len, 8);
        __builtin_memcpy(&y, b+len, 8);
        if (x != y) {
            return len + (__builtin_ctzll(x ^ y) >> 3);
        }
    }
    for (; len<max && a[len]==b[len]; len++) {}
    return len;
}

typedef struct {
    i32 len;
    i32 dist;
} match;

static match find_match(deflate *d, iz p, u32 cand, u32 cand3, i32 depth)
{
    u8 *win = d->win;
    i32 maxlen = (i32)MIN(d->win_len - p, MAX_MATCH);
    i32 best = MIN_MATCH - 1;
    iz  best_pos = 0;

    for (; cand && depth > 0; depth--) {
        iz c = (iz)cand - 1;
        if (p-c > DEF_WSIZE) {
            break;
        }
        cand = d->prev[chain_slot(d, c)];
        if (win[c+best]==win[p+best] && win[c]==win[p]) {
            i32 len = match_len(win+c, win+p, maxlen);
            if (len > best) {
                best = len;
                best_pos = c;
                if (best>=d->lvl.nice || best==maxlen) {
                    break;
                }
                if (best>=d->lvl.good && depth>8) {
                    depth = 8;
                }
            }
        }
    }

    // Fall back to short, close 3-byte matches
    i32 depth3 = best<MIN_MATCH ? d->lvl.depth3 : 0;
    for (; cand3 && depth3 > 0; depth3--) {
        iz c = (iz)cand3 - 1;
        if (p-c > D3_MAX) {
            break;
        }
        if (win[c]==win[p] && win[c+1]==win[p+1] && win[c+2]==win[p+2]) {
            best = MIN_MATCH;
            best_pos = c;
            break;
        }
        cand3 = d->prev3[chain_slot(d, c)];
    }

    match r = {0, 0};
    if (best >= MIN_MATCH) {
        r.len  = best;
        r.dist = (i32)(p - best_pos);
    }
    return r;
}

// Find the best match at p, inserting p into the hash chains.
static match match_at(deflate *d, iz p, i32 depth)
{
    u32 c3 = ensure_insert(d, p);
    if (p+HASH_LEN <= d->win_len) {
        return find_match(d, p, d->prev[chain_slot(d, p)], c3, depth);
    } else if (d->use3 && p+MIN_MATCH <= d->win_len) {
        u32 v = (u32)d->win[p] | (u32)d->win[p+1]<<8 | (u32)d->win[p+2]<<16;
        return find_match(d, p, 0, d->head3[hash3(v)], depth);
    }
    return (match){0, 0};
}

// Decide whether 3-byte matching is worthwhile by sampling how often
// 4-byte hashes collide in the first part of the input.
static void sample(deflate *d)
{
    d->sampled = 1;
    iz n = MIN(d->win_len, 32768);
    if (n < 64) {
        return;
    }
    u32 hits = 0;
    for (iz i = 0; i+4 <= n; i++) {
        u32 h = hash4(load32(d->win + i));
        hits += d->head3[h] != 0;
        d->head3[h] = 1;
    }
    bytefill(d->head3, 0, HASH_SIZE*(iz)sizeof(u32));
    d->use3 = (u64)hits*10 < (u64)(n - 3)*7;
}

static void parse(deflate *d, iz end)
{
    if (!d->sampled) {
        sample(d);
    }

    iz p = d->pos;
    while (p < end) {
        match m = match_at(d, p, d->lvl.depth);

        // Lazy matching: prefer a longer match starting one byte later
        for (i32 step = 0; step < d->lvl.lazy; step++) {
            if (m.len<MIN_MATCH || m.len>=d->lvl.good || p+1>=d->win_len) {
                break;
            }
            match m2 = match_at(d, p+1, d->lvl.depth>>1);
            if (m2.len <= m.len) {
                break;
            }
            tok_lit(d, p++);
            m = m2;
        }

        if (m.len >= MIN_MATCH) {
            tok_match(d, p, m.len, m.dist);
            ensure_insert(d, p + m.len - 1);
            p += m.len;
        } else {
            tok_lit(d, p++);
        }
    }
    d->pos = p;
}

static u32 slide_entry(u32 v, u32 shift)
{
    return v>shift ? v-shift : 0;
}

// Discard window contents more than WSIZE behind the parse position.
static void slide(deflate *d)
{
    iz shift = d->pos - DEF_WSIZE;
    if (shift <= 0) {
        return;
    }
    if (d->pend_len && d->pend_start < d->base+(u64)shift) {
        flush_pending(d, 0);
    }
    bytemove(d->win, d->win+shift, d->win_len-shift);
    d->base    += (u64)shift;
    d->win_len -= shift;
    d->pos     -= shift;
    d->ins     -= shift;

    u32 s = (u32)shift;
    for (i32 i = 0; i < HASH_SIZE; i++) {
        d->head[i] = slide_entry(d->head[i], s);
    }
    for (i32 i = 0; i < DEF_WSIZE; i++) {
        d->prev[i] = slide_entry(d->prev[i], s);
    }
    if (d->use3) {
        for (i32 i = 0; i < HASH_SIZE; i++) {
            d->head3[i] = slide_entry(d->head3[i], s);
        }
        for (i32 i = 0; i < DEF_WSIZE; i++) {
            d->prev3[i] = slide_entry(d->prev3[i], s);
        }
    }
}

static void deflate_push(deflate *d, u8 const *data, iz len)
{
    while (len) {
        if (d->win_len == WIN_CAP) {
            parse(d, WIN_CAP - LOOKAHEAD);
            slide(d);
        }
        iz n = MIN(len, WIN_CAP - d->win_len);
        bytecopy(d->win + d->win_len, data, n);
        d->win_len += n;
        data += n;
        len -= n;
    }
}

static void deflate_finish(deflate *d)
{
    parse(d, d->win_len);
    flush_block(d, 1);
    bw_align(d);
}
