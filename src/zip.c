// tugz ZIP format: headers, Zip64, archive parsing, names
//
// No I/O: headers are encoded into and parsed from memory, and the zip
// program (zipcli.c) moves the bytes. Sizes and offsets within archives
// are i64, since archives may exceed the address space on 32-bit hosts.
// Reference: PKWARE APPNOTE.TXT 6.3.

#define ZIP_LOCAL_SIG    0x04034b50u
#define ZIP_CENTRAL_SIG  0x02014b50u
#define ZIP_DESC_SIG     0x08074b50u
#define ZIP_END_SIG      0x06054b50u
#define ZIP_END64_SIG    0x06064b50u
#define ZIP_LOC64_SIG    0x07064b50u
#define ZIP_MAX16        0xffff
#define ZIP_MAX32        0xffffffffll

#define ZIP_LOCAL_LEN    30
#define ZIP_CENTRAL_LEN  46
#define ZIP_END_LEN      22
#define ZIP_END64_LEN    56
#define ZIP_LOC64_LEN    20

enum {
    ZIP_STORE    = 0,
    ZIP_DEFLATE  = 8,
};

enum {
    ZIP_FLAG_ENCRYPTED  = 1 << 0,
    ZIP_FLAG_DESCRIPTOR = 1 << 3,
    ZIP_FLAG_UTF8       = 1 << 11,
};

enum {
    ZIP_EXTRA_ZIP64     = 0x0001,
    ZIP_EXTRA_TIME      = 0x5455,  // "UT": Unix times
    ZIP_EXTRA_UPATH     = 0x7075,  // "up": Info-ZIP Unicode path
    ZIP_EXTRA_UNIX      = 0x7875,  // "ux": Unix UID and GID
    ZIP_EXTRA_UNIX1     = 0x5855,  // "UX": Info-ZIP's first, with times
    ZIP_EXTRA_UNIX2     = 0x7855,  // "Ux": Info-ZIP's second, IDs only
};

// Results of parsing an archive's end records.
enum {
    ZIP_OK,
    ZIP_ENOEND,   // no end of central directory record
    ZIP_EMULTI,   // split or spanned archive
    ZIP_EPREFIX,  // data precedes the archive (self-extractor)
    ZIP_EFORMAT,  // inconsistent structure
};

// An entry, one for each of an existing archive's, and so kept small: a
// byte for the flag leaves no padding (112 bytes on 64-bit hosts).
typedef struct {
    s8  name;
    s8  lextra;   // local extra fields, without Zip64
    s8  cextra;   // central extra fields, without Zip64
    s8  comment;
    i64 usize;
    i64 csize;
    i64 offset;   // of the local header
    u32 crc;
    u32 dostime;  // date<<16 | time
    u32 extattr;
    u16 made;     // version made by
    u16 needed;   // minimum version needed to extract, beyond our own
    u16 flags;
    u16 method;
    u16 intattr;
    u8  zip64;    // the local header carries a Zip64 extra
} zentry;

typedef struct {
    i64 count;
    i64 cdsize;
    i64 cdoff;
    i64 end64;    // offset of the Zip64 end record, or -1
    i64 endpos;   // offset of the end of central directory record
    s8  comment;
    u32 disk;     // disk numbers from the end record
    u32 cddisk;
    u32 ndisk;    // entries on this disk
} zend;

static u8 *put16(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 0);
    p[1] = (u8)(v >> 8);
    return p + 2;
}

static u8 *put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >>  0);
    p[1] = (u8)(v >>  8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
    return p + 4;
}

static u8 *put64(u8 *p, u64 v)
{
    store64le(p, v);
    return p + 8;
}

static u8 *putbytes(u8 *p, s8 s)
{
    bytecopy(p, s.s, s.len);
    return p + s.len;
}

static u32 get16(u8 const *p)
{
    return (u32)p[0] | (u32)p[1]<<8;
}

static u32 get32(u8 const *p)
{
    return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24;
}

static u64 get64(u8 const *p)
{
    return load64le(p);
}

static u32 min32(i64 v)
{
    return v<ZIP_MAX32 ? (u32)v : (u32)ZIP_MAX32;
}

// Version needed to extract: 4.5 for Zip64, 2.0 for deflate, otherwise
// 1.0, or more if a copied entry already required it. The version is
// the low byte; a copied entry keeps whatever its writer put above it.
static u16 zip_needed(zentry const *e)
{
    b32 big = e->zip64 || e->offset>=ZIP_MAX32 || e->usize>=ZIP_MAX32 ||
              e->csize>=ZIP_MAX32;
    u16 v   = big ? 45 : e->method==ZIP_DEFLATE ? 20 : 10;
    u16 old = e->needed & 0xff;
    return (u16)(MAX(v, old) | (e->needed & 0xff00));
}

static iz zip_local_len(zentry const *e)
{
    return ZIP_LOCAL_LEN + e->name.len + (e->zip64 ? 20 : 0) + e->lextra.len;
}

// Encode a local header, zip_local_len bytes. A Zip64 entry records both
// sizes in its extra field, as required.
static u8 *zip_local(u8 *p, zentry const *e)
{
    p = put32(p, ZIP_LOCAL_SIG);
    p = put16(p, zip_needed(e));
    p = put16(p, e->flags);
    p = put16(p, e->method);
    p = put16(p, e->dostime & 0xffff);
    p = put16(p, e->dostime >> 16);
    p = put32(p, e->crc);
    p = put32(p, e->zip64 ? (u32)ZIP_MAX32 : (u32)e->csize);
    p = put32(p, e->zip64 ? (u32)ZIP_MAX32 : (u32)e->usize);
    p = put16(p, (u32)e->name.len);
    p = put16(p, (u32)(e->lextra.len + (e->zip64 ? 20 : 0)));
    p = putbytes(p, e->name);
    if (e->zip64) {
        p = put16(p, ZIP_EXTRA_ZIP64);
        p = put16(p, 16);
        p = put64(p, (u64)e->usize);
        p = put64(p, (u64)e->csize);
    }
    return putbytes(p, e->lextra);
}

// Data descriptor following an entry's data, at most 24 bytes, for
// entries that keep the descriptor flag.
static u8 *zip_desc(u8 *p, zentry const *e)
{
    p = put32(p, ZIP_DESC_SIG);
    p = put32(p, e->crc);
    if (e->zip64) {
        p = put64(p, (u64)e->csize);
        return put64(p, (u64)e->usize);
    }
    p = put32(p, (u32)e->csize);
    return put32(p, (u32)e->usize);
}

// A central Zip64 extra, when anything overflows, holds both sizes, with
// their 32-bit fields saturated, and then the offset if it overflows.
// Holding only the fields that overflow, as APPNOTE also allows, trips
// UnZip 6.0: once it has read a size of exactly 0xffffffff from one, it
// expects every later Zip64 extra to begin with that size, and so reads
// an entry's offset as its size.
static iz zip_central64_len(zentry const *e)
{
    b32 off = e->offset >= ZIP_MAX32;
    b32 any = off || e->usize>=ZIP_MAX32 || e->csize>=ZIP_MAX32;
    return any ? 4 + 16 + (off ? 8 : 0) : 0;
}

static iz zip_central_len(zentry const *e)
{
    return ZIP_CENTRAL_LEN + e->name.len + zip_central64_len(e) +
           e->cextra.len + e->comment.len;
}

// Encode a central directory header, zip_central_len bytes.
static u8 *zip_central(u8 *p, zentry const *e)
{
    iz z64 = zip_central64_len(e);
    p = put32(p, ZIP_CENTRAL_SIG);
    p = put16(p, e->made);
    p = put16(p, zip_needed(e));
    p = put16(p, e->flags);
    p = put16(p, e->method);
    p = put16(p, e->dostime & 0xffff);
    p = put16(p, e->dostime >> 16);
    p = put32(p, e->crc);
    p = put32(p, z64 ? (u32)ZIP_MAX32 : (u32)e->csize);
    p = put32(p, z64 ? (u32)ZIP_MAX32 : (u32)e->usize);
    p = put16(p, (u32)e->name.len);
    p = put16(p, (u32)(z64 + e->cextra.len));
    p = put16(p, (u32)e->comment.len);
    p = put16(p, 0);  // disk number start
    p = put16(p, e->intattr);
    p = put32(p, e->extattr);
    p = put32(p, min32(e->offset));
    p = putbytes(p, e->name);
    if (z64) {
        p = put16(p, ZIP_EXTRA_ZIP64);
        p = put16(p, (u32)(z64 - 4));
        p = put64(p, (u64)e->usize);
        p = put64(p, (u64)e->csize);
        if (e->offset >= ZIP_MAX32) {
            p = put64(p, (u64)e->offset);
        }
    }
    p = putbytes(p, e->cextra);
    return putbytes(p, e->comment);
}

// Whether an entry's name, extra fields (with Zip64 fields as they would
// be written), and comment fit their 16-bit lengths in both headers.
static b32 zip_fits(zentry const *e)
{
    return e->name.len<=ZIP_MAX16 && e->comment.len<=ZIP_MAX16 &&
           e->lextra.len+(e->zip64 ? 20 : 0)<=ZIP_MAX16 &&
           e->cextra.len+zip_central64_len(e)<=ZIP_MAX16;
}

static b32 zip_end_needs64(i64 count, i64 cdsize, i64 cdoff)
{
    return count>=ZIP_MAX16 || cdsize>=ZIP_MAX32 || cdoff>=ZIP_MAX32;
}

static iz zip_end_len(i64 count, i64 cdsize, i64 cdoff, s8 comment)
{
    b32 z64 = zip_end_needs64(count, cdsize, cdoff);
    return (z64 ? ZIP_END64_LEN+ZIP_LOC64_LEN : 0) + ZIP_END_LEN + comment.len;
}

// Encode the end records for a central directory of count entries and
// cdsize bytes at cdoff: Zip64 records when anything overflows, then the
// end of central directory record. The Zip64 record gives the program's
// version made by, as its entries do.
static u8 *zip_end(u8 *p, i64 count, i64 cdsize, i64 cdoff, s8 comment,
                   u16 made)
{
    if (zip_end_needs64(count, cdsize, cdoff)) {
        p = put32(p, ZIP_END64_SIG);
        p = put64(p, ZIP_END64_LEN - 12);
        p = put16(p, made);
        p = put16(p, 45);  // version needed
        p = put32(p, 0);   // this disk
        p = put32(p, 0);   // disk with the central directory
        p = put64(p, (u64)count);
        p = put64(p, (u64)count);
        p = put64(p, (u64)cdsize);
        p = put64(p, (u64)cdoff);

        p = put32(p, ZIP_LOC64_SIG);
        p = put32(p, 0);   // disk with the Zip64 end record
        p = put64(p, (u64)(cdoff + cdsize));
        p = put32(p, 1);   // total disks
    }
    u32 n16 = count<ZIP_MAX16 ? (u32)count : ZIP_MAX16;
    p = put32(p, ZIP_END_SIG);
    p = put16(p, 0);
    p = put16(p, 0);
    p = put16(p, n16);
    p = put16(p, n16);
    p = put32(p, min32(cdsize));
    p = put32(p, min32(cdoff));
    p = put16(p, (u32)comment.len);
    return putbytes(p, comment);
}

// Whether the end record's fields defer to a Zip64 end record.
static b32 zip_end_saturated(zend const *e)
{
    return e->count==ZIP_MAX16 || e->ndisk==ZIP_MAX16 ||
           e->disk==ZIP_MAX16 || e->cddisk==ZIP_MAX16 ||
           e->cdsize==ZIP_MAX32 || e->cdoff==ZIP_MAX32;
}

// Whether, by the end record, the central directory ends at it, so that
// there is no room for Zip64 records between, and bytes resembling them
// are by chance (the end of the last entry's comment).
static b32 zip_end_reached(zend const *e)
{
    return e->cdoff+e->cdsize == e->endpos;
}

// Validate the end of central directory record on its own. As Info-ZIP
// does, a split archive is known by its disk numbers, not by a count of
// entries on this disk that differs from the total, which, on a single
// disk, some writers get wrong (where Info-ZIP and UnZip read it).
static i32 zip_check_end32(zend *e)
{
    e->end64 = -1;
    if (e->disk || e->cddisk) {
        return ZIP_EMULTI;
    } else if (!zip_end_reached(e)) {
        return e->cdoff+e->cdsize<e->endpos ? ZIP_EPREFIX : ZIP_EFORMAT;
    }
    return e->count<=e->cdsize/ZIP_CENTRAL_LEN ? ZIP_OK : ZIP_EFORMAT;
}

// Find the end of central directory record among the final n bytes of an
// archive of the given total size. Following it, if e->end64 is not -1,
// read ZIP_END64_LEN bytes there and pass them to zip_parse_end64.
//
// Bytes resembling a Zip64 locator may precede the end record by chance,
// such as in an entry comment, so a Zip64 record is relied upon only if
// it checks out or the end record's fields call for one, or leave room
// for one: then its own faults are reported, not the end record's.
static i32 zip_find_end(u8 *tail, iz n, i64 size, zend *e)
{
    i64 base = size - n;
    for (iz i = n-ZIP_END_LEN; i >= 0; i--) {
        u8 *p = tail + i;
        if (get32(p) != ZIP_END_SIG) {
            continue;
        }
        iz clen = get16(p+20);
        if (clen > n-i-ZIP_END_LEN) {
            continue;  // a stray signature, perhaps within the comment
        }

        e->endpos  = base + i;
        e->comment = (s8){p+ZIP_END_LEN, clen};
        e->disk    = get16(p+4);
        e->cddisk  = get16(p+6);
        e->ndisk   = get16(p+8);
        e->count   = get16(p+10);
        e->cdsize  = get32(p+12);
        e->cdoff   = get32(p+16);
        e->end64   = -1;
        if (!e->count && !e->cdsize && !e->cdoff) {
            // An empty central directory at offset 0, as Info-ZIP writes
            // one wherever it is, as after an emptied self-extractor's
            // stub, and UnZip takes for an empty archive: it is here, and
            // whatever precedes it is a preamble
            e->cdoff = e->endpos;
        }

        if (i>=ZIP_LOC64_LEN && get32(p-ZIP_LOC64_LEN)==ZIP_LOC64_SIG) {
            // On another disk, or one of other than one disk: split
            u8 *loc   = p - ZIP_LOC64_LEN;
            u64 off   = get64(loc+8);
            b32 split = get32(loc+4) || get32(loc+16)!=1;
            b32 ok    = !split && e->endpos>=ZIP_LOC64_LEN+ZIP_END64_LEN &&
                        off<=(u64)(e->endpos-ZIP_LOC64_LEN-ZIP_END64_LEN);
            if (ok) {
                e->end64 = (i64)off;
                return ZIP_OK;
            } else if (zip_end_saturated(e) || !zip_end_reached(e)) {
                return split ? ZIP_EMULTI : ZIP_EFORMAT;
            }
        }
        return zip_check_end32(e);
    }
    return ZIP_ENOEND;
}

static i32 zip_check_end64(u8 const *p, zend *e)
{
    if (get32(p) != ZIP_END64_SIG) {
        return ZIP_EFORMAT;
    }
    u64 recsize = get64(p+4);
    u64 total   = get64(p+32);
    u64 cdsize  = get64(p+40);
    u64 cdoff   = get64(p+48);
    if (get32(p+20)) {
        // As in Info-ZIP, only the disk with the central directory tells,
        // not this disk's number or its count of entries
        return ZIP_EMULTI;
    }

    // The record must sit just before the locator, by its size, and the
    // central directory just before it. Data prepended without adjusting
    // offsets would show here only if the locator had been adjusted: if
    // not, the locator points short of the record, at no record.
    u64 limit = (u64)e->endpos;
    if (recsize<ZIP_END64_LEN-12 || recsize>limit ||
        e->endpos-ZIP_LOC64_LEN-12-(i64)recsize != e->end64) {
        return ZIP_EFORMAT;
    }
    if (cdsize>limit || cdoff>limit || cdoff+cdsize!=(u64)e->end64) {
        b32 prefix = cdoff+cdsize < (u64)e->end64;
        if (prefix && cdsize<=limit && cdoff<=limit) {
            // Kept, as zip_check_end32 keeps them, so that a reader may
            // shift the offsets by the data before the archive, as UnZip
            // does: then the central directory ends at the record
            e->count  = (i64)total;
            e->cdsize = (i64)cdsize;
            e->cdoff  = (i64)cdoff;
        }
        return prefix ? ZIP_EPREFIX : ZIP_EFORMAT;
    }
    if (total > cdsize/ZIP_CENTRAL_LEN) {
        return ZIP_EFORMAT;
    }
    e->count  = (i64)total;
    e->cdsize = (i64)cdsize;
    e->cdoff  = (i64)cdoff;
    return ZIP_OK;
}

// Parse the Zip64 end record, ZIP_END64_LEN bytes at e->end64, falling
// back to the end record alone when that suffices: when its fields do
// not defer to Zip64 records, and leave no room for them.
static i32 zip_parse_end64(u8 const *p, zend *e)
{
    zend end = *e;  // as the end record has it
    i32  r   = zip_check_end64(p, e);
    b32  own = r==ZIP_OK || zip_end_saturated(&end) || !zip_end_reached(&end);
    if (!own) {
        *e = end;
        r  = zip_check_end32(e);
    }
    return r;
}

// Copy a kept entry's extra fields, dropping only Zip64, which is
// regenerated as needed. Like Info-ZIP, -X does not apply to kept
// entries, some of which need their fields, such as AES encryption's
// (0x9901). Malformed trailing data is dropped.
static s8 zip_filter_extra(arena *a, s8 x)
{
    s8 r = {newstr(a, x.len), 0};
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        if (len > x.len-i-4) {
            break;
        }
        if (id != ZIP_EXTRA_ZIP64) {
            bytecopy(r.s+r.len, x.s+i, 4+len);
            r.len += 4 + len;
        }
        i += 4 + len;
    }
    return r;
}

// The modification time among extra fields as Info-ZIP's -u and -f
// compare it (its ef_scan_ut_time), unsigned as it reads it: that of the
// last extended timestamp ("UT") field, if it has one, else of an old
// Unix ("UX") field, unless after a newer Unix ("Ux") field, which has
// none, or a UT field. Returns false if there is none.
static b32 zip_extra_mtime(s8 x, i64 *t)
{
    b32 have  = 0;
    b32 newer = 0;  // a UT or Ux field, over any UX field
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        u8 *d   = x.s + i + 4;
        if (len > x.len-i-4) {
            break;
        }
        if (id == ZIP_EXTRA_TIME) {
            newer = 1;
            have  = len>=5 && (d[0] & 1);
            *t    = have ? get32(d+1) : *t;
        } else if (id==ZIP_EXTRA_UNIX2 && !newer) {
            newer = 1;
            have  = 0;
        } else if (id==ZIP_EXTRA_UNIX1 && !newer && len>=8) {
            have = 1;
            *t   = get32(d+4);  // after the access time
        }
        i += 4 + len;
    }
    return have;
}

// The data of the first extra field with an ID, else a null string.
static s8 zip_extra_field(s8 x, u32 id)
{
    for (iz i = 0; x.len-i >= 4;) {
        iz len = get16(x.s+i+2);
        if (len > x.len-i-4) {
            break;
        } else if (get16(x.s+i) == id) {
            return (s8){x.s+i+4, len};
        }
        i += 4 + len;
    }
    return (s8){0};
}

// The UTF-8 name in an Info-ZIP Unicode path field among extra fields,
// if, as Info-ZIP and UnZip check, its version is at most 1 and it gives
// crc as the stored name's CRC-32, else a null string. An empty name
// means the stored one is UTF-8.
static s8 zip_extra_upath(s8 x, u32 crc)
{
    s8  f  = zip_extra_field(x, ZIP_EXTRA_UPATH);
    b32 ok = f.len>=5 && f.s[0]<=1 && get32(f.s+1)==crc;
    return ok ? (s8){f.s+5, f.len-5} : (s8){0};
}

// Whether extra fields hold a Unicode path field that Info-ZIP reads but
// finds stale, the stored name having changed since it was made: of a
// version at most 1, without crc, the stored name's CRC-32, or too short
// to hold one.
static b32 zip_extra_upath_stale(s8 x, u32 crc)
{
    s8 f = zip_extra_field(x, ZIP_EXTRA_UPATH);
    return f.s && (!f.len || f.s[0]<=1) && (f.len<5 || get32(f.s+1)!=crc);
}

// Whether a name, not flagged UTF-8, is in an OEM (IBM PC) code page,
// as Info-ZIP's Windows port judges by the host that made it: MS-DOS,
// except PKZIP 2.5, 2.6, and 4.0 for Windows (known by attributes beyond
// DOS's), which use the ANSI code page, OS/2, and WinZip's NTFS 5.0.
static b32 zip_oem_name(zentry const *e)
{
    u32 host  = e->made >> 8;
    u32 ver   = e->made & 0xff;
    b32 pkwin = (e->extattr>>16) && (ver==25 || ver==26 || ver==40);
    return !(e->flags & ZIP_FLAG_UTF8) &&
           ((host==0 && !pkwin) || host==6 || (host==11 && ver==50));
}

// Apply a central header's Zip64 extra field to the entry. Returns false
// if a needed field is missing. Without that extra, saturated sizes and
// offset are literal, as Info-ZIP, which uses Zip64 only past 0xffffffff,
// writes a file of exactly 4 GiB - 1 bytes, and as UnZip, Python, and
// Info-ZIP read it; the bounds checks that follow catch a bogus offset.
// So are saturated sizes beside an extra that holds just a saturated
// offset, as Info-ZIP writes such a file past 4 GiB: APPNOTE would have
// the sizes first, and UnZip and Info-ZIP itself take the offset for
// the uncompressed size, while Python refuses it. A saturated disk
// number still needs the extra.
static b32 zip_apply64(zentry *e, s8 x, b32 diskmax)
{
    b32 want[3] = {e->usize==ZIP_MAX32, e->csize==ZIP_MAX32,
                   e->offset==ZIP_MAX32};
    if (!want[0] && !want[1] && !want[2] && !diskmax) {
        return 1;
    }
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        if (len > x.len-i-4) {
            break;
        }
        if (id == ZIP_EXTRA_ZIP64) {
            if (want[2] && len==8 && !diskmax) {
                want[0] = want[1] = 0;  // only the offset: Info-ZIP's
            }
            u8 *f = x.s + i + 4;
            i64 *fields[3] = {&e->usize, &e->csize, &e->offset};
            for (i32 k = 0; k < 3; k++) {
                if (!want[k]) {
                    continue;
                }
                if (len < 8) {
                    return 0;
                }
                u64 v = get64(f);
                if (v >> 63) {
                    return 0;
                }
                *fields[k] = (i64)v;
                f += 8;
                len -= 8;
            }
            return !diskmax || (len>=4 && !get32(f));
        }
        i += 4 + len;
    }
    return !diskmax;
}

// Length of a central header's name, extra fields, and comment, or -1 if
// the fixed part, ZIP_CENTRAL_LEN bytes, is not a central header.
static iz zip_central_varlen(u8 const *h)
{
    if (get32(h) != ZIP_CENTRAL_SIG) {
        return -1;
    }
    return (iz)get16(h+28) + (iz)get16(h+30) + (iz)get16(h+32);
}

// Parse the central header beginning the n bytes at h, for an entry whose
// data lies before cdoff. Its name, extra fields (with Zip64, unfiltered),
// and comment point into h. Returns its length, or 0 if malformed.
static iz zip_parse_header(u8 *h, iz n, i64 cdoff, zentry *e)
{
    iz var = n<ZIP_CENTRAL_LEN ? -1 : zip_central_varlen(h);
    if (var<0 || var>n-ZIP_CENTRAL_LEN) {
        return 0;
    }
    iz nlen = get16(h+28);
    iz xlen = get16(h+30);
    iz clen = get16(h+32);

    *e = (zentry){0};
    e->made    = (u16)get16(h+4);
    e->needed  = (u16)get16(h+6);
    e->flags   = (u16)get16(h+8);
    e->method  = (u16)get16(h+10);
    e->dostime = get16(h+14)<<16 | get16(h+12);
    e->crc     = get32(h+16);
    e->csize   = get32(h+20);
    e->usize   = get32(h+24);
    e->intattr = (u16)get16(h+36);
    e->extattr = get32(h+38);
    e->offset  = get32(h+42);
    e->name    = (s8){h+ZIP_CENTRAL_LEN, nlen};
    e->cextra  = (s8){h+ZIP_CENTRAL_LEN+nlen, xlen};
    e->comment = (s8){h+ZIP_CENTRAL_LEN+nlen+xlen, clen};

    u32 disk = get16(h+34);
    if ((disk && disk!=ZIP_MAX16) ||
        !zip_apply64(e, e->cextra, disk==ZIP_MAX16)) {
        return 0;
    }
    if (e->offset>cdoff-ZIP_LOCAL_LEN || e->csize>cdoff-e->offset) {
        return 0;
    }
    return ZIP_CENTRAL_LEN + var;
}

// Parse a central directory of n bytes, expected to hold count entries
// whose data lies before cdoff. Names and comments point into p, and
// extra fields, without Zip64, are copied into a. Returns null if
// malformed. (The zip program reads a header at a time instead.)
[[maybe_unused]]
static zentry *zip_parse_central(u8 *p, iz n, i64 count, i64 cdoff,
                                 arena *a)
{
    if (count > n/ZIP_CENTRAL_LEN) {
        return 0;
    }
    zentry *entries = new(a, (iz)count, zentry);
    iz off = 0;
    for (i64 i = 0; i < count; i++) {
        zentry *e   = entries + i;
        iz      len = zip_parse_header(p+off, n-off, cdoff, e);
        if (!len) {
            return 0;
        }
        e->cextra = zip_filter_extra(a, e->cextra);
        off += len;
    }
    return off==n ? entries : 0;
}

// Length of a local header's name and extra fields, or -1 if the fixed
// part, ZIP_LOCAL_LEN bytes, is not a local header.
static iz zip_local_varlen(u8 const *h)
{
    if (get32(h) != ZIP_LOCAL_SIG) {
        return -1;
    }
    return (iz)get16(h+26) + (iz)get16(h+28);
}

// Pack a broken-down time {year, month 1-12, day, hour, minute, second}
// as an MS-DOS date<<16 | time, clamped to its range, 1980 to 2107.
static u32 zip_dostime(i32 const t[6])
{
    if (t[0] < 1980) {
        return (0<<9 | 1<<5 | 1) << 16;
    } else if (t[0] > 2107) {
        return (127u<<9 | 12<<5 | 31) << 16 | (23<<11 | 59<<5 | 29);
    }
    u32 date = (u32)(t[0]-1980)<<9 | (u32)t[1]<<5 | (u32)t[2];
    u32 time = (u32)t[3]<<11 | (u32)t[4]<<5 | (u32)t[5]>>1;
    return date<<16 | time;
}

// Broken-down UTC time from Unix seconds (proleptic Gregorian calendar).
// Years beyond the range of i32 saturate, so zip_dostime clamps them.
static void zip_gmtime(i64 t, i32 tm[6])
{
    i64 days = t / 86400;
    i64 secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    // Howard Hinnant's civil_from_days
    days += 719468;
    i64 era = (days >= 0 ? days : days-146096) / 146097;
    i64 doe = days - era*146097;
    i64 yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    i64 doy = doe - (365*yoe + yoe/4 - yoe/100);
    i64 mp  = (5*doy + 2) / 153;
    i64 d   = doy - (153*mp + 2)/5 + 1;
    i64 m   = mp<10 ? mp+3 : mp-9;
    i64 y   = yoe + era*400 + (m <= 2);
    tm[0] = (i32)MAX(MIN(y, 0x7fffffff), -0x7fffffff-1);
    tm[1] = (i32)m;
    tm[2] = (i32)d;
    tm[3] = (i32)(secs / 3600);
    tm[4] = (i32)(secs / 60 % 60);
    tm[5] = (i32)(secs % 60);
}

// 0 for ASCII, 1 for valid UTF-8 with non-ASCII, or -1 if not UTF-8
// (including WTF-8 surrogates).
static i32 zip_utf8(s8 s)
{
    i32 r = 0;
    for (iz i = 0; i < s.len;) {
        u32 c = s.s[i++];
        if (c < 0x80) {
            continue;
        }
        r = 1;
        i32 n   = c>=0xc2 && c<0xe0 ? 1 : c>=0xe0 && c<0xf0 ? 2 :
                  c>=0xf0 && c<0xf5 ? 3 : 0;
        u32 min = n==3 ? 0x10000 : n==2 ? 0x800 : 0x80;
        if (!n || n>s.len-i) {
            return -1;
        }
        c &= 0x3f >> n;
        for (i32 k = 0; k < n; k++, i++) {
            if ((s.s[i] & 0xc0) != 0x80) {
                return -1;
            }
            c = c<<6 | (s.s[i] & 0x3f);
        }
        if (c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) {
            return -1;
        }
    }
    return r;
}

static b32 zip_sep(u8 c, b32 windows)
{
    return c=='/' || (windows && c=='\\');
}

// Length of a UNC prefix "//server/share/" to drop from a name, as
// Info-ZIP does on Unix too, or zero. Without its final separator,
// "//server/share" is kept, though a directory, such as a share root,
// is named with one. On Windows either separator counts, a
// device path "//?/X:/" counts as server "?" and share "X:", and
// "//?/UNC/server/share/" as a whole.
static iz zip_unc(s8 p, b32 windows)
{
    if (p.len<3 || !zip_sep(p.s[0], windows) ||
        !zip_sep(p.s[1], windows) || zip_sep(p.s[2], windows)) {
        return 0;
    }
    iz drop = 0;
    iz i    = 2;
    for (i32 k = 0, parts = 2; k < parts; k++) {
        iz beg = i;
        for (; i<p.len && !zip_sep(p.s[i], windows); i++) {}
        if (i++ == p.len) {
            break;
        }
        b32 dev = windows && k==1 && beg==4 &&
                  (p.s[2]=='?' || p.s[2]=='.');
        if (dev && i-beg==4 && (p.s[beg]|32)=='u' &&
            (p.s[beg+1]|32)=='n' && (p.s[beg+2]|32)=='c') {
            parts = 4;
        }
        drop = k&1 ? i : drop;
    }
    return drop;
}

// Archive name for a path: backslashes become slashes on Windows; a
// drive (Windows) or UNC prefix, leading slashes, and leading ./
// components are dropped; and doubled slashes collapse. Like Info-ZIP,
// ../ components are kept.
static s8 zip_name(arena *a, s8 path, b32 windows)
{
    s8 r = {newstr(a, path.len), 0};
    iz i = 0;
    u8 drive = path.len>=2 && path.s[1]==':' ? (u8)(path.s[0] | 0x20) : 0;
    if (windows && drive>='a' && drive<='z') {
        i = 2;  // only a letter is a drive: "1:x" is a stream of file "1"
    } else {
        i = zip_unc(path, windows);
    }
    for (b32 lead = 1; i < path.len; i++) {
        u8 c = path.s[i];
        c = windows && c=='\\' ? '/' : c;
        if (c=='/' && (lead || (r.len && r.s[r.len-1]=='/'))) {
            continue;
        }
        if (lead && c=='.') {
            u8 n = i+1<path.len ? path.s[i+1] : '/';
            n = windows && n=='\\' ? '/' : n;
            if (n == '/') {
                i++;  // drop "./"
                continue;
            }
        }
        lead = 0;
        r.s[r.len++] = c;
    }
    return r;
}

// Info-ZIP's compression percentage, rounded, as its builds with 64-bit
// offsets compute it, exactly (its 256-divisor reduction for large sizes
// is disabled), which no real file size can overflow.
static i32 zip_percent(i64 n, i64 m)
{
    return n>m ? (i32)((1 + 200*(n - m)/n) / 2) : 0;
}
