// tugz core: raw DEFLATE encoder (RFC 1951)
//
// Input accumulates in a sliding window and is parsed into tokens once
// the window fills (or at a flush), so output depends only on the input
// and flush points, not on how input and output buffers are split
// across calls. Hash chains store window-relative positions plus one
// (zero is "none") plus a stamp (see forget), rebased whenever the
// window slides, except that the levels that search deepest link their
// chains by distances (see chain_link), and the fastest levels keep a
// bucket of recent positions per hash instead of chains (see
// parse_bucket_with).
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
#define BLOCK_CAP     65532 // bucket levels' block size in bytes
#define NARROW_LEVEL  5     // levels from here on link by distances
#define STAMP_STEP    (1u << 21)  // stamps' unit; see forget
#define STAMP_LAST    (0u - STAMP_STEP)

_Static_assert(WIN_CAP + DEF_WSIZE <= STAMP_STEP,
               "a stamp step puts older entries beyond any match");
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
    i32 bucket;  // entries per bucket of the bucket finder, or 0 (chains)
} deflate_level;

// Chosen from a sweep over Silesia: each level beats zlib's compression
// ratio at the same level while running faster. Levels 1 and 2 find
// matches in buckets of one and two recent positions per hash (see
// parse_bucket_with), the rest in hash chains; the bucket levels use
// only nice and insert besides (nice, which stops a search, only with
// two entries).
static deflate_level const deflate_levels[10] = {
    // depth nice lazy good depth3 insert bucket
    {   0,    0,  0,   0,    0,   0, 0},
    {   0,    0,  0,   0,    0,  16, 1},
    {   0,   32,  0,   0,    0,  16, 2},
    {   8,   32,  0,   8,    8,  32, 0},
    {   8,   32,  1,   8,    8, 258, 0},
    {  16,   64,  1,  16,   16, 258, 0},
    {  32,   64,  1,  16,   32, 258, 0},
    {  64,  128,  2,  32,   32, 258, 0},
    { 256,  258,  2,  64,   64, 258, 0},
    {1024,  258,  2, 258,  256, 258, 0},
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
    i32 flushing; // flush due once a call's input is in, or DEF_NONE
    b32 flushed;  // flushing's output is staged, awaiting drain
    b32 finq;     // a FINISH is due after flushing and those queued
    u64 nqueued;  // SYNC/FULL flushes due after flushing, alternating

    u8 *win;
    iz  win_len;
    u64 base;     // stream offset of win[0]
    iz  pos;      // next position to parse
    iz  ins;      // next position to insert into hash chains
    b32 sampled;
    b32 use3;

    u32 *head;    // or the buckets (see parse_bucket_with)
    u32 *prev;
    u32 *head3;   // follows head
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
    u32 lenbits[MAX_MATCH+1];  // block's length codes, see emit_tokens
    b32 narrow;   // the level links chains by distances (see chain_link)
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
    u16 sa[288];
    u16 sb[288];
    u32 w[288];
    i32 m = 0;

    for (i32 i = 0; i < n; i++) {
        t->len[i] = 0;
        if (freq[i]) {
            sa[m++] = (u16)i;
        }
    }
    for (i32 i = 0; m < 2; i++) {
        if (!freq[i]) {
            sa[m++] = (u16)i;
        }
    }
    if (sa[0] > sa[1]) {
        u16 s = sa[0];  // a filler below the one used symbol
        sa[0] = sa[1];
        sa[1] = s;
    }

    // Sort by weight (unused fillers weigh 1), ties by symbol: symbols
    // start in order, and an LSD radix sort on the weight's bytes, being
    // stable, keeps ties so. O(n) rather than an insertion sort's O(n^2).
    #define HUFF_W(s) (freq[s] ? freq[s] : 1)
    u32 maxw = 0;
    for (i32 i = 0; i < m; i++) {
        maxw = MAX(maxw, HUFF_W(sa[i]));
    }
    u16 *syms = sa;
    u16 *tmp  = sb;
    for (i32 shift = 0; shift<32 && maxw>>shift; shift += 8) {
        u16 cnt[256] = {0};
        for (i32 i = 0; i < m; i++) {
            cnt[HUFF_W(syms[i])>>shift & 255]++;
        }
        u16 sum = 0;
        for (i32 i = 0; i < 256; i++) {
            u16 c = cnt[i];
            cnt[i] = sum;
            sum = (u16)(sum + c);
        }
        for (i32 i = 0; i < m; i++) {
            tmp[cnt[HUFF_W(syms[i])>>shift & 255]++] = syms[i];
        }
        u16 *x = syms;
        syms = tmp;
        tmp = x;
    }
    for (i32 i = 0; i < m; i++) {
        w[i] = HUFF_W(syms[i]);
    }
    #undef HUFF_W

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
// 8 bytes at a time once 32 or more bits accumulate. Always inlined:
// GCC would otherwise call it for every code it writes (emit_tokens,
// which writes most of them, keeps this state in locals instead).
[[gnu::always_inline]] static inline void bw_put(deflator *d, u64 v, i32 n)
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

// Emit the block's tokens. The bit buffer, its count and the output
// position live in locals for the whole loop (stores to the output would
// otherwise force reloads of the deflator's fields), and whole bytes
// spill once per match or per up to three literals. After a spill at
// most 7 bits remain, and a match adds at most 15+5+15+13 = 48 bits and
// three literals at most 45, so the 64-bit buffer never overflows. Each
// spill stores 8 bytes but advances only by the whole bytes, which stay
// within the TOK_CAP*6 bytes of DEF_STAGE_NEED (plus those bw_put held);
// DEF_STAGE's slack beyond that absorbs the store's overhang.
static void emit_tokens(deflator *d, htree *lt, htree *dt)
{
    u64    bb   = d->bitbuf;
    i32    bc   = d->bitcnt;
    u8    *out  = d->obuf + d->olen;
    token *toks = d->toks;
    iz     ntok = d->ntok;
    assert(DEF_STAGE-d->olen >= ntok*6 + 16);

    // Each length's code and extra bits under this block's code (at most
    // 15+5 bits) above the low 8 bits, which hold their count
    for (i32 len = MIN_MATCH; len <= MAX_MATCH; len++) {
        i32 lc = lcode_of(d, len);
        i32 ln = lt->len[257+lc];
        u32 lx = (u32)(len - def_len_base[lc]);
        u32 v  = lt->code[257+lc] | lx<<ln;
        d->lenbits[len] = v<<8 | (u32)(ln + def_len_extra[lc]);
    }

    // Spill first, as bw_put may have left up to 31 bits
    store64le(out, bb);
    out += bc >> 3;
    bb >>= bc & ~7;
    bc &= 7;

    #define EMIT_LIT(c) do { \
        bb |= (u64)lt->code[c] << bc; \
        bc += lt->len[c]; \
    } while (0)

    for (iz i = 0; i < ntok;) {
        token t = toks[i++];
        if (!t.dist) {
            EMIT_LIT(t.litlen);
            if (i<ntok && !toks[i].dist) {
                EMIT_LIT(toks[i].litlen);
                i++;
                if (i<ntok && !toks[i].dist) {
                    EMIT_LIT(toks[i].litlen);
                    i++;
                }
            }
        } else {
            // The length's bits from the table, then the distance's code
            // plus extra bits: at most 15+13 bits
            u32 le = d->lenbits[t.litlen];
            i32 dc = dcode_of(d, t.dist);
            u64 dx = (u32)(t.dist - def_dist_base[dc]);
            i32 dn = dt->len[dc];
            bb |= (u64)(le >> 8) << bc;
            bc += (i32)(le & 0xff);
            bb |= ((u64)dt->code[dc] | dx<<dn) << bc;
            bc += dn + def_dist_extra[dc];
        }
        assert(bc <= 7+48);
        store64le(out, bb);
        out += bc >> 3;
        bb >>= bc & ~7;
        bc &= 7;
    }
    #undef EMIT_LIT

    d->bitbuf = bb;
    d->bitcnt = bc;
    d->olen   = out - d->obuf;
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
    // Always true, as a block ends before a slide would discard its data
    // (see slide_ends_block), but checked, as storing reads the window.
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

// The token functions run once per token in parse. Always inlined (with
// should_split left out of line): GCC would otherwise call tok_end for
// every token, and tok_lit too once more of parse is inlined.
[[gnu::always_inline]] static inline void tok_end(deflator *d)
{
    if (!d->blkready && (d->ntok>=TOK_LIMIT ||
                         (++d->nnewobs==OBS_BATCH && should_split(d)))) {
        d->blkready = 1;
    }
}

[[gnu::always_inline]] static inline void tok_lit(deflator *d, iz p)
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

[[gnu::always_inline]]
static inline void tok_match(deflator *d, iz p, i32 len, i32 dist)
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

// The match finder's state, which parse keeps in a local: stores to the
// hash tables and to the block's statistics, all u32 arrays, might alias
// the deflator's fields, which GCC would then reload after every
// insertion and token. (See also parse_with.)
typedef struct {
    u8  *win;
    u32 *head;
    u32 *prev;
    u32 *head3;
    u32 *prev3;
    iz   win_len;
    iz   lim;    // positions below have HASH_LEN bytes to hash
    iz   ins;    // next position to insert into hash chains
    u32  h;      // hashes of ins, when below lim, computed ahead
    u32  h3;
    u32  base;   // low bits of the stream offset of win[0]
    u32  stamp;
    b32  use3;
    deflate_level lvl;
    b32  narrow;
} finder;

// Index into prev tables for window position p.
static u32 chain_slot(finder *f, iz p)
{
    return (f->base + (u32)p) & DEF_WMASK;
}

// Index into prev tables for a hash table entry's position. Stamps are
// multiples of the window size, so they drop out.
static u32 entry_slot(finder *f, u32 entry)
{
    return (f->base + entry - 1) & DEF_WMASK;
}

// A chain link as the levels from NARROW_LEVEL on store it, in the first
// half of the link tables viewed as u16: the distance from a new entry
// back to the one it displaces from its head, saturated at LINK_NONE,
// beyond any match (an empty head, zero, gives one back past position
// 0). Their long walks of chains gain more from the halved footprint in
// the cache than the saturation costs each insertion, which the faster
// levels would not. Such links need no rebasing as the window slides.
#define LINK_NONE 0xffff
static u16 chain_link(u32 entry, u32 old)
{
    return (u16)MIN(entry - old, LINK_NONE);
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

// Hash position x ahead of its insertion, prefetching its hash table
// entries, which the insertion and the search after it would otherwise
// wait for.
[[gnu::always_inline]] static inline void hash_ahead(finder *f, iz x)
{
    if (x < f->lim) {
        u32 v = load32(f->win + x);
        f->h = hash4(v);
        __builtin_prefetch(f->head + f->h);
        if (f->use3) {
            f->h3 = hash3(v);
            __builtin_prefetch(f->head3 + f->h3);
        }
    }
}

typedef struct {
    u32 c4;  // the chains' previous heads
    u32 c3;  // (zero when not using 3-byte chains)
} heads;

// Insert ins, which must be below lim, and hash the position after it.
[[gnu::always_inline]] static inline heads hash_insert(finder *f)
{
    iz  x  = f->ins++;
    u32 h  = f->h;
    u32 h3 = f->h3;
    hash_ahead(f, x+1);
    u32 slot  = chain_slot(f, x);
    u32 entry = (u32)x + 1 + f->stamp;
    heads r = {f->head[h], 0};
    if (f->narrow) {
        ((u16 *)f->prev)[slot] = chain_link(entry, r.c4);
    } else {
        f->prev[slot] = r.c4;
    }
    f->head[h] = entry;
    if (f->use3) {
        r.c3 = f->head3[h3];
        if (f->narrow) {
            ((u16 *)f->prev3)[slot] = chain_link(entry, r.c3);
        } else {
            f->prev3[slot] = r.c3;
        }
        f->head3[h3] = entry;
    }
    return r;
}

// Insert all positions through q. Always inlined, like hash_insert and
// match_at: GCC would otherwise call this and match_at from parse, and
// hash_insert once they inline.
[[gnu::always_inline]] static inline void ensure_insert(finder *f, iz q)
{
    while (f->ins<=q && f->ins<f->lim) {
        hash_insert(f);
    }
    f->ins = MAX(f->ins, q + 1);
}

[[gnu::always_inline]]
static inline i32 match_len(u8 const *a, u8 const *b, i32 max)
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

// find_match for distance links. Candidates are tracked by the low bits
// of their stream offsets (cb), as is p (pb), from which their chain
// slots follow in one step, as from entries. Candidates lie at most a
// window back, and at or after position 0, which the first from an
// empty head does not. A candidate exactly WSIZE back shares p's chain
// slot, where inserting p linked this walk's first candidate: its link
// leads more than WSIZE back, ending the walk before it would revisit
// candidates (find_match makes it the last). Otherwise as find_match.
[[gnu::always_inline]] static inline match
find_match16(finder *f, iz p, u32 cand, u32 cand3, i32 depth, i32 best)
{
    u8  *win  = f->win;
    u16 *prev = (u16 *)f->prev;
    i32 maxlen = (i32)MIN(f->win_len - p, MAX_MATCH);
    assert(maxlen >= MIN_MATCH);  // p+MIN_MATCH <= win_len
    i32 least = best;
    iz  best_pos = 0;
    u32 pb   = f->base + (u32)p;
    u32 kb   = f->base - 1 - f->stamp;  // an entry's offset, less it
    u32 cb   = cand + kb;
    u32 maxd = (u32)MIN(p, DEF_WSIZE);
    if (best >= maxlen) {
        cb = pb - LINK_NONE;  // nothing longer fits
    }
    u32 p4 = pb-cb<=maxd ? load32(win+p) : 0;  // p+HASH_LEN <= win_len

    for (; best<MIN_MATCH && pb-cb<=maxd && depth>0; depth--) {
        iz c = p - (iz)(pb - cb);
        cb -= prev[cb & DEF_WMASK];
        u32 x = load32(win+c) ^ p4;
        if (!(x & 0xffffff)) {
            best = x ? 3 : 4 + match_len(win+c+4, win+p+4, maxlen-4);
            best_pos = c;
            if (best>=f->lvl.nice || best==maxlen) {
                cb = pb - LINK_NONE;
                break;
            }
            if (best>=f->lvl.good && depth>8) {
                depth = 8;
            }
        }
    }

    for (; pb-cb<=maxd && depth>0; depth--) {
        iz c = p - (iz)(pb - cb);
        cb -= prev[cb & DEF_WMASK];
        if (load32(win+c+best-3)==load32(win+p+best-3) &&
            load32(win+c)==p4) {
            i32 len = 4 + match_len(win+c+4, win+p+4, maxlen-4);
            if (len > best) {
                best = len;
                best_pos = c;
                if (best>=f->lvl.nice || best==maxlen) {
                    break;
                }
                if (best>=f->lvl.good && depth>8) {
                    depth = 8;
                }
            }
        }
    }

    u16 *prev3  = (u16 *)f->prev3;
    i32  depth3 = best<MIN_MATCH ? f->lvl.depth3 : 0;
    u32  cb3    = cand3 + kb;
    for (; pb-cb3<=MIN(maxd, D3_MAX) && depth3>0; depth3--) {
        iz c = p - (iz)(pb - cb3);
        if (win[c]==win[p] && win[c+1]==win[p+1] && win[c+2]==win[p+2]) {
            best = MIN_MATCH;
            best_pos = c;
            break;
        }
        cb3 -= prev3[cb3 & DEF_WMASK];
    }

    match r = {0, 0};
    if (best > least) {
        r.len  = best;
        r.dist = (i32)(p - best_pos);
    }
    return r;
}

// Candidates are hash table entries, from which distances are measured
// against p's own entry: a forgotten entry is farther than any match.
// A candidate counts only if longer than the best match so far, so once
// there is one it must share p's first 4 bytes and the 4 ending at the
// best length: two 32-bit compares rule out most candidates before
// match_len, which then starts at the fifth byte. Until a first match,
// a candidate (whose 4-byte hash may merely collide) needs 3 bytes.
//
// Only a match longer than best counts, else the result is empty, so
// lazy evaluation searches from the length it has, with the stricter
// filters throughout. It walks the same candidates as from MIN_MATCH-1
// and finds the same match: a match no longer than best would end the
// walk early only at nice, or cap its depth only at good, and good <=
// nice, while lazy evaluation starts only below good; and the 3-byte
// fallback cannot find a longer match than best >= MIN_MATCH.
//
// Always inlined, with match_len: GCC (and clang) would otherwise call
// it for every position searched.
[[gnu::always_inline]] static inline match
find_match(finder *f, iz p, u32 cand, u32 cand3, i32 depth, i32 best)
{
    if (f->narrow) {
        return find_match16(f, p, cand, cand3, depth, best);
    }
    u8 *win = f->win;
    i32 maxlen = (i32)MIN(f->win_len - p, MAX_MATCH);
    assert(maxlen >= MIN_MATCH);  // p+MIN_MATCH <= win_len
    i32 least = best;
    iz  best_pos = 0;
    u32 here = (u32)p + 1 + f->stamp;
    if (best >= maxlen) {
        cand = 0;  // nothing longer fits
    }
    u32 p4 = cand ? load32(win+p) : 0;  // p+HASH_LEN <= win_len

    // Until a first match (see the next loop's comments)
    for (; best<MIN_MATCH && cand && depth>0; depth--) {
        u32 dist = here - cand;
        if (dist > DEF_WSIZE) {
            cand = 0;
            break;
        }
        iz c = p - (iz)dist;
        if (dist == DEF_WSIZE) {
            depth = 1;
        }
        cand = f->prev[entry_slot(f, cand)];
        u32 x = load32(win+c) ^ p4;
        if (!(x & 0xffffff)) {
            best = x ? 3 : 4 + match_len(win+c+4, win+p+4, maxlen-4);
            best_pos = c;
            if (best>=f->lvl.nice || best==maxlen) {
                cand = 0;
                break;
            }
            if (best>=f->lvl.good && depth>8) {
                depth = 8;
            }
        }
    }

    // Once best >= MIN_MATCH
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
        cand = f->prev[entry_slot(f, cand)];
        if (load32(win+c+best-3)==load32(win+p+best-3) &&
            load32(win+c)==p4) {
            i32 len = 4 + match_len(win+c+4, win+p+4, maxlen-4);
            if (len > best) {
                best = len;
                best_pos = c;
                if (best>=f->lvl.nice || best==maxlen) {
                    break;
                }
                if (best>=f->lvl.good && depth>8) {
                    depth = 8;
                }
            }
        }
    }

    // Fall back to short, close 3-byte matches
    i32 depth3 = best<MIN_MATCH ? f->lvl.depth3 : 0;
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
        cand3 = f->prev3[entry_slot(f, cand3)];
    }

    match r = {0, 0};
    if (best > least) {
        r.len  = best;
        r.dist = (i32)(p - best_pos);
    }
    return r;
}

// Find the best match at p longer than best, inserting p, the next
// position to insert, into the hash chains.
[[gnu::always_inline]]
static inline match match_at(finder *f, iz p, i32 depth, i32 best)
{
    assert(p == f->ins);
    if (p < f->lim) {  // p+HASH_LEN <= win_len
        heads c = hash_insert(f);
        return find_match(f, p, c.c4, c.c3, depth, best);
    }
    f->ins = p + 1;
    if (f->use3 && p+MIN_MATCH <= f->win_len) {
        u32 v = (u32)f->win[p] | (u32)f->win[p+1]<<8 | (u32)f->win[p+2]<<16;
        return find_match(f, p, 0, f->head3[hash3(v)], depth, best);
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

// Parse with use3 and lazy (whether the level evaluates lazily) as
// constants: each combination has an instance (below), which carries
// only the state and code it uses (the greedy levels drop lazy
// evaluation), so that GCC can keep more of it in x86-64's few
// registers.
[[gnu::always_inline]]
static inline void
parse_with(deflator *d, iz end, b32 use3, b32 lazy, b32 narrow)
{
    finder f = {
        .win     = d->win,
        .head    = d->head,
        .prev    = d->prev,
        .head3   = d->head3,
        .prev3   = d->prev3,
        .win_len = d->win_len,
        .lim     = d->win_len - (HASH_LEN - 1),
        .ins     = d->ins,
        .base    = (u32)d->base,
        .stamp   = d->stamp,
        .use3    = use3,
        .lvl     = d->lvl,
        .narrow  = narrow,
    };
    if (!lazy) {
        f.lvl.lazy = 0;
    }
    hash_ahead(&f, f.ins);

    iz p = d->pos;
    while (p<end && !d->blkready) {
        match m = match_at(&f, p, f.lvl.depth, MIN_MATCH-1);

        // Lazy matching: prefer a longer match starting one byte later
        for (i32 step = 0; step < f.lvl.lazy; step++) {
            if (m.len<MIN_MATCH || m.len>=f.lvl.good || p+1>=f.win_len) {
                break;
            }
            match m2 = match_at(&f, p+1, f.lvl.depth>>1, m.len);
            if (m2.len <= m.len) {
                break;
            }
            tok_lit(d, p++);
            m = m2;
        }

        if (m.len >= MIN_MATCH) {
            tok_match(d, p, m.len, m.dist);
            if (m.len <= f.lvl.insert) {
                ensure_insert(&f, p + m.len - 1);
            } else {
                f.ins = MAX(f.ins, p + m.len);
                hash_ahead(&f, f.ins);
            }
            p += m.len;
        } else {
            tok_lit(d, p++);
        }
    }
    d->pos = p;
    d->ins = f.ins;
}

// The instances, each a function of its own, which GCC compiles, and
// allocates registers for, alone
[[gnu::noinline]] static void parse_greedy(deflator *d, iz end)
{
    parse_with(d, end, 0, 0, 0);
}

[[gnu::noinline]] static void parse_greedy3(deflator *d, iz end)
{
    parse_with(d, end, 1, 0, 0);
}

[[gnu::noinline]] static void parse_lazy(deflator *d, iz end)
{
    parse_with(d, end, 0, 1, 0);
}

[[gnu::noinline]] static void parse_lazy3(deflator *d, iz end)
{
    parse_with(d, end, 1, 1, 0);
}

[[gnu::noinline]] static void parse_deep(deflator *d, iz end)
{
    parse_with(d, end, 0, 1, 1);
}

[[gnu::noinline]] static void parse_deep3(deflator *d, iz end)
{
    parse_with(d, end, 1, 1, 1);
}

// The bucket finder of levels 1 and 2, after libdeflate's
// ht_matchfinder: each 4-byte hash keeps its nb (1 or 2) most recent
// positions, newest first, in a bucket of nb entries, so a search is at
// most nb compares and an insertion a store or two, with no chain links.
// The table, HASH_SIZE buckets, takes head, and with two entries head3
// too, which follows it in memory; the chain levels' other tables lie
// idle. Entries are positions plus one plus the stamp, as the heads'
// (see forget), so an empty or forgotten entry lies farther back than
// any match. Matches are 4 bytes or longer, found greedily, and the
// positions inside a match are inserted up to the level's insert
// length. Blocks are not split by statistics, which cost these levels
// more time than their ratio was worth: a block ends once it holds
// BLOCK_CAP bytes of input (or before a slide, or at a flush), so it
// holds at most BLOCK_CAP tokens and, but for the last, more than
// MIN_BLOCK bytes, as deflate_bound requires.
_Static_assert(BLOCK_CAP<=TOK_LIMIT && BLOCK_CAP>=MIN_BLOCK, "block cap");

// The bucket finder's state, in a local as the chains' finder is, with
// the block's token pointer and statistics.
typedef struct {
    u8    *win;
    u32   *t;
    token *tok;    // next token
    u32   *lit_freq;
    u32   *dist_freq;
    deflator *d;
    iz     lim;    // positions below have HASH_LEN bytes to hash
    u32    stamp1; // stamp plus one
    u32    h;      // hash of the position in hand, when below lim
    i32    nice;
    i32    ins;
} bfinder;

#define BKT(f, h) ((f)->t + (iz)(h)*nb)

// Search and insert position p, below lim, and emit its token. With
// last, p+1 may be lim (else below), and has no hash. Returns the next
// position, whose hash is then in f->h when below lim. Its bucket, and
// those of positions inserted inside a match, are prefetched a step
// ahead. A candidate is used only if 1 to MIN(p, WSIZE) back, which
// rules out empty and forgotten entries (and a stale one at p itself).
// Always inlined, with match_len, into an instance per entry count.
[[gnu::always_inline]]
static inline iz bkt_step(bfinder *f, iz p, i32 nb, b32 last)
{
    u8  *win  = f->win;
    u32 *b    = BKT(f, f->h);
    u32  c0   = b[0];
    u32  c1   = nb==2 ? b[1] : 0;
    u32  here = (u32)p + f->stamp1;
    if (nb == 2) {
        b[1] = c0;
    }
    b[0] = here;

    u32 hn = 0;
    if (!last || p+1<f->lim) {
        hn = hash4(load32(win+p+1));
        __builtin_prefetch(BKT(f, hn));
    }

    u32 maxd  = (u32)MIN(p, DEF_WSIZE);
    u32 p4    = load32(win+p);
    i32 best  = 0;
    u32 bestd = 0;
    u32 dist  = here - c0;
    i32 maxlen = 0;
    if (dist-1 < maxd && load32(win+p-dist)==p4) {
        maxlen = (i32)MIN(f->d->win_len - p, MAX_MATCH);
        best   = 4 + match_len(win+p-dist+4, win+p+4, maxlen-4);
        bestd  = dist;
    }
    if (nb==2 && best<f->nice) {
        dist = here - c1;
        if (dist-1 < maxd) {
            u8 const *c = win + p - dist;
            if (!best) {
                if (load32(c) == p4) {
                    maxlen = (i32)MIN(f->d->win_len - p, MAX_MATCH);
                    best   = 4 + match_len(c+4, win+p+4, maxlen-4);
                    bestd  = dist;
                }
            } else if (best<maxlen &&
                       load32(c+best-3)==load32(win+p+best-3) &&
                       load32(c)==p4) {
                i32 len = 4 + match_len(c+4, win+p+4, maxlen-4);
                if (len > best) {
                    best  = len;
                    bestd = dist;
                }
            }
        }
    }

    deflator *d = f->d;
    if (!best) {
        *f->tok++ = (token){(u8)p4, 0};
        f->lit_freq[(u8)p4]++;
        f->h = hn;
        return p + 1;
    }

    *f->tok++ = (token){(u16)best, (u16)bestd};
    f->lit_freq[257+lcode_of(d, best)]++;
    f->dist_freq[dcode_of(d, (i32)bestd)]++;
    iz q = p + best;
    u32 hq = 0;
    if (q < f->lim) {
        hq = hash4(load32(win+q));
        __builtin_prefetch(BKT(f, hq));
    }
    if (best <= f->ins) {
        // Insert the match's later positions, hashing ahead
        iz xe = MIN(q, f->lim);
        u32 hx = hn;
        for (iz x = p+1; x < xe; x++) {
            u32 hy = hq;
            if (x+1 < xe) {
                hy = hash4(load32(win+x+1));
                __builtin_prefetch(BKT(f, hy));
            }
            u32 *bx = BKT(f, hx);
            if (nb == 2) {
                bx[1] = bx[0];
            }
            bx[0] = (u32)x + f->stamp1;
            hx = hy;
        }
    }
    f->h = hq;
    return q;
}

[[gnu::always_inline]]
static inline void parse_bucket_with(deflator *d, iz end, i32 nb)
{
    iz p = d->pos;
    assert(d->ins == p);
    assert(d->blk_len < BLOCK_CAP);
    bfinder f = {
        .win       = d->win,
        .t         = d->head,
        .tok       = d->toks + d->ntok,
        .lit_freq  = d->lit_freq,
        .dist_freq = d->dist_freq,
        .d         = d,
        .lim       = d->win_len - (HASH_LEN - 1),
        .stamp1    = d->stamp + 1,
        .nice      = d->lvl.nice,
        .ins       = d->lvl.insert,
    };
    if (!d->ntok) {
        d->blk_start = d->base + (u64)p;
    }
    // Blocks grow by the input parsed, so the cap is a position. The
    // main loop needs no bounds but its own; positions too near the end
    // to hash the next (or themselves) follow.
    iz p0  = p;
    iz cap = p + (iz)(BLOCK_CAP - d->blk_len);
    end = MIN(end, cap);
    iz stop = MIN(end, f.lim-1);  // p+1 has a hash too
    if (p < f.lim) {
        f.h = hash4(load32(f.win+p));
    }
    while (p < stop) {
        p = bkt_step(&f, p, nb, 0);
    }
    while (p < end) {
        if (p < f.lim) {
            p = bkt_step(&f, p, nb, 1);
        } else {
            *f.tok++ = (token){f.win[p], 0};
            f.lit_freq[f.win[p]]++;
            p++;
        }
    }
    d->ntok     = f.tok - d->toks;
    d->blk_len += (u64)(p - p0);
    d->blkready = d->blk_len >= BLOCK_CAP;
    d->pos = p;
    d->ins = p;
}
#undef BKT

[[gnu::noinline]] static void parse_bucket1(deflator *d, iz end)
{
    parse_bucket_with(d, end, 1);
}

[[gnu::noinline]] static void parse_bucket2(deflator *d, iz end)
{
    parse_bucket_with(d, end, 2);
}

static void parse(deflator *d, iz end)
{
    if (d->lvl.bucket) {
        d->sampled = 1;
        d->lvl.bucket==2 ? parse_bucket2(d, end) : parse_bucket1(d, end);
        return;
    }
    if (!d->sampled) {
        sample(d);
    }
    if (d->narrow) {
        d->use3 ? parse_deep3(d, end) : parse_deep(d, end);
    } else if (d->lvl.lazy) {
        d->use3 ? parse_lazy3(d, end) : parse_lazy(d, end);
    } else {
        d->use3 ? parse_greedy3(d, end) : parse_greedy(d, end);
    }
}

// Rebase a table's entries, emptying those whose positions fall out of
// the window or were forgotten, either way at or below lim. Chain links
// that no insertion wrote get rebased too, harmlessly, since no chain
// leads to them, but without a branch on their unknown values, which
// MemorySanitizer would report.
static void slide_table(u32 *t, i32 n, u32 lim, u32 shift)
{
    for (i32 i = 0; i < n; i++) {
        u32 v = t[i];
        t[i] = (v - shift) & -(u32)(v > lim);
    }
}

// The same for a link table, through memcpy, as an earlier stream may
// have left distance links there. (Distance links need nothing.)
static void slide_links(u32 *t, u32 lim, u32 shift)
{
    u8 *b = (u8 *)t;
    for (iz i = 0; i < DEF_WSIZE*(iz)sizeof(u32); i += 4) {
        u32 v;
        __builtin_memcpy(&v, b+i, 4);
        v = (v - shift) & -(u32)(v > lim);
        __builtin_memcpy(b+i, &v, 4);
    }
}

// Whether the next slide would discard data of the block in progress,
// which must then end first, so that every block may be stored. Else a
// block of incompressible data that straddled a slide was coded with
// Huffman codes at a little over 8 bits per byte (on random data, 55
// bytes more than stored, for every other slide), and no simple bound
// on output held. This ends at most one block per slide (about 1 MiB of
// input), and that block is over 32 KiB long.
static b32 slide_ends_block(deflator *d)
{
    iz shift = d->pos - DEF_WSIZE;
    return d->ntok && shift>0 && d->blk_start<d->base+(u64)shift;
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
    if (d->lvl.bucket) {  // one or two entries a bucket, from head on
        slide_table(d->head, HASH_SIZE*d->lvl.bucket, lim, s);
        return;
    }
    slide_table(d->head, HASH_SIZE, lim, s);
    if (!d->narrow) {
        slide_links(d->prev, lim, s);
    }
    if (d->use3) {
        slide_table(d->head3, HASH_SIZE, lim, s);
        if (!d->narrow) {
            slide_links(d->prev3, lim, s);
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
// more than any position plus a window: measured from a new entry, as
// find_match does, every older one then lies farther back than any match
// reaches, as good as empty, even once a reset restarts positions at
// zero. (A slide also empties older entries, all being at or below its
// limit.) When the stamps run out, after 2,047 advances, the heads are
// cleared after all. Rehashing clears a position's slots in every layout
// (the heads, the 3-byte heads, and buckets of one and two entries,
// which span both), whatever the level that wrote them and whether it
// sampled for 3-byte matching: a reset may change the level, before or
// after forgetting, and so the layout.
static void forget(deflator *d)
{
    iz n = MIN(d->ins, d->win_len - (HASH_LEN - 1));
    if (n <= FORGET_REHASH) {
        for (iz p = 0; p < n; p++) {
            u32 v = load32(d->win + p);
            u32 h = hash4(v);
            d->head[h] = 0;
            d->head3[hash3(v)] = 0;
            d->head[2*h] = d->head[2*h+1] = 0;  // two-entry buckets
        }
    } else if (d->stamp < STAMP_LAST) {
        d->stamp += STAMP_STEP;
    } else {
        bytefill(d->head, 0, 2*HASH_SIZE*(iz)sizeof(u32));  // and head3
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
    d->blkready   = d->flushed = 0;
    d->flushing   = DEF_NONE;
    d->finq       = 0;
    d->nqueued    = 0;
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
    level = MAX(1, MIN(level, 9));
    d->lvl = deflate_levels[level];
    d->narrow = level >= NARROW_LEVEL;
}

// Most bits a stored chunk's header takes: BFINAL and BTYPE, padding to
// a byte boundary, then LEN and NLEN. Padding is 5 bits from a boundary,
// and 7 when 6 or 7 bits were pending.
#define DEF_CHUNK_BITS  (3 + 7 + 32)
#define DEF_SLIDE_MIN   (WIN_CAP - LOOKAHEAD - DEF_WSIZE)  // least shift

// Most bytes of raw output for len bytes of input in a stream with no
// flush but FINISH. Output is 8 bits per input byte, plus at most
// DEF_CHUNK_BITS per "header", plus at most 7 bits of final padding:
// - A stored chunk is its header and its data. Full chunks (65,535
//   bytes) number at most len/65535. Partial ones come only when held
//   back data is flushed: before a Huffman block, at a slide, and at the
//   end (or else an empty final block of 10 bits).
// - A Huffman block is chosen only when its cost is no more than storing
//   its L bytes alone: one header per 65,535 bytes or part, so at most
//   L/65535 + 1 headers and 8L bits. This holds because a block always
//   may be stored (see slide_ends_block).
// - A block ends early, by its statistics (should_split), only once it
//   holds at least MIN_BLOCK bytes, and otherwise at TOK_LIMIT tokens,
//   which is longer, or before a slide, by then over 32 KiB. Of blocks
//   B, all but the last are at least MIN_BLOCK, so B <= len/MIN_BLOCK+1.
// - Slides discard at least DEF_SLIDE_MIN bytes each.
// So headers number at most len/65535 + 2B + len/DEF_SLIDE_MIN + 1.
// Each SYNC or FULL flush adds at most DEF_FLUSH_BOUND bytes: a block it
// ends (at most two headers if Huffman, its own and the held-back data
// before it, else one, flushing its own held-back data), and its empty
// stored block (one more).
#define DEF_FLUSH_BOUND  ((3*DEF_CHUNK_BITS + 7) / 8)
[[maybe_unused]] static u64 deflate_bound(u64 len)
{
    u64 blocks  = len/MIN_BLOCK + 1;
    u64 headers = len/65535 + 2*blocks + len/DEF_SLIDE_MIN + 1;
    return len + (headers*DEF_CHUNK_BITS + 7 + 7)/8;
}

// Memory needed by deflate_new, including alignment padding.
static iz deflate_memsize(void)
{
    return (iz)sizeof(deflator) + WIN_CAP + 2*HASH_SIZE*(iz)sizeof(u32) +
           2*DEF_WSIZE*(iz)sizeof(u32) + TOK_CAP*(iz)sizeof(token) +
           DEF_STAGE + HASH_SIZE/8 + 9*64;
}

// Tokens are written before they are read, and chains lead only to links
// that insertions wrote (see forget and slide_table), so of the large
// tables only the hash heads (and the sampling bitmap) start zeroed. The
// 3-byte heads follow the heads, and two-entry buckets span both.
static deflator *deflate_new(arena *a, i32 level)
{
    deflator *d = new(a, 1, deflator);
    deflate_setlevel(d, level);
    d->obuf  = newbytes(a, DEF_STAGE);
    d->win   = newbytes(a, WIN_CAP);
    d->head  = alloc(a, 2*HASH_SIZE, sizeof(u32), 64, 1);  // and head3
    d->head3 = d->head + HASH_SIZE;
    d->prev  = alloc(a, DEF_WSIZE, sizeof(u32), _Alignof(u32), 0);
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

// Ensure staging room for one emission step, compacting only if that
// makes the room: otherwise a caller taking a little output per call
// would pay on each call to move all that remains.
static b32 def_room(deflator *d)
{
    iz room = DEF_STAGE - d->olen;
    if (room<DEF_STAGE_NEED && room+d->ooff>=DEF_STAGE_NEED) {
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

// The mode of the latest flush due, or DEF_NONE if none is.
static i32 def_lastdue(deflator *d)
{
    return d->finq      ? DEF_FINISH :
           d->nqueued&1 ? DEF_SYNC + DEF_FULL - d->flushing :
           d->flushing;
}

// Compress from b->in into b->out, advancing both. With DEF_NONE,
// returns GZ_NEEDIN once all input is consumed, though output may remain
// staged. Otherwise returns GZ_OK once all input is consumed, the flush
// is complete, and all output is delivered. In any mode, returns
// GZ_NEEDOUT, perhaps with input left, when the output buffer is full
// and either staging lacks room for the next emission step or a flush's
// output remains.
//
// A flush falls due once its call has consumed all input, and later
// calls complete it before anything else. While flushes are unfinished,
// a call never consumes input, and one with no input in the mode of the
// latest due flush only completes them (returning GZ_OK once all are
// complete): it is a repeat. A no-input SYNC, FULL, or FINISH in another
// mode falls due at once, queued behind the others, so the stream is
// the one a caller waiting for each flush to complete would get. Any
// other call completes the unfinished flushes, then goes on with its own
// input and mode. After DEF_FINISH falls due, queued or not, only
// repeats are valid.
//
// Repeats make adjacent queued flushes differ in mode, so the queue is
// a run of SYNC and FULL alternating from flushing, perhaps then FINISH:
// a count and a flag hold it exactly.
//
// Output may also be taken without copying through deflate_pending and
// deflate_consume, in which case b->out may be empty.
static i32 deflate_stream(deflator *d, zbuf *b, i32 flush)
{
    if ((d->flushing==DEF_FINISH || d->finq) &&
        (b->inlen || flush!=DEF_FINISH)) {
        return GZ_EUSAGE;
    }

    if (d->flushing && flush && !b->inlen && flush!=def_lastdue(d)) {
        if (flush == DEF_FINISH) {
            d->finq = 1;
        } else {
            d->nqueued++;
        }
    }

    for (;;) {
        def_drain(d, b);
        if (d->flushed) {
            if (d->ooff < d->olen) {
                return GZ_NEEDOUT;
            } else if (d->flushing == DEF_FINISH) {
                return GZ_OK;
            }
            d->flushed = 0;
            if (d->nqueued) {
                d->nqueued--;
                d->flushing = DEF_SYNC + DEF_FULL - d->flushing;
                continue;
            } else if (d->finq) {
                d->finq = 0;
                d->flushing = DEF_FINISH;
                continue;
            }
            b32 repeat = flush==d->flushing && !b->inlen;
            d->flushing = DEF_NONE;
            if (repeat) {
                return GZ_OK;
            }
        }
        if (!d->flushing && !b->inlen) {
            d->flushing = flush;
        }

        if (d->blkready) {
            if (!def_room(d)) {
                return GZ_NEEDOUT;
            }
            flush_block(d, 0);
            d->blkready = 0;
        } else if (d->win_len == WIN_CAP) {
            parse(d, WIN_CAP - LOOKAHEAD);
            if (!d->blkready && slide_ends_block(d)) {
                d->blkready = 1;  // emitted on the next pass, then slide
            } else if (!d->blkready) {
                if (!def_room(d)) {
                    return GZ_NEEDOUT;
                }
                slide(d);
            }
        } else if (d->flushing) {
            parse(d, d->win_len);
            if (!d->blkready) {
                if (!def_room(d)) {
                    return GZ_NEEDOUT;
                }
                def_flush(d, d->flushing);
                d->flushed = 1;
            }
        } else if (b->inlen) {
            iz n = MIN(b->inlen, WIN_CAP - d->win_len);
            bytecopy(d->win + d->win_len, b->in, n);
            d->win_len += n;
            b->in += n;
            b->inlen -= n;
        } else {
            return GZ_NEEDIN;
        }
    }
}
