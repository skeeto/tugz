// tugz core: raw DEFLATE encoder (RFC 1951)
//
// Input accumulates in a sliding window and is parsed into tokens once
// the window fills (or at a flush), so output depends only on the input
// and flush points, not on how input and output buffers are split
// across calls. Hash chains store window-relative positions plus one
// (zero is "none") plus a stamp (see forget), rebased whenever the
// window slides.
//
// Output is staged in a buffer from which the caller drains it. Each
// emission step (one block, a window slide, or a flush) requires
// DEF_STAGE_NEED bytes of staging room, which bounds its output: a block
// holds at most TOK_CAP tokens of at most 48 bits each, a stored block
// is only chosen when no larger than that, and held-back stored data is
// under 64 KiB. Parsing pauses when a block is ready but there is no
// room, and resumes once the caller has drained output.

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
#define TOK_LIMIT     (TOK_CAP - 3)  // headroom for one lazy parse step
#define DEF_STAGE_NEED (TOK_CAP*6 + 2*(65535+5) + 1024)
#define DEF_STAGE     (DEF_STAGE_NEED + (1 << 16))
#define NLIT          286
#define NDIST         30
#define NCL           19
#define NOBS          10    // block split observation categories
#define OBS_BATCH     512   // observations between block split checks
#define MIN_BLOCK     10000 // minimum block size in bytes for splitting
#define FORGET_REHASH 1024  // longest history to clear by rehashing
#define STAMP_STEP    (1u << 21)  // stamps' unit, above any position
#define STAMP_LAST    (0u - STAMP_STEP)

_Static_assert(WIN_CAP < STAMP_STEP, "a stamp step exceeds positions");
_Static_assert(STAMP_STEP%DEF_WSIZE == 0, "stamps keep chain slots");

// Flush modes, matching the library's
enum {
    DEF_NONE,
    DEF_SYNC,    // byte-align output with an empty stored block
    DEF_FULL,    // also forget history
    DEF_FINISH,  // end the stream
};

typedef struct {
    u16 litlen;
    u16 dist;  // zero for literals
} token;

typedef struct {
    i32 depth;   // hash chain search depth
    i32 nice;    // stop searching at a match this long
    i32 lazy;    // lazy evaluation steps (0 for greedy)
    i32 good;    // skip lazy evaluation, and shorten search, past this
    i32 depth3;  // 3-byte chain search depth
    i32 insert;  // insert positions inside matches up to this long
} deflate_level;

// Chosen from a sweep over Silesia: each level beats zlib's compression
// ratio at the same level while running faster.
static deflate_level const deflate_levels[10] = {
    // depth nice lazy good depth3 insert
    {   0,    0,  0,   0,    0,   0},
    {   4,   16,  0,   4,    4,  16},
    {   6,   32,  0,   8,    6,  16},
    {   8,   32,  0,   8,    8,  32},
    {   8,   32,  1,   8,    8, 258},
    {  16,   64,  1,  16,   16, 258},
    {  32,   64,  1,  16,   32, 258},
    {  64,  128,  2,  32,   32, 258},
    { 256,  258,  2,  64,   64, 258},
    {1024,  258,  2, 258,  256, 258},
};

typedef struct {
    u8  len[288];
    u16 code[288];
} htree;

// A dynamic block's codes and their run-length encoded description.
// Kept in the deflator rather than on the stack, whose frame would
// otherwise exceed a page (a stack probe on Windows).
typedef struct {
    htree lt, dt, cl;
    i32   hlit, hdist, hclen;
    i32   nrle;
    u8    rsym[NLIT+NDIST];
    u8    rextra[NLIT+NDIST];
} dynblock;

typedef struct {
    deflate_level lvl;

    u8 *obuf;     // staged output: [ooff, olen) is pending
    iz  olen;
    iz  ooff;
    b32 blkready; // a block is complete and should be emitted
    b32 flushing; // a flush has been emitted, awaiting drain
    b32 finished;

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
    u32 *seen;    // sample's bitmap of hashes, zero between samples
    u32  stamp;   // added to hash table entries; see forget

    token *toks;
    iz     ntok;

    u64 blk_start;
    u64 blk_len;
    u64 pend_start;
    iz  pend_len;

    // Block splitting statistics: coarse symbol categories observed in
    // the current block, and in the latest batch of observations.
    u32 obs[NOBS];
    u32 newobs[NOBS];
    u32 nobs;
    u32 nnewobs;

    u32 lit_freq[NLIT];
    u32 dist_freq[NDIST];

    u64 bitbuf;
    i32 bitcnt;

    htree fixlit;
    htree fixdist;
    dynblock dyn;

    u8 lcode[MAX_MATCH+1];  // length -> length code index
    u8 dcode[512];          // see dcode_of
} deflator;

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

static i32 lcode_of(deflator *d, i32 len)
{
    return d->lcode[len];
}

static i32 dcode_of(deflator *d, i32 dist)
{
    return d->dcode[dist<=256 ? dist-1 : 256 + ((dist-1)>>7)];
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

// Append n bits, n <= 32. Whole bytes spill into the staging buffer
// 8 bytes at a time once 32 or more bits accumulate.
static void bw_put(deflator *d, u64 v, i32 n)
{
    assert(n<=32 && !(v>>n));
    d->bitbuf |= v << d->bitcnt;
    d->bitcnt += n;
    if (d->bitcnt >= 32) {
        assert(DEF_STAGE-d->olen >= 8);
        store64le(d->obuf+d->olen, d->bitbuf);
        d->olen += d->bitcnt >> 3;
        d->bitbuf >>= d->bitcnt & ~7;
        d->bitcnt &= 7;
    }
}

// Pad to a byte boundary and spill all bits.
static void bw_align(deflator *d)
{
    d->bitcnt = (d->bitcnt + 7) & ~7;
    for (; d->bitcnt; d->bitcnt -= 8) {
        assert(d->olen < DEF_STAGE);
        d->obuf[d->olen++] = (u8)d->bitbuf;
        d->bitbuf >>= 8;
    }
}

// Append whole bytes, such as a container header or trailer, at a byte
// boundary.
static void deflate_bytes(deflator *d, u8 const *p, iz len)
{
    assert(!d->bitcnt && len<=DEF_STAGE-d->olen);
    bytecopy(d->obuf+d->olen, p, len);
    d->olen += len;
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

static void build_dyn(deflator *d, dynblock *b)
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

// Size of the block's symbols under the given codes, from frequencies.
static u64 cost_tokens(deflator *d, htree *lt, htree *dt)
{
    u64 bits = lt->len[256];
    for (i32 i = 0; i < 256; i++) {
        bits += (u64)d->lit_freq[i] * lt->len[i];
    }
    for (i32 i = 0; i < 29; i++) {
        bits += (u64)d->lit_freq[257+i] * (lt->len[257+i] + def_len_extra[i]);
    }
    for (i32 i = 0; i < NDIST; i++) {
        bits += (u64)d->dist_freq[i] * (dt->len[i] + def_dist_extra[i]);
    }
    return bits;
}

static u64 cost_dyn(deflator *d, dynblock *b)
{
    u64 bits = 3 + 5 + 5 + 4 + 3*(u64)b->hclen;
    for (i32 i = 0; i < b->nrle; i++) {
        bits += b->cl.len[b->rsym[i]] + (u64)cl_extra_bits(b->rsym[i]);
    }
    return bits + cost_tokens(d, &b->lt, &b->dt);
}

static u64 cost_stored(deflator *d)
{
    u64 n = d->blk_len;
    u64 nchunks = (n + 65534) / 65535;
    u64 pad = (8 - ((u64)d->bitcnt + 3)%8) % 8;
    return 3 + pad + 32 + 8*n + (nchunks - 1)*40;
}

static void emit_stored_chunk(deflator *d, iz start, iz len, b32 final)
{
    assert(len>=0 && len<=65535);
    bw_put(d, (u32)final, 1);
    bw_put(d, 0, 2);
    bw_align(d);
    bw_put(d, (u32)len | (~(u32)len & 0xffff)<<16, 32);
    bw_align(d);
    deflate_bytes(d, d->win+start, len);
}

static void flush_pending(deflator *d, b32 final)
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

static void emit_stored(deflator *d, b32 final)
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

static void emit_tokens(deflator *d, htree *lt, htree *dt)
{
    for (iz i = 0; i < d->ntok; i++) {
        token t = d->toks[i];
        if (!t.dist) {
            bw_put(d, lt->code[t.litlen], lt->len[t.litlen]);
        } else {
            // Code plus extra bits: at most 15+5 and 15+13 bits
            i32 lc = lcode_of(d, t.litlen);
            i32 dc = dcode_of(d, t.dist);
            u32 lx = (u32)(t.litlen - def_len_base[lc]);
            u32 dx = (u32)(t.dist - def_dist_base[dc]);
            i32 ln = lt->len[257+lc];
            i32 dn = dt->len[dc];
            bw_put(d, lt->code[257+lc] | lx<<ln, ln + def_len_extra[lc]);
            bw_put(d, dt->code[dc] | dx<<dn, dn + def_dist_extra[dc]);
        }
    }
    bw_put(d, lt->code[256], lt->len[256]);
}

static void emit_dynamic(deflator *d, dynblock *b)
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

// Empty the token buffer and its statistics for the next block.
static void clear_block(deflator *d)
{
    d->ntok = 0;
    d->blk_len = 0;
    bytefill(d->lit_freq, 0, sizeof(d->lit_freq));
    bytefill(d->dist_freq, 0, sizeof(d->dist_freq));
    bytefill(d->obs, 0, sizeof(d->obs));
    bytefill(d->newobs, 0, sizeof(d->newobs));
    d->nobs = d->nnewobs = 0;
}

static void flush_block(deflator *d, b32 final)
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

    dynblock *b = &d->dyn;
    build_dyn(d, b);

    u64 cd = cost_dyn(d, b);
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
            emit_dynamic(d, b);
        }
    }

    clear_block(d);
}

// Decide whether the latest batch of observations differs enough from
// the block so far that a new block should start, so that its Huffman
// codes can adapt. The idea follows libdeflate: compare the batch's
// category distribution to the block's, scaling both to a common total,
// with a bias toward longer blocks that grows as the block does.
static b32 should_split(deflator *d)
{
    b32 split = 0;
    if (d->nobs) {
        u64 delta = 0;
        for (i32 i = 0; i < NOBS; i++) {
            u64 expect = (u64)d->obs[i] * d->nnewobs;
            u64 actual = (u64)d->newobs[i] * d->nobs;
            delta += actual>expect ? actual-expect : expect-actual;
        }
        u64 total  = (u64)d->nobs + d->nnewobs;
        u64 cutoff = (u64)d->nnewobs * 200/512 * d->nobs;
        if (d->blk_len<MIN_BLOCK && total<8192) {
            cutoff += cutoff * (8192 - total) / 8192;
        }
        split = d->blk_len>=MIN_BLOCK &&
                delta + d->blk_len/4096*d->nobs >= cutoff;
    }
    for (i32 i = 0; i < NOBS; i++) {
        d->obs[i] += d->newobs[i];
        d->newobs[i] = 0;
    }
    d->nobs += d->nnewobs;
    d->nnewobs = 0;
    return split;
}

static void tok_end(deflator *d)
{
    if (!d->blkready && (d->ntok>=TOK_LIMIT ||
                         (++d->nnewobs==OBS_BATCH && should_split(d)))) {
        d->blkready = 1;
    }
}

static void tok_lit(deflator *d, iz p)
{
    if (!d->ntok) {
        d->blk_start = d->base + (u64)p;
    }
    u8 c = d->win[p];
    d->blk_len++;
    d->toks[d->ntok++] = (token){c, 0};
    d->lit_freq[c]++;
    d->newobs[(c>>5 & 6) | (c & 1)]++;
    tok_end(d);
}

static void tok_match(deflator *d, iz p, i32 len, i32 dist)
{
    if (!d->ntok) {
        d->blk_start = d->base + (u64)p;
    }
    d->blk_len += (u64)len;
    d->toks[d->ntok++] = (token){(u16)len, (u16)dist};
    d->lit_freq[257+lcode_of(d, len)]++;
    d->dist_freq[dcode_of(d, dist)]++;
    d->newobs[8 + (len >= 9)]++;
    tok_end(d);
}

// Index into prev tables for window position p.
static u32 chain_slot(deflator *d, iz p)
{
    return (u32)(d->base + (u64)p) & DEF_WMASK;
}

// Index into prev tables for a hash table entry's position. Stamps are
// multiples of the window size, so they drop out.
static u32 entry_slot(deflator *d, u32 entry)
{
    return ((u32)d->base + entry - 1) & DEF_WMASK;
}

// Little-endian, so hashes (and therefore output) match across hosts.
static u32 load32(u8 const *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
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
static u32 hash_insert(deflator *d, iz p)
{
    u32 v = load32(d->win + p);
    u32 slot = chain_slot(d, p);
    u32 entry = (u32)p + 1 + d->stamp;
    u32 h = hash4(v);
    d->prev[slot] = d->head[h];
    d->head[h] = entry;
    u32 prev3 = 0;
    if (d->use3) {
        u32 h3 = hash3(v);
        prev3 = d->head3[h3];
        d->prev3[slot] = prev3;
        d->head3[h3] = entry;
    }
    return prev3;
}

// Insert all positions through q, returning the 3-byte chain for q.
static u32 ensure_insert(deflator *d, iz q)
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
        u64 x = load64le(a+len) ^ load64le(b+len);
        if (x) {
            return len + (__builtin_ctzll(x) >> 3);  // first differing byte
        }
    }
    for (; len<max && a[len]==b[len]; len++) {}
    return len;
}

typedef struct {
    i32 len;
    i32 dist;
} match;

// Candidates are hash table entries, from which distances are measured
// against p's own entry: a forgotten entry is farther than any match.
static match find_match(deflator *d, iz p, u32 cand, u32 cand3, i32 depth)
{
    u8 *win = d->win;
    i32 maxlen = (i32)MIN(d->win_len - p, MAX_MATCH);
    i32 best = MIN_MATCH - 1;
    iz  best_pos = 0;
    u32 here = (u32)p + 1 + d->stamp;

    for (; cand && depth > 0; depth--) {
        u32 dist = here - cand;
        if (dist > DEF_WSIZE) {
            break;
        }
        iz c = p - (iz)dist;
        // A candidate exactly WSIZE back shares p's prev slot, where
        // inserting p linked this walk's first candidate. Revisiting
        // candidates cannot beat best, so make this one the last.
        if (dist == DEF_WSIZE) {
            depth = 1;
        }
        cand = d->prev[entry_slot(d, cand)];
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
        u32 dist = here - cand3;
        if (dist > D3_MAX) {
            break;
        }
        iz c = p - (iz)dist;
        if (win[c]==win[p] && win[c+1]==win[p+1] && win[c+2]==win[p+2]) {
            best = MIN_MATCH;
            best_pos = c;
            break;
        }
        cand3 = d->prev3[entry_slot(d, cand3)];
    }

    match r = {0, 0};
    if (best >= MIN_MATCH) {
        r.len  = best;
        r.dist = (i32)(p - best_pos);
    }
    return r;
}

// Find the best match at p, inserting p into the hash chains.
static match match_at(deflator *d, iz p, i32 depth)
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
static void sample(deflator *d)
{
    d->sampled = 1;
    iz n = MIN(d->win_len, 32768);
    if (n < 64) {
        return;
    }
    u32 *seen = d->seen;
    u32  hits = 0;
    for (iz i = 0; i+4 <= n; i++) {
        u32 h   = hash4(load32(d->win + i));
        u32 bit = (u32)1 << (h & 31);
        hits += (seen[h>>5] & bit) != 0;
        seen[h>>5] |= bit;
    }
    bytefill(seen, 0, HASH_SIZE/8);
    d->use3 = (u64)hits*10 < (u64)(n - 3)*7;
}

static void parse(deflator *d, iz end)
{
    if (!d->sampled) {
        sample(d);
    }

    iz p = d->pos;
    while (p<end && !d->blkready) {
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
            if (m.len <= d->lvl.insert) {
                ensure_insert(d, p + m.len - 1);
            } else {
                d->ins = MAX(d->ins, p + m.len);
            }
            p += m.len;
        } else {
            tok_lit(d, p++);
        }
    }
    d->pos = p;
}

// Rebase an entry, emptying it if its position falls out of the window
// or it was forgotten, either way at or below lim.
static u32 slide_entry(u32 v, u32 lim, u32 shift)
{
    return v>lim ? v-shift : 0;
}

// Discard window contents more than WSIZE behind the parse position.
static void slide(deflator *d)
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
    u32 lim = d->stamp + s;
    for (i32 i = 0; i < HASH_SIZE; i++) {
        d->head[i] = slide_entry(d->head[i], lim, s);
    }
    for (i32 i = 0; i < DEF_WSIZE; i++) {
        d->prev[i] = slide_entry(d->prev[i], lim, s);
    }
    if (d->use3) {
        for (i32 i = 0; i < HASH_SIZE; i++) {
            d->head3[i] = slide_entry(d->head3[i], lim, s);
        }
        for (i32 i = 0; i < DEF_WSIZE; i++) {
            d->prev3[i] = slide_entry(d->prev3[i], lim, s);
        }
    }
}

// Forget all history by emptying the hash chains. Emptying the heads is
// enough: chains then lead only to positions inserted afterward, whose
// links are written on insertion. Positions inserted since the heads
// were last emptied all lie below ins, still in the window (a slide
// empties the heads of positions it discards), so after a short history
// it is cheapest to rehash them and clear only their slots. Otherwise,
// rather than clear 512 KiB, advance the stamp added to new entries by
// more than any position: measured from a new entry, as find_match does,
// every older one then lies farther back than any match reaches, as good
// as empty. When the stamps run out, after 2,047 advances, the heads are
// cleared after all.
static void forget(deflator *d)
{
    iz n = MIN(d->ins, d->win_len - (HASH_LEN - 1));
    if (n <= FORGET_REHASH) {
        for (iz p = 0; p < n; p++) {
            u32 v = load32(d->win + p);
            d->head[hash4(v)] = 0;
            if (d->use3) {
                d->head3[hash3(v)] = 0;
            }
        }
    } else if (d->stamp < STAMP_LAST) {
        d->stamp += STAMP_STEP;
    } else {
        bytefill(d->head, 0, HASH_SIZE*(iz)sizeof(u32));
        bytefill(d->head3, 0, HASH_SIZE*(iz)sizeof(u32));
        d->stamp = 0;
    }
}

// Prepare to compress a new stream at the same level, unless changed
// with deflate_setlevel. This costs time in proportion to the previous
// stream, up to clearing the hash heads, and so is far cheaper than
// deflate_new for short streams.
static void deflate_reset(deflator *d)
{
    forget(d);
    d->olen       = d->ooff = 0;
    d->blkready   = d->flushing = d->finished = 0;
    d->win_len    = 0;
    d->base       = 0;
    d->pos        = d->ins = 0;
    d->sampled    = d->use3 = 0;
    d->blk_start  = 0;
    d->pend_start = 0;
    d->pend_len   = 0;
    d->bitbuf     = 0;
    d->bitcnt     = 0;
    clear_block(d);
}

// Levels 1 through 9; others are clamped. Only at the start of a stream.
static void deflate_setlevel(deflator *d, i32 level)
{
    d->lvl = deflate_levels[MAX(1, MIN(level, 9))];
}

// Memory needed by deflate_new, including alignment padding.
static iz deflate_memsize(void)
{
    return (iz)sizeof(deflator) + WIN_CAP + 2*HASH_SIZE*(iz)sizeof(u32) +
           2*DEF_WSIZE*(iz)sizeof(u32) + TOK_CAP*(iz)sizeof(token) +
           DEF_STAGE + HASH_SIZE/8 + 9*64;
}

// Tokens and chain links are always written before they are used (see
// forget), so of the large tables only the hash heads (and the sampling
// bitmap) start zeroed.
static deflator *deflate_new(arena *a, i32 level)
{
    deflator *d = new(a, 1, deflator);
    deflate_setlevel(d, level);
    d->obuf  = newbytes(a, DEF_STAGE);
    d->win   = newbytes(a, WIN_CAP);
    d->head  = new(a, HASH_SIZE, u32);
    d->prev  = alloc(a, DEF_WSIZE, sizeof(u32), _Alignof(u32), 0);
    d->head3 = new(a, HASH_SIZE, u32);
    d->seen  = new(a, HASH_SIZE/32, u32);
    d->prev3 = alloc(a, DEF_WSIZE, sizeof(u32), _Alignof(u32), 0);
    d->toks  = alloc(a, TOK_CAP, sizeof(token), _Alignof(token), 0);

    for (i32 i = 0; i < 288; i++) {
        d->fixlit.len[i] = (u8)(i<144 ? 8 : i<256 ? 9 : i<280 ? 7 : 8);
    }
    huff_codes(&d->fixlit, 288);
    for (i32 i = 0; i < NDIST; i++) {
        d->fixdist.len[i] = 5;
    }
    huff_codes(&d->fixdist, NDIST);

    for (i32 len = MIN_MATCH; len <= MAX_MATCH; len++) {
        d->lcode[len] = (u8)length_code(len);
    }
    for (i32 dist = 1; dist <= 256; dist++) {
        d->dcode[dist-1] = (u8)dist_code(dist);
    }
    for (i32 i = 2; i < 256; i++) {
        d->dcode[256+i] = (u8)dist_code((i<<7) + 1);
    }
    deflate_reset(d);
    return d;
}

// Ensure staging room for one emission step, compacting if needed.
static b32 def_room(deflator *d)
{
    if (DEF_STAGE-d->olen < DEF_STAGE_NEED && d->ooff) {
        bytemove(d->obuf, d->obuf+d->ooff, d->olen-d->ooff);
        d->olen -= d->ooff;
        d->ooff = 0;
    }
    return DEF_STAGE-d->olen >= DEF_STAGE_NEED;
}

// Staged output not yet handed out.
static s8 deflate_pending(deflator *d)
{
    s8 r = {d->obuf + d->ooff, d->olen - d->ooff};
    return r;
}

// Mark the first n bytes of pending output as handed out.
static void deflate_consume(deflator *d, iz n)
{
    assert(n>=0 && n<=d->olen-d->ooff);
    d->ooff += n;
    if (d->ooff == d->olen) {
        d->ooff = d->olen = 0;
    }
}

static void def_drain(deflator *d, zbuf *b)
{
    s8 p = deflate_pending(d);
    iz n = MIN(p.len, b->outlen);
    if (n) {
        bytecopy(b->out, p.s, n);
        b->out += n;
        b->outlen -= n;
        deflate_consume(d, n);
    }
}

// Emit everything parsed so far for the given flush.
static void def_flush(deflator *d, i32 flush)
{
    switch (flush) {
    case DEF_FINISH:
        flush_block(d, 1);
        bw_align(d);
        d->finished = 1;
        break;
    case DEF_SYNC:
    case DEF_FULL:
        flush_block(d, 0);
        flush_pending(d, 0);
        emit_stored_chunk(d, 0, 0, 0);
        if (flush == DEF_FULL) {
            forget(d);
            d->ins = d->pos;
        }
        break;
    }
}

// Compress from b->in into b->out, advancing both. With DEF_NONE,
// returns GZ_NEEDIN once all input is consumed, though output may remain
// staged. Otherwise returns GZ_OK once all input is consumed, the flush
// is complete, and all output is delivered. Returns GZ_NEEDOUT when the
// output buffer is full.
//
// Output may also be taken without copying through deflate_pending and
// deflate_consume, in which case b->out may be empty.
static i32 deflate_stream(deflator *d, zbuf *b, i32 flush)
{
    for (;;) {
        def_drain(d, b);
        if (d->flushing || d->finished) {
            if (d->ooff < d->olen) {
                return GZ_NEEDOUT;
            }
            d->flushing = 0;
            return GZ_OK;
        }

        if (d->blkready) {
            if (!def_room(d)) {
                return GZ_NEEDOUT;
            }
            flush_block(d, 0);
            d->blkready = 0;
        } else if (d->win_len == WIN_CAP) {
            parse(d, WIN_CAP - LOOKAHEAD);
            if (!d->blkready) {
                if (!def_room(d)) {
                    return GZ_NEEDOUT;
                }
                slide(d);
            }
        } else if (b->inlen) {
            iz n = MIN(b->inlen, WIN_CAP - d->win_len);
            bytecopy(d->win + d->win_len, b->in, n);
            d->win_len += n;
            b->in += n;
            b->inlen -= n;
        } else if (flush == DEF_NONE) {
            return GZ_NEEDIN;
        } else {
            parse(d, d->win_len);
            if (!d->blkready) {
                if (!def_room(d)) {
                    return GZ_NEEDOUT;
                }
                def_flush(d, flush);
                d->flushing = 1;
            }
        }
    }
}
