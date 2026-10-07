// tugz core: raw DEFLATE decoder (RFC 1951)
//
// Accepts exactly the streams zlib accepts: incomplete Huffman codes are
// rejected except for a lone 1-bit code, and an empty distance code is
// only an error if a distance is actually decoded.
//
// Decoding is table-driven, with a fast loop that runs while plenty of
// input and output space remain, refilling the bit buffer 64 bits at a
// time. Near buffer edges a careful byte-at-a-time path takes over.
// Output is produced directly into a window that retains 32 KiB of
// history, from which it is handed out as the window fills.
//
// The caller supplies input and output buffers of any size, and decoding
// resumes wherever the previous call stopped.
//
// Window invariant: until history first slides, wpos equals the total
// output so far. Afterwards wpos >= INF_HIST, which exceeds any distance.
// Therefore a distance is too far back exactly when it exceeds wpos.

#define INF_HIST    32768
#define INF_CHUNK   (1 << 18)
#define INF_SLACK   (258 + 32)  // room for one match plus wide-copy overrun
#define INF_WINCAP  (INF_HIST + INF_CHUNK + INF_SLACK)
#define LIT_ROOT    11
#define DIST_ROOT   8
#define LIT_ENOUGH  2342  // zlib's enough for 286 symbols, 11-bit root
#define DIST_ENOUGH 402   // zlib's enough for 30 symbols, 8-bit root

enum {
    HUFF_CODELEN,
    HUFF_LITLEN,
    HUFF_DIST,
};

// Table entry layout:
//   bits  0..4   total bits: code length plus extra bits
//   bit   6      F_LINK: subtable link
//   bit   7      F_LIT: literal
//   bits  8..11  code length (root bits for a subtable link)
//   bits 12..15  subtable index bits (links only)
//   bits 16..30  value: literal, base length/distance, or subtable offset
//   bit  30      F_EOB: end of block (only with F_SPECIAL)
//   bit  31      F_SPECIAL: end of block or invalid code
// Length and distance entries have no flags. Flags make the common tests
// single-bit tests. Subtable entries hold the full code length, so a
// lookup never needs to consume the root bits separately.
enum {
    ENT_LIT,
    ENT_LEN,  // also distances
    ENT_EOB,
    ENT_SUB,
    ENT_BAD,
};
#define F_LINK      (1u << 6)
#define F_LIT       (1u << 7)
#define F_EOB       (1u << 30)
#define F_SPECIAL   (1u << 31)
#define ENT_FLAGS(kind) \
    ((kind)==ENT_LIT ? F_LIT : (kind)==ENT_SUB ? F_LINK : \
     (kind)==ENT_EOB ? F_SPECIAL|F_EOB : (kind)==ENT_BAD ? F_SPECIAL : 0)
#define ENT(len, kind, extra, val) \
    ((u32)((len) + (extra)) | ENT_FLAGS(kind) | (u32)(len)<<8 | (u32)(val)<<16)
#define ENT_LINK(root, bits, off) \
    (F_LINK | (u32)(root)<<8 | (u32)(bits)<<12 | (u32)(off)<<16)
#define ENT_TOTAL(e)    ((i32)((e) & 31))
#define ENT_CODELEN(e)  ((i32)((e)>>8 & 15))
#define ENT_SUBBITS(e)  ((i32)((e)>>12 & 15))
#define ENT_EXTRA(e)    (ENT_TOTAL(e) - ENT_CODELEN(e))
#define ENT_VAL(e)      ((i32)((e)>>16 & 0x7fff))
#define ENT_KIND(e) \
    ((e) & F_LINK ? ENT_SUB : (e) & F_LIT ? ENT_LIT : \
     !((e) & F_SPECIAL) ? ENT_LEN : (e) & F_EOB ? ENT_EOB : ENT_BAD)

typedef struct {
    u32 const *entries;
    u32        mask;
} htable;

enum {
    INF_HEAD,     // next is a block header
    INF_LENS,     // reading a dynamic block's code lengths
    INF_STORED,   // copying stored block data
    INF_SYMBOLS,  // decoding a compressed block
    INF_END,      // the final block has ended
};

// Decoding proceeds in atomic units: a block header (for a dynamic
// block, up to its code lengths), a run of a dynamic block's code
// lengths, or one literal or length/distance pair. When input runs out
// partway through a unit, it is rolled back and its bytes are saved in
// the stash, to be completed by the next call's input. A unit that runs
// out needs at most 10 bytes (a dynamic block header's 74 bits; a run of
// code lengths runs out only in its first), which the stash covers.
// Since the code lengths resume where they stopped, as zlib's do, input
// in tiny pieces costs no more per byte than it otherwise would.
//
// Input invariant: between calls the bit buffer holds fewer than 8
// bits. Whole bytes are always given back to the input, so the stream
// end is reported exactly, and the stash only ever holds bytes of one
// incomplete unit.
#define INF_STASH   32

typedef struct {
    u8 const *in;     // input cursor for the current call
    u8 const *inend;

    u64 bitbuf;   // bits above bitcnt are zero or upcoming input
    i32 bitcnt;
    i32 err;      // sticky error, or GZ_NEEDIN while a unit is starved

    i32 state;
    b32 final;
    iz  stored;   // stored block bytes remaining
    htable lt;    // the current block's codes: fixed, or in the entries
    htable dt;

    u8 *win;
    iz  wpos;
    iz  wflushed; // output before this has been handed out

    iz  stashlen;
    u8  stash[INF_STASH];

    // A dynamic block's code descriptions, while being read
    htable cl;      // code length code
    i32 nlit;       // literal/length code lengths, then distance ones
    i32 nlens;      // total
    i32 nread;      // read so far
    u16 lens[286+30];
    u32 cl_entries[128];

    u32 lit_entries[LIT_ENOUGH];
    u32 dist_entries[DIST_ENOUGH];
} inflator;

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

// Decoding tables for the fixed codes (RFC 1951, 3.2.6), exactly as
// htable_build makes them from the fixed code lengths: 9-bit and 5-bit
// root tables, the latter with distance codes 30 and 31 invalid. As
// constants they cost an inflator nothing to set up. test_tables in
// test/tests.c checks them against htable_build.
static u32 const inf_fixlit[512] = {
    0xc0000707, 0x00500888, 0x00100888, 0x0073080c, 0x001f0709, 0x00700888,
    0x00300888, 0x00c00989, 0x000a0707, 0x00600888, 0x00200888, 0x00a00989,
    0x00000888, 0x00800888, 0x00400888, 0x00e00989, 0x00060707, 0x00580888,
    0x00180888, 0x00900989, 0x003b070a, 0x00780888, 0x00380888, 0x00d00989,
    0x00110708, 0x00680888, 0x00280888, 0x00b00989, 0x00080888, 0x00880888,
    0x00480888, 0x00f00989, 0x00040707, 0x00540888, 0x00140888, 0x00e3080d,
    0x002b070a, 0x00740888, 0x00340888, 0x00c80989, 0x000d0708, 0x00640888,
    0x00240888, 0x00a80989, 0x00040888, 0x00840888, 0x00440888, 0x00e80989,
    0x00080707, 0x005c0888, 0x001c0888, 0x00980989, 0x0053070b, 0x007c0888,
    0x003c0888, 0x00d80989, 0x00170709, 0x006c0888, 0x002c0888, 0x00b80989,
    0x000c0888, 0x008c0888, 0x004c0888, 0x00f80989, 0x00030707, 0x00520888,
    0x00120888, 0x00a3080d, 0x0023070a, 0x00720888, 0x00320888, 0x00c40989,
    0x000b0708, 0x00620888, 0x00220888, 0x00a40989, 0x00020888, 0x00820888,
    0x00420888, 0x00e40989, 0x00070707, 0x005a0888, 0x001a0888, 0x00940989,
    0x0043070b, 0x007a0888, 0x003a0888, 0x00d40989, 0x00130709, 0x006a0888,
    0x002a0888, 0x00b40989, 0x000a0888, 0x008a0888, 0x004a0888, 0x00f40989,
    0x00050707, 0x00560888, 0x00160888, 0x80000808, 0x0033070a, 0x00760888,
    0x00360888, 0x00cc0989, 0x000f0708, 0x00660888, 0x00260888, 0x00ac0989,
    0x00060888, 0x00860888, 0x00460888, 0x00ec0989, 0x00090707, 0x005e0888,
    0x001e0888, 0x009c0989, 0x0063070b, 0x007e0888, 0x003e0888, 0x00dc0989,
    0x001b0709, 0x006e0888, 0x002e0888, 0x00bc0989, 0x000e0888, 0x008e0888,
    0x004e0888, 0x00fc0989, 0xc0000707, 0x00510888, 0x00110888, 0x0083080d,
    0x001f0709, 0x00710888, 0x00310888, 0x00c20989, 0x000a0707, 0x00610888,
    0x00210888, 0x00a20989, 0x00010888, 0x00810888, 0x00410888, 0x00e20989,
    0x00060707, 0x00590888, 0x00190888, 0x00920989, 0x003b070a, 0x00790888,
    0x00390888, 0x00d20989, 0x00110708, 0x00690888, 0x00290888, 0x00b20989,
    0x00090888, 0x00890888, 0x00490888, 0x00f20989, 0x00040707, 0x00550888,
    0x00150888, 0x01020808, 0x002b070a, 0x00750888, 0x00350888, 0x00ca0989,
    0x000d0708, 0x00650888, 0x00250888, 0x00aa0989, 0x00050888, 0x00850888,
    0x00450888, 0x00ea0989, 0x00080707, 0x005d0888, 0x001d0888, 0x009a0989,
    0x0053070b, 0x007d0888, 0x003d0888, 0x00da0989, 0x00170709, 0x006d0888,
    0x002d0888, 0x00ba0989, 0x000d0888, 0x008d0888, 0x004d0888, 0x00fa0989,
    0x00030707, 0x00530888, 0x00130888, 0x00c3080d, 0x0023070a, 0x00730888,
    0x00330888, 0x00c60989, 0x000b0708, 0x00630888, 0x00230888, 0x00a60989,
    0x00030888, 0x00830888, 0x00430888, 0x00e60989, 0x00070707, 0x005b0888,
    0x001b0888, 0x00960989, 0x0043070b, 0x007b0888, 0x003b0888, 0x00d60989,
    0x00130709, 0x006b0888, 0x002b0888, 0x00b60989, 0x000b0888, 0x008b0888,
    0x004b0888, 0x00f60989, 0x00050707, 0x00570888, 0x00170888, 0x80000808,
    0x0033070a, 0x00770888, 0x00370888, 0x00ce0989, 0x000f0708, 0x00670888,
    0x00270888, 0x00ae0989, 0x00070888, 0x00870888, 0x00470888, 0x00ee0989,
    0x00090707, 0x005f0888, 0x001f0888, 0x009e0989, 0x0063070b, 0x007f0888,
    0x003f0888, 0x00de0989, 0x001b0709, 0x006f0888, 0x002f0888, 0x00be0989,
    0x000f0888, 0x008f0888, 0x004f0888, 0x00fe0989, 0xc0000707, 0x00500888,
    0x00100888, 0x0073080c, 0x001f0709, 0x00700888, 0x00300888, 0x00c10989,
    0x000a0707, 0x00600888, 0x00200888, 0x00a10989, 0x00000888, 0x00800888,
    0x00400888, 0x00e10989, 0x00060707, 0x00580888, 0x00180888, 0x00910989,
    0x003b070a, 0x00780888, 0x00380888, 0x00d10989, 0x00110708, 0x00680888,
    0x00280888, 0x00b10989, 0x00080888, 0x00880888, 0x00480888, 0x00f10989,
    0x00040707, 0x00540888, 0x00140888, 0x00e3080d, 0x002b070a, 0x00740888,
    0x00340888, 0x00c90989, 0x000d0708, 0x00640888, 0x00240888, 0x00a90989,
    0x00040888, 0x00840888, 0x00440888, 0x00e90989, 0x00080707, 0x005c0888,
    0x001c0888, 0x00990989, 0x0053070b, 0x007c0888, 0x003c0888, 0x00d90989,
    0x00170709, 0x006c0888, 0x002c0888, 0x00b90989, 0x000c0888, 0x008c0888,
    0x004c0888, 0x00f90989, 0x00030707, 0x00520888, 0x00120888, 0x00a3080d,
    0x0023070a, 0x00720888, 0x00320888, 0x00c50989, 0x000b0708, 0x00620888,
    0x00220888, 0x00a50989, 0x00020888, 0x00820888, 0x00420888, 0x00e50989,
    0x00070707, 0x005a0888, 0x001a0888, 0x00950989, 0x0043070b, 0x007a0888,
    0x003a0888, 0x00d50989, 0x00130709, 0x006a0888, 0x002a0888, 0x00b50989,
    0x000a0888, 0x008a0888, 0x004a0888, 0x00f50989, 0x00050707, 0x00560888,
    0x00160888, 0x80000808, 0x0033070a, 0x00760888, 0x00360888, 0x00cd0989,
    0x000f0708, 0x00660888, 0x00260888, 0x00ad0989, 0x00060888, 0x00860888,
    0x00460888, 0x00ed0989, 0x00090707, 0x005e0888, 0x001e0888, 0x009d0989,
    0x0063070b, 0x007e0888, 0x003e0888, 0x00dd0989, 0x001b0709, 0x006e0888,
    0x002e0888, 0x00bd0989, 0x000e0888, 0x008e0888, 0x004e0888, 0x00fd0989,
    0xc0000707, 0x00510888, 0x00110888, 0x0083080d, 0x001f0709, 0x00710888,
    0x00310888, 0x00c30989, 0x000a0707, 0x00610888, 0x00210888, 0x00a30989,
    0x00010888, 0x00810888, 0x00410888, 0x00e30989, 0x00060707, 0x00590888,
    0x00190888, 0x00930989, 0x003b070a, 0x00790888, 0x00390888, 0x00d30989,
    0x00110708, 0x00690888, 0x00290888, 0x00b30989, 0x00090888, 0x00890888,
    0x00490888, 0x00f30989, 0x00040707, 0x00550888, 0x00150888, 0x01020808,
    0x002b070a, 0x00750888, 0x00350888, 0x00cb0989, 0x000d0708, 0x00650888,
    0x00250888, 0x00ab0989, 0x00050888, 0x00850888, 0x00450888, 0x00eb0989,
    0x00080707, 0x005d0888, 0x001d0888, 0x009b0989, 0x0053070b, 0x007d0888,
    0x003d0888, 0x00db0989, 0x00170709, 0x006d0888, 0x002d0888, 0x00bb0989,
    0x000d0888, 0x008d0888, 0x004d0888, 0x00fb0989, 0x00030707, 0x00530888,
    0x00130888, 0x00c3080d, 0x0023070a, 0x00730888, 0x00330888, 0x00c70989,
    0x000b0708, 0x00630888, 0x00230888, 0x00a70989, 0x00030888, 0x00830888,
    0x00430888, 0x00e70989, 0x00070707, 0x005b0888, 0x001b0888, 0x00970989,
    0x0043070b, 0x007b0888, 0x003b0888, 0x00d70989, 0x00130709, 0x006b0888,
    0x002b0888, 0x00b70989, 0x000b0888, 0x008b0888, 0x004b0888, 0x00f70989,
    0x00050707, 0x00570888, 0x00170888, 0x80000808, 0x0033070a, 0x00770888,
    0x00370888, 0x00cf0989, 0x000f0708, 0x00670888, 0x00270888, 0x00af0989,
    0x00070888, 0x00870888, 0x00470888, 0x00ef0989, 0x00090707, 0x005f0888,
    0x001f0888, 0x009f0989, 0x0063070b, 0x007f0888, 0x003f0888, 0x00df0989,
    0x001b0709, 0x006f0888, 0x002f0888, 0x00bf0989, 0x000f0888, 0x008f0888,
    0x004f0888, 0x00ff0989,
};
static u32 const inf_fixdist[32] = {
    0x00010505, 0x0101050c, 0x00110508, 0x10010510, 0x00050506, 0x0401050e,
    0x0041050a, 0x40010512, 0x00030505, 0x0201050d, 0x00210509, 0x20010511,
    0x00090507, 0x0801050f, 0x0081050b, 0x80000505, 0x00020505, 0x0181050c,
    0x00190508, 0x18010510, 0x00070506, 0x0601050e, 0x0061050a, 0x60010512,
    0x00040505, 0x0301050d, 0x00310509, 0x30010511, 0x000d0507, 0x0c01050f,
    0x00c1050b, 0x80000505,
};

static u32 sym_entry(i32 sym, i32 len, i32 kind)
{
    switch (kind) {
    case HUFF_CODELEN:
        return ENT(len, ENT_LIT, 0, sym);
    case HUFF_LITLEN:
        if (sym < 256) {
            return ENT(len, ENT_LIT, 0, sym);
        } else if (sym == 256) {
            return ENT(len, ENT_EOB, 0, 0);
        } else if (sym < 286) {
            sym -= 257;
            return ENT(len, ENT_LEN, inf_len_extra[sym], inf_len_base[sym]);
        }
        break;
    case HUFF_DIST:
        if (sym < 30) {
            return ENT(len, ENT_LEN, inf_dist_extra[sym], inf_dist_base[sym]);
        }
        break;
    }
    return ENT(len, ENT_BAD, 0, 0);
}

// Build a decoding table from code lengths, returning false if the
// lengths do not describe a code that zlib would accept. Adapted from
// zlib's inflate_table: a root table indexed by the low bits, with
// subtables for longer codes.
static b32 htable_build(htable *t, u32 *entries, iz cap, u16 const *lens,
                        i32 n, i32 kind, i32 rootbits)
{
    u16 count[16] = {0};
    for (i32 i = 0; i < n; i++) {
        count[lens[i]]++;
    }
    i32 max = 15;
    for (; max && !count[max]; max--) {}
    t->entries = entries;

    if (!max) {
        // Nothing can be decoded, which is only allowed for distances.
        // Like zlib, an empty code length code decodes every length as a
        // 1-bit zero, failing later on the missing end-of-block code.
        t->mask = 1;
        entries[0] = entries[1] = kind==HUFF_CODELEN ? ENT(1, ENT_LIT, 0, 0)
                                                     : ENT(1, ENT_BAD, 0, 0);
        return kind != HUFF_LITLEN;
    }

    i32 left = 1;
    for (i32 len = 1; len < 16; len++) {
        left = (left << 1) - count[len];
        if (left < 0) {
            return 0;  // over-subscribed
        }
    }
    if (left>0 && (kind==HUFF_CODELEN || max!=1)) {
        return 0;  // incomplete
    }

    u16 offs[16];
    offs[1] = 0;
    for (i32 len = 1; len < 15; len++) {
        offs[len+1] = (u16)(offs[len] + count[len]);
    }
    u16 work[288];
    for (i32 i = 0; i < n; i++) {
        if (lens[i]) {
            work[offs[lens[i]]++] = (u16)i;
        }
    }

    i32 len = 1;
    for (; !count[len]; len++) {}
    i32 root = MIN(rootbits, max);
    t->mask = (1u << root) - 1;

    u32 *next = entries;
    i32  curr = root;
    i32  drop = 0;
    u32  low  = (u32)-1;
    iz   used = (iz)1 << root;
    u32  huff = 0;  // bit-reversed code of the current symbol
    for (i32 sym = 0;; sym++) {
        // Replicate the entry across the current (sub)table
        u32 here = sym_entry(work[sym], len, kind);
        u32 incr = 1u << (len - drop);
        u32 fill = 1u << curr;
        u32 size = fill;
        do {
            fill -= incr;
            next[(huff >> drop) + fill] = here;
        } while (fill);

        // Increment the bit-reversed code
        incr = 1u << (len - 1);
        for (; huff & incr; incr >>= 1) {}
        huff = incr ? (huff & (incr - 1)) + incr : 0;

        if (!--count[len]) {
            if (len == max) {
                break;
            }
            len = lens[work[sym+1]];
        }

        // Start a new subtable when the low root bits change
        if (len>root && (huff & t->mask)!=low) {
            if (!drop) {
                drop = root;
            }
            next += size;
            curr = len - drop;
            i32 avail = 1 << curr;
            for (; curr+drop < max; curr++, avail <<= 1) {
                avail -= count[curr + drop];
                if (avail <= 0) {
                    break;
                }
            }
            used += (iz)1 << curr;
            assert(used <= cap);
            low = huff & t->mask;
            entries[low] = ENT_LINK(root, curr, next - entries);
        }
    }

    // An incomplete code is a lone 1-bit code with one unused entry
    if (huff) {
        next[huff] = ENT(len, ENT_BAD, 0, 0);
    }
    return 1;
}

// Memory needed by inflate_new, including alignment padding.
[[maybe_unused]] static iz inflate_memsize(void)
{
    return (iz)sizeof(inflator) + _Alignof(inflator) + INF_WINCAP + 64;
}

// Prepare to decode a new stream.
static void inflate_reset(inflator *s)
{
    s->in       = s->inend = 0;
    s->bitbuf   = 0;
    s->bitcnt   = 0;
    s->err      = 0;
    s->state    = INF_HEAD;
    s->final    = 0;
    s->stored   = 0;
    s->wpos     = 0;
    s->wflushed = 0;
    s->stashlen = 0;
}

// Only the fields inflate_reset sets need a value: each block header
// sets lt and dt (a dynamic one through its code lengths, which it
// starts) before any symbol is decoded, every code length is written
// before it is read, and the stash is filled before it is read. So the
// state starts uncleared, and costs no more to set up than to reset.
static inflator *inflate_new(arena *a)
{
    inflator *s = alloc(a, 1, sizeof(inflator), _Alignof(inflator), 0);
    s->win = newbytes(a, INF_WINCAP);
    inflate_reset(s);
    return s;
}

// Note that the current unit cannot complete with the input at hand.
static void inf_starve(inflator *s)
{
    if (!s->err) {
        s->err = GZ_NEEDIN;
    }
}

// Make at least n bits available, one byte at a time. Returns false if
// input ran out first, in which case fewer bits are available.
static b32 inf_need(inflator *s, i32 n)
{
    assert(n <= 32);
    while (s->bitcnt < n) {
        if (s->in == s->inend) {
            return 0;
        }
        s->bitbuf |= (u64)*s->in++ << s->bitcnt;
        s->bitcnt += 8;
    }
    return 1;
}

static void inf_drop(inflator *s, i32 n)
{
    s->bitbuf >>= n;
    s->bitcnt -= n;
}

static u32 inf_bits(inflator *s, i32 n)
{
    if (!inf_need(s, n)) {
        inf_starve(s);
        return 0;
    }
    u32 v = (u32)s->bitbuf & ((1u << n) - 1);
    inf_drop(s, n);
    return v;
}

// Return whole bytes in the bit buffer to the input.
static void inf_giveback(inflator *s)
{
    s->in -= s->bitcnt >> 3;
    s->bitcnt &= 7;
    s->bitbuf &= ((u64)1 << s->bitcnt) - 1;
}

// Look up the entry for the next code, resolving subtables.
static u32 lookup(u32 const *t, u32 mask, u64 bb)
{
    u32 e = t[bb & mask];
    if (e & F_LINK) {
        u32 sub = (u32)(bb >> ENT_CODELEN(e)) & ((1u << ENT_SUBBITS(e)) - 1);
        e = t[ENT_VAL(e) + sub];
    }
    return e;
}

// Decode one code the careful way, consuming only the code itself. Like
// zlib, an invalid code is only reported once all its bits are present.
// Returns a BAD entry on error. The unit must not be starved already:
// the lookup would then begin at bits of the unfinished field, and an
// invalid entry there (a lone or empty code has 1-bit ones) would turn
// truncation into an error.
static u32 inf_decode(inflator *s, htable const *t)
{
    assert(!s->err);
    inf_need(s, 15);
    u32 e = lookup(t->entries, t->mask, s->bitbuf);
    if (ENT_CODELEN(e) > s->bitcnt) {
        inf_starve(s);
        return ENT(0, ENT_BAD, 0, 0);
    } else if (ENT_KIND(e) == ENT_BAD) {
        s->err = GZ_EDATA;
    } else {
        inf_drop(s, ENT_CODELEN(e));
    }
    return e;
}

// Make room for at least one maximum-length match, sliding history to
// the front once all output has been handed out. Returns false if
// undelivered output is in the way.
static b32 inf_room(inflator *s)
{
    if (s->wpos <= INF_WINCAP-INF_SLACK) {
        return 1;
    } else if (s->wflushed < s->wpos) {
        return 0;
    }
    bytemove(s->win, s->win+s->wpos-INF_HIST, INF_HIST);
    s->wpos = s->wflushed = INF_HIST;
    return 1;
}

// Decoded output not yet handed out.
static s8 inflate_pending(inflator *s)
{
    s8 r = {s->win + s->wflushed, s->wpos - s->wflushed};
    return r;
}

// Mark the first n bytes of pending output as handed out.
static void inflate_consume(inflator *s, iz n)
{
    assert(n>=0 && n<=s->wpos-s->wflushed);
    s->wflushed += n;
}

static u64 load64(u8 const *p)
{
    u64 v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static void store64(u8 *p, u64 v)
{
    __builtin_memcpy(p, &v, 8);
}

// Copy a match of len bytes from dist back, writing up to 31 bytes past
// the end (requires slack).
static u8 *copy_match(u8 *out, iz dist, iz len)
{
    u8 *src = out - dist;
    u8 *end = out + len;
    if (dist >= 16) {
        do {
            __builtin_memcpy(out+ 0, src+ 0, 16);
            __builtin_memcpy(out+16, src+16, 16);
            out += 32;
            src += 32;
        } while (out < end);
    } else if (dist >= 8) {
        do {
            store64(out, load64(src));
            out += 8;
            src += 8;
        } while (out < end);
    } else if (dist == 1) {
        u64 v = 0x0101010101010101u * src[0];
        do {
            store64(out+0, v);
            store64(out+8, v);
            out += 16;
        } while (out < end);
    } else {
        // Short period: expand it into a pattern buffer, then store 8
        // bytes at a time, rotating the phase within the period.
        u8 pat[16];
        for (i32 i = 0, j = 0; i < 16; i++, j = j+1==dist ? 0 : j+1) {
            pat[i] = src[j];
        }
        iz step  = 8 % dist;
        iz phase = 0;
        do {
            store64(out, load64(pat + phase));
            out += 8;
            phase += step;
            phase -= phase>=dist ? dist : 0;
        } while (out < end);
    }
    return end;
}

// Fast loop: runs while at least 16 input bytes and room for a full
// match remain. Returns true at end of block, false to fall back to the
// careful path (or on error).
static b32 decode_fast(inflator *s)
{
    htable const *lt = &s->lt;
    htable const *dt = &s->dt;
    u8 const *in    = s->in;
    u8 const *inend = s->inend;
    u8 *win    = s->win;
    u8 *out    = win + s->wpos;
    u8 *outlim = win + INF_WINCAP - INF_SLACK;
    u64 bb = s->bitbuf;
    i32 bc = s->bitcnt;
    u32 const *lte = lt->entries;
    u32 const *dte = dt->entries;
    u32 lmask = lt->mask;
    u32 dmask = dt->mask;
    b32 eob = 0;

    // Each iteration begins with a refill, after which all 64 buffered
    // bits are real input (bitcnt counts only whole bytes, at least 56).
    // An iteration consumes at most 48 bits: three literals (3*15), or a
    // length and distance (15+5+15+13). So at least 16 real bits remain,
    // enough to look up the next code early, overlapping the lookup with
    // the match copy. Refilling only appends above existing bits, so the
    // preloaded entry stays valid. An iteration reads at most 8 bytes.
    #define REFILL() \
        bb |= load64le(in) << bc; \
        in += (63 - bc) >> 3; \
        bc |= 56
    #define CONSUME(e) \
        bb >>= ENT_TOTAL(e); \
        bc -= ENT_TOTAL(e)
    #define EXTRA(e) \
        (iz)(((u32)bb & ((1u << ENT_TOTAL(e)) - 1)) >> ENT_CODELEN(e))

    if (inend-in>=16 && out<=outlim) {
        REFILL();
        u32 e = lookup(lte, lmask, bb);
        for (;;) {
            if (e & F_LIT) {
                CONSUME(e);
                *out++ = (u8)ENT_VAL(e);
                e = lookup(lte, lmask, bb);
                if (e & F_LIT) {
                    CONSUME(e);
                    *out++ = (u8)ENT_VAL(e);
                    e = lookup(lte, lmask, bb);
                    if (e & F_LIT) {
                        CONSUME(e);
                        *out++ = (u8)ENT_VAL(e);
                        e = lookup(lte, lmask, bb);
                    }
                }
            } else if (!(e & F_SPECIAL)) {
                iz len = ENT_VAL(e) + EXTRA(e);
                CONSUME(e);

                e = lookup(dte, dmask, bb);
                if (e & F_SPECIAL) {
                    s->err = GZ_EDATA;
                    break;
                }
                iz dist = ENT_VAL(e) + EXTRA(e);
                CONSUME(e);
                if (dist > out-win) {
                    s->err = GZ_EDATA;
                    break;
                }

                e = lookup(lte, lmask, bb);
                out = copy_match(out, dist, len);
            } else {
                eob = !!(e & F_EOB);
                if (!eob) {
                    s->err = GZ_EDATA;
                    break;
                }
                CONSUME(e);
                break;
            }

            if (inend-in<16 || out>outlim) {
                break;
            }
            REFILL();
        }
    }
    #undef EXTRA
    #undef CONSUME
    #undef REFILL

    s->in = in;
    s->wpos = out - win;
    s->bitbuf = bb;
    s->bitcnt = bc;
    inf_giveback(s);
    return eob;
}

// Careful path: decode one literal, length/distance pair, or end of
// block. Output is only written once the whole unit has been decoded.
// Like zlib, each field is finished before the next begins, so input
// running out anywhere in the unit is truncation.
static void inf_symbol(inflator *s)
{
    u32 e = inf_decode(s, &s->lt);
    if (s->err) {
        return;
    } else if (e & F_LIT) {
        s->win[s->wpos++] = (u8)ENT_VAL(e);
        return;
    } else if (e & F_SPECIAL) {
        s->state = s->final ? INF_END : INF_HEAD;  // end of block
        return;
    }
    iz len = ENT_VAL(e) + inf_bits(s, ENT_EXTRA(e));
    if (s->err) {
        return;
    }
    e = inf_decode(s, &s->dt);
    if (s->err) {
        return;
    }
    iz dist = ENT_VAL(e) + inf_bits(s, ENT_EXTRA(e));
    if (s->err) {
        return;
    } else if (dist > s->wpos) {
        s->err = GZ_EDATA;
        return;
    }
    u8 *out = s->win + s->wpos;
    for (iz i = 0; i < len; i++) {
        out[i] = out[i-dist];
    }
    s->wpos += len;
}

// Read a dynamic block's code counts and code length code, which
// describes the code lengths that follow (inf_lens).
static void inf_dynamic(inflator *s)
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

    u16 cllens[19] = {0};
    for (i32 i = 0; i < hclen; i++) {
        cllens[inf_cl_order[i]] = (u16)inf_bits(s, 3);
    }
    if (s->err) {
        return;
    } else if (!htable_build(&s->cl, s->cl_entries, countof(s->cl_entries),
                             cllens, 19, HUFF_CODELEN, 7)) {
        s->err = GZ_EDATA;
        return;
    }
    s->nlit  = hlit;
    s->nlens = hlit + hdist;
    s->nread = 0;
}

// Read a dynamic block's code lengths, one per symbol (with its repeat),
// and once all are read, build the block's tables into lt and dt. Only
// the first can run out of input: each takes at most 14 bits, and the
// next is read only with 2 more bytes at hand. Errors are found as zlib
// finds them, the tables' once the last length is read.
static void inf_lens(inflator *s)
{
    i32  total = s->nlens;
    i32  n     = s->nread;
    u16 *lens  = s->lens;
    do {
        u32 e = inf_decode(s, &s->cl);
        if (s->err) {
            return;
        }
        i32 sym = ENT_VAL(e);
        if (sym < 16) {
            lens[n++] = (u16)sym;
            continue;
        }

        // Like zlib, read the extra bits before validating the repeat
        u16 fill = 0;
        i32 repeat = 0;
        switch (sym) {
        case 16:
            repeat = 3 + (i32)inf_bits(s, 2);
            fill = n ? lens[n-1] : 0;
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
        } else if ((sym==16 && !n) || repeat>total-n) {
            s->err = GZ_EDATA;
            return;
        }
        for (; repeat; repeat--) {
            lens[n++] = fill;
        }
    } while (n<total && s->inend-s->in>=2);
    s->nread = n;
    if (n < total) {
        return;
    }

    if (!lens[256] ||
        !htable_build(&s->lt, s->lit_entries, countof(s->lit_entries),
                      lens, s->nlit, HUFF_LITLEN, LIT_ROOT) ||
        !htable_build(&s->dt, s->dist_entries, countof(s->dist_entries),
                      lens+s->nlit, total-s->nlit, HUFF_DIST, DIST_ROOT)) {
        s->err = GZ_EDATA;
        return;
    }
    s->state = INF_SYMBOLS;
}

// Read a block header, including a stored block's lengths or the start
// of a dynamic block's code descriptions.
static void inf_header(inflator *s)
{
    b32 final = (b32)inf_bits(s, 1);
    i32 type  = (i32)inf_bits(s, 2);
    if (s->err) {
        return;
    }
    switch (type) {
    case 0: {
        inf_drop(s, s->bitcnt & 7);
        u32 len  = inf_bits(s, 16);
        u32 nlen = inf_bits(s, 16);
        if (s->err) {
            return;
        } else if (len != (~nlen & 0xffff)) {
            s->err = GZ_EDATA;
            return;
        }
        inf_giveback(s);  // the bit buffer is now empty
        s->stored = len;
        s->state = INF_STORED;
    } break;
    case 1:
        s->lt = (htable){inf_fixlit, countof(inf_fixlit) - 1};
        s->dt = (htable){inf_fixdist, countof(inf_fixdist) - 1};
        s->state = INF_SYMBOLS;
        break;
    case 2:
        inf_dynamic(s);
        if (s->err) {
            return;
        }
        s->state = INF_LENS;
        break;
    default:
        s->err = GZ_EDATA;
        return;
    }
    s->final = final;
}

// Run one atomic unit. If input runs out, roll it back and return false.
static b32 inf_unit(inflator *s)
{
    u8 const *in = s->in;
    u64 bitbuf = s->bitbuf;
    i32 bitcnt = s->bitcnt;
    switch (s->state) {
    case INF_HEAD: inf_header(s); break;
    case INF_LENS: inf_lens(s);   break;
    default:       inf_symbol(s);
    }
    if (s->err == GZ_NEEDIN) {
        s->in = in;
        s->bitbuf = bitbuf;
        s->bitcnt = bitcnt;
        s->err = 0;
        return 0;
    }
    return 1;
}

// Decode until the input runs out, undelivered output fills the window,
// or the stream ends. Returns a GZ_* status.
static i32 inf_run(inflator *s)
{
    for (;;) {
        if (s->err) {
            return s->err;
        } else if (s->state == INF_END) {
            return GZ_OK;
        } else if (!inf_room(s)) {
            return GZ_NEEDOUT;
        }

        switch (s->state) {
        case INF_STORED: {
            if (!s->stored) {
                s->state = s->final ? INF_END : INF_HEAD;
                continue;
            }
            iz n = MIN(s->stored, s->inend - s->in);
            n = MIN(n, INF_WINCAP - s->wpos);
            if (!n) {
                return GZ_NEEDIN;
            }
            bytecopy(s->win + s->wpos, s->in, n);
            s->wpos += n;
            s->in += n;
            s->stored -= n;
        } continue;
        case INF_SYMBOLS:
            if (s->inend-s->in >= 16) {
                if (decode_fast(s)) {
                    s->state = s->final ? INF_END : INF_HEAD;
                }
                continue;
            }
            break;
        }

        if (!inf_unit(s)) {
            return GZ_NEEDIN;
        }
    }
}

// Copy pending output into the caller's buffer.
static void inf_drain(inflator *s, zbuf *b)
{
    s8 p = inflate_pending(s);
    iz n = MIN(p.len, b->outlen);
    if (n) {
        bytecopy(b->out, p.s, n);
        b->out += n;
        b->outlen -= n;
        inflate_consume(s, n);
    }
}

// Status of a call that has delivered what output fits: GZ_NEEDOUT while
// decoded output remains to be delivered, else r.
static i32 inf_held(inflator *s, i32 r)
{
    return s->wflushed<s->wpos ? GZ_NEEDOUT : r;
}

// Decode from b->in into b->out, advancing both. Returns GZ_NEEDOUT while
// decoded output remains to be delivered, which is only once b->out is
// full. Otherwise all output decoded so far has been delivered, and it
// returns GZ_OK at the end of the stream, with b->in just past it,
// GZ_NEEDIN when all input has been consumed, or an error. Decoding runs
// ahead of b->out into the window, so without this a caller would see
// the input run out, or an error, with output still held. An error thus
// waits for the output before it, as with zlib, which decodes no further
// than its output buffer.
//
// Output may also be taken without copying through inflate_pending and
// inflate_consume, in which case b->out may be empty, and GZ_NEEDOUT
// asks the caller to take the pending output and call again.
static i32 inflate_stream(inflator *s, zbuf *b)
{
    if (s->err) {
        inf_drain(s, b);
        return inf_held(s, s->err);
    }

    if (s->stashlen) {
        // Complete the unit begun by an earlier call. All stashed bytes
        // belong to it, so whatever remains afterward came from b->in.
        iz n = MIN(b->inlen, INF_STASH - s->stashlen);
        if (n) {
            bytecopy(s->stash + s->stashlen, b->in, n);
        }
        s->stashlen += n;
        b->in += n;
        b->inlen -= n;
        s->in = s->stash;
        s->inend = s->stash + s->stashlen;
        if (!inf_unit(s)) {
            assert(s->stashlen < INF_STASH);
            inf_drain(s, b);
            return inf_held(s, GZ_NEEDIN);
        }
        inf_giveback(s);
        iz left = s->inend - s->in;
        b->in -= left;
        b->inlen += left;
        s->stashlen = 0;
    }

    s->in = b->in;
    s->inend = b->in + b->inlen;
    i32 r;
    do {
        r = inf_run(s);
        inf_drain(s, b);
    } while (r==GZ_NEEDOUT && s->wflushed==s->wpos);

    inf_giveback(s);
    if (r == GZ_NEEDIN) {
        iz n = s->inend - s->in;
        assert(n < INF_STASH);
        bytecopy(s->stash, s->in, n);
        s->stashlen = n;
        s->in = s->inend;
    }
    b->in = s->in;
    b->inlen = s->inend - s->in;

    if (r == GZ_OK) {
        s->bitbuf = 0;  // padding in the final byte
        s->bitcnt = 0;
    }
    return inf_held(s, r);
}
