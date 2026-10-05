// tugz ZIP format: headers, Zip64, archive parsing, names, wildcards
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
    ZIP_EXTRA_UNIX      = 0x7875,  // "ux": Unix UID and GID
};

// Results of parsing an archive's end records.
enum {
    ZIP_OK,
    ZIP_ENOEND,   // no end of central directory record
    ZIP_EMULTI,   // split or spanned archive
    ZIP_EPREFIX,  // data precedes the archive (self-extractor)
    ZIP_EFORMAT,  // inconsistent structure
};

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
    b32 zip64;    // the local header carries a Zip64 extra
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

static iz zip_central64_len(zentry const *e)
{
    iz n = (e->usize >=ZIP_MAX32 ? 8 : 0) + (e->csize>=ZIP_MAX32 ? 8 : 0) +
           (e->offset>=ZIP_MAX32 ? 8 : 0);
    return n ? 4+n : 0;
}

static iz zip_central_len(zentry const *e)
{
    return ZIP_CENTRAL_LEN + e->name.len + zip_central64_len(e) +
           e->cextra.len + e->comment.len;
}

// Encode a central directory header, zip_central_len bytes. Its Zip64
// extra holds only the fields that overflow.
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
    p = put32(p, min32(e->csize));
    p = put32(p, min32(e->usize));
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
        if (e->usize >= ZIP_MAX32) {
            p = put64(p, (u64)e->usize);
        }
        if (e->csize >= ZIP_MAX32) {
            p = put64(p, (u64)e->csize);
        }
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

// Validate the end of central directory record on its own.
static i32 zip_check_end32(zend *e)
{
    e->end64 = -1;
    if (e->disk || e->cddisk || e->ndisk!=e->count) {
        return ZIP_EMULTI;
    } else if (e->cdoff+e->cdsize != e->endpos) {
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
// it checks out or the end record's fields call for one.
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

        if (i>=ZIP_LOC64_LEN && get32(p-ZIP_LOC64_LEN)==ZIP_LOC64_SIG) {
            u8 *loc = p - ZIP_LOC64_LEN;
            u64 off = get64(loc+8);
            b32 ok  = !get32(loc+4) && get32(loc+16)==1 &&
                      e->endpos>=ZIP_LOC64_LEN+ZIP_END64_LEN &&
                      off<=(u64)(e->endpos - ZIP_LOC64_LEN - ZIP_END64_LEN);
            if (ok) {
                e->end64 = (i64)off;
                return ZIP_OK;
            } else if (zip_end_saturated(e)) {
                return get32(loc+16)!=1 ? ZIP_EMULTI : ZIP_EFORMAT;
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
    u64 count   = get64(p+24);
    u64 total   = get64(p+32);
    u64 cdsize  = get64(p+40);
    u64 cdoff   = get64(p+48);
    if (get32(p+16) || get32(p+20) || count!=total) {
        return ZIP_EMULTI;
    }

    // The record must sit just before the locator, and the central
    // directory just before it.
    u64 limit = (u64)e->endpos;
    if (recsize<ZIP_END64_LEN-12 || recsize>limit) {
        return ZIP_EFORMAT;
    }
    i64 actual = e->endpos - ZIP_LOC64_LEN - 12 - (i64)recsize;
    if (actual != e->end64) {
        return actual > e->end64 ? ZIP_EPREFIX : ZIP_EFORMAT;
    }
    if (cdsize>limit || cdoff>limit || cdoff+cdsize!=(u64)e->end64) {
        b32 prefix = cdoff+cdsize < (u64)e->end64;
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
// back to the end record alone when that suffices.
static i32 zip_parse_end64(u8 const *p, zend *e)
{
    i32 r = zip_check_end64(p, e);
    return r==ZIP_OK || zip_end_saturated(e) ? r : zip_check_end32(e);
}

// Copy a kept entry's extra fields, dropping only Zip64, which is
// regenerated as needed. Like Info-ZIP, -X does not apply to kept
// entries, some of which need their fields, such as AES encryption's
// (0x9901). Malformed trailing data is dropped.
static s8 zip_filter_extra(arena *a, s8 x)
{
    s8 r = {newbytes(a, x.len), 0};
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

// The modification time in an extended timestamp ("UT") field among
// extra fields, as Info-ZIP's -u and -f compare it, unsigned as it reads
// it. Returns false if there is none.
static b32 zip_extra_mtime(s8 x, i64 *t)
{
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        if (len > x.len-i-4) {
            break;
        }
        if (id==ZIP_EXTRA_TIME && len>=5 && (x.s[i+4] & 1)) {
            *t = get32(x.s+i+5);
            return 1;
        }
        i += 4 + len;
    }
    return 0;
}

// Apply a central header's Zip64 extra field to the entry. Returns false
// if a needed field is missing.
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
    return 0;
}

// Parse a central directory of n bytes, expected to hold count entries
// whose data lies before cdoff. Names, extras, and comments point into p.
// Returns null if malformed.
static zentry *zip_parse_central(u8 *p, iz n, i64 count, i64 cdoff,
                                 arena *a)
{
    if (count > n/ZIP_CENTRAL_LEN) {
        return 0;
    }
    zentry *entries = new(a, (iz)count, zentry);
    iz off = 0;
    for (i64 i = 0; i < count; i++) {
        if (n-off<ZIP_CENTRAL_LEN || get32(p+off)!=ZIP_CENTRAL_SIG) {
            return 0;
        }
        u8 *h    = p + off;
        iz  nlen = get16(h+28);
        iz  xlen = get16(h+30);
        iz  clen = get16(h+32);
        if (nlen+xlen+clen > n-off-ZIP_CENTRAL_LEN) {
            return 0;
        }

        zentry *e  = entries + i;
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
        e->cextra = zip_filter_extra(a, e->cextra);
        off += ZIP_CENTRAL_LEN + nlen + xlen + clen;
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
    tm[0] = (i32)(yoe + era*400 + (m <= 2));
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

enum {
    ZIP_SETS   = 1 << 0,  // [sets] and backslash escapes, as on Unix
    ZIP_FOLD   = 1 << 1,  // ASCII case-insensitive, for Windows file names
    ZIP_NOWILD = 1 << 2,  // only ? is a wildcard, as with Info-ZIP's -nw
    ZIP_DOS    = 1 << 3,  // a name without a period ends in one (Windows)
};

static u8 zip_fold(u8 c, i32 flags)
{
    return (flags & ZIP_FOLD) && c>='A' && c<='Z' ? c+('a'-'A') : c;
}

static b32 zip_has(s8 s, u8 c)
{
    for (iz i = 0; i < s.len; i++) {
        if (s.s[i] == c) {
            return 1;
        }
    }
    return 0;
}

// Byte i of a name, past whose end lies ZIP_DOS's implicit period.
static u8 zip_at(s8 s, iz i)
{
    return i<s.len ? s.s[i] : '.';
}

// Whether c is in the set between pat[beg] and pat[end], the bytes
// within the brackets, as Info-ZIP reads one: a backslash escapes, a
// leading - is literal, any other - makes the byte before it the start
// of a range ending at the byte after it, and a byte followed by - is
// only a range start (so [a-] and the a in [a-b-c] match nothing). An
// end of 0xff wraps Info-ZIP's range loop around, matching any byte.
static b32 zip_inset(s8 pat, iz beg, iz end, u8 c, i32 flags)
{
    u8 other = c;
    if (flags & ZIP_FOLD) {
        other = c>='a' && c<='z' ? c-('a'-'A') : zip_fold(c, flags);
    }
    i32 lo  = -1;  // pending range start
    b32 esc = beg<end && pat.s[beg]=='-';
    for (iz k = beg; k < end; k++) {
        u8 b = pat.s[k];
        if (!esc && b=='\\') {
            esc = 1;
        } else if (!esc && b=='-') {
            lo = pat.s[k-1];
        } else {
            if (pat.s[k+1] != '-') {
                u8 l = lo<0 ? b : (u8)lo;
                if (b==0xff || (c>=l && c<=b) || (other>=l && other<=b)) {
                    return 1;
                }
            }
            lo  = -1;
            esc = 0;
        }
    }
    return 0;
}

// Whether a pattern has no wildcards from pat[p] on, by Info-ZIP's
// isshexp, which skips escaped bytes and counts [ even without sets.
static b32 zip_literal(s8 pat, iz p, i32 flags)
{
    for (; p < pat.len; p++) {
        u8 c = pat.s[p];
        if (c=='\\' && (flags & ZIP_SETS) && p+1<pat.len) {
            p++;
        } else if (c=='*' || c=='?' || c=='[') {
            return 0;
        }
    }
    return 1;
}

// Info-ZIP's wildcard match (recmatch) over whole names: * matches any
// run of bytes, including slashes, and ? any single byte. With ZIP_SETS,
// [abc], [a-z], and [!x] or [^x] match a set, and backslash escapes the
// next byte; otherwise brackets and backslashes are literal. ZIP_NOWILD
// keeps only ?, and ZIP_DOS matches a name without a period as if it
// ended in one when the pattern has one, so that *.* matches every name.
//
// Info-ZIP's quirks are kept: an unclosed set or a trailing backslash
// matches nothing; a trailing ** needs at least one more byte; and once
// a * is followed by no more wildcards, the rest of the pattern is
// compared with the end of the name byte for byte, escapes included.
static b32 zip_match(s8 pat, s8 s, i32 flags)
{
    b32 wild = !(flags & ZIP_NOWILD);
    b32 sets = wild && (flags & ZIP_SETS);
    b32 dot  = (flags & ZIP_DOS) && zip_has(pat, '.') && !zip_has(s, '.');
    iz  n    = s.len + dot;
    iz p = 0, i = 0;
    iz star = -1, mark = 0;
    while (i < n) {
        if (p < pat.len) {
            u8 c  = pat.s[p];
            u8 sc = zip_at(s, i);
            if (c=='*' && wild) {
                if (zip_literal(pat, ++p, flags)) {
                    iz tail = pat.len - p;
                    if (tail > n-i) {
                        return 0;
                    }
                    for (iz k = 0; k < tail; k++) {
                        u8 a = zip_fold(pat.s[p+k], flags);
                        if (a != zip_fold(zip_at(s, n-tail+k), flags)) {
                            return 0;
                        }
                    }
                    return 1;
                }
                star = p;
                mark = i;
                continue;
            } else if (c == '?') {
                p++;
                i++;
                continue;
            } else if (c=='[' && sets) {
                iz  q   = p + 1;
                b32 neg = q<pat.len && (pat.s[q]=='!' || pat.s[q]=='^');
                q += neg;
                iz end = q;  // the first unescaped ]
                for (b32 e = 0; end<pat.len && (e || pat.s[end]!=']'); end++) {
                    e = !e && pat.s[end]=='\\';
                }
                if (end == pat.len) {
                    return 0;  // unclosed: nothing matches
                }
                if (zip_inset(pat, q, end, sc, flags) != neg) {
                    p = end + 1;
                    i++;
                    continue;
                }
            } else {
                if (c=='\\' && sets) {
                    if (++p == pat.len) {
                        return 0;  // nothing to escape: nothing matches
                    }
                    c = pat.s[p];
                }
                if (zip_fold(c, flags) == zip_fold(sc, flags)) {
                    p++;
                    i++;
                    continue;
                }
            }
        }
        if (star < 0) {
            return 0;
        }
        p = star;
        i = ++mark;
    }
    iz stars = 0;
    for (; wild && p<pat.len && pat.s[p]=='*'; p++, stars++) {}
    return p==pat.len && stars<2;
}

static b32 zip_haswild(s8 s, i32 flags)
{
    for (iz i = 0; i < s.len; i++) {
        if (s.s[i]=='*' || s.s[i]=='?' || ((flags & ZIP_SETS) && s.s[i]=='[')) {
            return 1;
        }
    }
    return 0;
}

static b32 zip_sep(u8 c, b32 windows)
{
    return c=='/' || (windows && c=='\\');
}

// Length of a UNC prefix "//server/share/" to drop from a name, as
// Info-ZIP does on Unix too, or zero. Without its final separator,
// "//server/share" is kept. On Windows either separator counts, a
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
    s8 r = {newbytes(a, path.len), 0};
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

// Info-ZIP's compression percentage, rounded.
static i32 zip_percent(i64 n, i64 m)
{
    while (n > 0xffffff) {
        n = (n + 0x80) >> 8;
        m = (m + 0x80) >> 8;
    }
    return n>m ? (i32)((1 + 200*(n - m)/n) / 2) : 0;
}
