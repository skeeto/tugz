// tugz zipinfo rules: ZipInfo mode's options, its listings' fields, and
// the text of its verbose report (-v)
//
// No I/O, as in unzip.c: the unzip program's ZipInfo mode (unzip -Z, or
// the program run by the name zipinfo) applies these. Each follows
// Info-ZIP UnZip 6.0's zipinfo.c (ZipInfo 3.00), named at each function,
// and the behavior of its builds as recorded in test/unziptests.c. The
// fields are short, each built in a zistr of its own; -v's longer text,
// a section or a subfield's description at a time, in a zitext.

typedef struct {
    u8 s[32];
    iz len;
} zistr;

static void zi_putc(zistr *z, u8 c)
{
    z->s[z->len++] = c;
}

static void zi_puts(zistr *z, char const *s)
{
    for (; *s; s++) {
        zi_putc(z, (u8)*s);
    }
}

// An unsigned number, as "%wu", or with zero padding, "%0wu".
static void zi_putu(zistr *z, u64 v, iz w, b32 zero)
{
    u8  tmp[20];
    iz  n = 0;
    do {
        tmp[n++] = (u8)('0' + v%10);
    } while (v /= 10);
    for (; w > n; w--) {
        zi_putc(z, zero ? '0' : ' ');
    }
    while (n) {
        zi_putc(z, tmp[--n]);
    }
}

static s8 zi_s8(zistr *z)
{
    return (s8){z->s, z->len};
}

// ZipInfo's options, as zi_opts keeps them (zipinfo.c)
typedef struct {
    i32 lflag;  // the format: 1 -1, 2 -2, 3 -s, 4 -m, 5 -l, 10 -v; or
                // -1 none yet, -2 one negated, 0 only -h or -t
    i32 neg;    // negations ('-') pending, carried into the next word
    b32 hslmv;  // the header for -s, -m, -l, and -v,
    b32 h2;     // and for -2 (and -h or -t alone)
    b32 tslm;   // the totals likewise,
    b32 t2v;    // for -2 and -v
    b32 exph;   // -h given
    b32 expt;   // -t given

    // Once resolved (zi_resolve)
    b32 hflag;  // the header lines
    b32 tflag;  // the totals line
    b32 zflag;  // the archive comment
    b32 Cflag;  // names matched ignoring case
    b32 Tflag;  // times in decimal
} ziopts;

static ziopts zi_new(void)
{
    ziopts z = {0};
    z.lflag = -1;
    z.hslmv = z.tslm = 1;
    return z;
}

// Results of zi_option
enum {
    ZI_OK,
    ZI_BAD,      // unknown: the usage follows (10)
    ZI_REFUSED,  // ZipInfo's, but not tugz's: -M (the pager), -U
};

// Apply an option letter as zi_opts does: '-' negates the letter after
// it, even in the next word; a negated format leaves none chosen; -h and
// -t alone choose the format of neither (0); Z does nothing.
static i32 zi_option(ziopts *z, u8 c)
{
    b32 neg  = z->neg > 0;
    i32 kept = z->neg;
    z->neg = 0;
    switch (c) {
    case '-':
        z->neg = kept + 1;
        return ZI_OK;
    case '1': case '2': case 's': case 'm': case 'l': case 'v':
        z->lflag = neg ? -2 : c=='1' ? 1 : c=='2' ? 2 : c=='s' ? 3 :
                   c=='m' ? 4 : c=='l' ? 5 : 10;
        return ZI_OK;
    case 'h':
        z->hslmv = z->h2 = !neg;
        z->exph |= !neg;
        z->lflag = !neg && z->lflag==-1 ? 0 : z->lflag;
        return ZI_OK;
    case 't':
        z->tslm = z->t2v = !neg;
        z->expt |= !neg;
        z->lflag = !neg && z->lflag==-1 ? 0 : z->lflag;
        return ZI_OK;
    case 'C':
        z->Cflag = !neg;
        return ZI_OK;
    case 'T':
        z->Tflag = !neg;
        return ZI_OK;
    case 'z':
        z->zflag = !neg;
        return ZI_OK;
    case 'Z':
        z->neg = kept;  // ignored, negations and all
        return ZI_OK;
    case 'M': case 'U':
        z->neg = kept;
        return ZI_REFUSED;
    }
    z->neg = kept;
    return ZI_BAD;
}

// Settle the format and which of the header, totals, and comment show,
// as zi_opts does once the options are read, given whether any word
// follows the archive: by default, and with members if only -h or -t
// was given, the short format (-s). Members turn off the header and
// totals of -s, -m, and -l unless they were asked for; -1 shows neither,
// nor the comment.
static void zi_resolve(ziopts *z, b32 members)
{
    if (z->lflag<0 || (members && z->lflag==0)) {
        z->lflag = 3;
    }
    switch (z->lflag) {
    case 0:
    case 2:
        z->hflag = z->h2;
        z->tflag = z->t2v;
        break;
    case 1:
        z->hflag = z->tflag = z->zflag = 0;
        break;
    case 3:
    case 4:
    case 5:
        z->hflag = members && !z->exph ? 0 : z->hslmv;
        z->tflag = members && !z->expt ? 0 : z->tslm;
        break;
    case 10:
        z->hflag = z->hslmv;
        z->tflag = z->t2v;
        break;
    }
}

static u8 zi_lower(u8 c)
{
    return c>='A' && c<='Z' ? (u8)(c + 32) : c;
}

// Whether s begins with prefix, ignoring ASCII case, as STRNICMP.
static b32 zi_prefix(s8 s, char const *prefix)
{
    iz i = 0;
    for (; prefix[i]; i++) {
        if (i>=s.len || zi_lower(s.s[i])!=(u8)prefix[i]) {
            return 0;
        }
    }
    return 1;
}

// Whether the program, by the name it was run as, is ZipInfo: a base
// name starting with "zipinfo" or "ii", in any case, as unzip.c finds it
// (so "ZipInfo.exe" too), after the last '/', or with Windows
// conventions, '/' or '\'.
static b32 zi_named(s8 argv0, b32 windows)
{
    iz beg = 0;
    for (iz i = 0; i < argv0.len; i++) {
        u8 c = argv0.s[i];
        beg = c=='/' || (windows && c=='\\') ? i+1 : beg;
    }
    s8 base = {argv0.s+beg, argv0.len-beg};
    return zi_prefix(base, "zipinfo") || zi_prefix(base, "ii");
}

// The host's three letters, by the upper byte of "version made by", as
// zi_short's os[], hosts past 30 taking the last, "???".
static s8 zi_host(u32 made)
{
    static char const hosts[UZ_HOSTS+1][4] = {
        "fat", "ami", "vms", "unx", "cms", "atr", "hpf", "mac", "zzz",
        "cpm", "t20", "ntf", "qds", "aco", "vft", "mvs", "be ", "nsk",
        "ths", "osx", "???", "???", "???", "???", "???", "???", "???",
        "???", "???", "???", "ath", "???",
    };
    return (s8){(u8 *)hosts[uz_host(made)], 3};
}

// Hosts with attributes of their own (zi_short)
enum {
    ZI_CMS   = 4,
    ZI_HPFS  = 6,
    ZI_NTFS  = 11,
    ZI_VFAT  = 14,
    ZI_MVS   = 15,
};

// An entry's attributes as zi_short shows them, from its external
// attributes, host, and name, with the version that made it: a field of
// 15, the version from column 12, as "-rw-r--r--  3.0":
// - VMS: R, W, E, D for owner, group, and other, from the Unix bits,
//   as "RWED,RWED,RE", the version only where they leave room.
// - Amiga: its type (d, -, ?), then hsparwed.
// - THEOS: its type, then a letter for each bit not set.
// - MS-DOS and the hosts with its attributes (OS/2 HPFS, NTFS, VM/CMS,
//   MVS, Acorn, VFAT): the low byte as d or - (V for a volume label),
//   r, w unless read-only, x for a directory or a name whose last '.'
//   is followed by com, exe, btm, cmd, or bat (in any case), a, h, s.
//   But from MS-DOS, Unix bits that agree with those, for the owner, as
//   an Info-ZIP zip for Unix writes them, show as from Unix.
// - Any other host, Unix's: its type (d - l b c p s, else ?), and rwx,
//   with set-ID (s, S) and sticky (t, T) bits.
static zistr zi_attribs(u32 made, u32 extattr, s8 name)
{
    zistr r     = {0};
    u32   host  = uz_host(made);
    u32   ver   = made & 0xff;
    u32   x     = extattr>>16 & 0xffff;
    u8    a[16] = "               ";
    iz    vms   = -1;  // where VMS's permissions end
    switch (host) {
    case UZ_VMS: {
        u8  w[12] = {0};
        u32 bits[] = {0400, 0200, 0100, 040, 020, 010, 04, 02, 01};
        for (i32 k = 0; k < 3; k++) {
            w[4*k+0] = x & bits[3*k+0] ? 'R' : 0;
            w[4*k+1] = x & bits[3*k+1] ? 'W' : 0;
            w[4*k+2] = x & bits[3*k+2] ? 'E' : 0;
            w[4*k+3] = x & bits[3*k+1] ? 'D' : 0;
        }
        iz p = 0;
        for (i32 k = 0; k < 12; k++) {
            if (w[k]) {
                a[p++] = w[k];
            }
            if (k%4 == 3) {
                a[p++] = ',';
            }
        }
        a[--p] = ' ';
        vms = p;
    } break;

    case UZ_AMIGA:
        a[0] = (x & 06000)==04000 ? 'd' : (x & 06000)==02000 ? '-' : '?';
        for (i32 k = 0; k < 8; k++) {
            a[1+k] = x & 0200u>>k ? (u8)"hsparwed"[k] : '-';
        }
        break;

    case UZ_THEOS: {
        static char const types[] = "??C?DL??SRKI?P23";
        a[0] = (u8)types[x>>12 & 15];
        u32 bits[] = {0x400, 0x800, 0x002, 0x004, 0x200, 0x040, 0x080, 0x100};
        for (i32 k = 0; k < 8; k++) {
            a[1+k] = x & bits[k] ? '.' : (u8)"HMWREXWR"[k];
        }
    } break;

    case UZ_FAT: case ZI_HPFS: case ZI_NTFS: case ZI_CMS: case ZI_MVS:
    case UZ_ACORN: case ZI_VFAT:
        if (host==UZ_FAT && (x & 0700)==(0400 | (u32)!(extattr & 1)<<7 |
                                        (extattr & 0x10)<<2)) {
            goto unixlike;  // as from Unix, by agreeing bits
        }
        x = extattr & 0xff;
        bytecopy(a, ".r.-...", 7);
        a[2] = x & 0x01 ? '-' : 'w';
        a[5] = x & 0x02 ? 'h' : '-';
        a[6] = x & 0x04 ? 's' : '-';
        a[4] = x & 0x20 ? 'a' : '-';
        a[0] = x & 0x10 ? 'd' : '-';
        a[3] = x & 0x10 ? 'x' : a[3];
        if (x & 0x08) {
            a[0] = 'V';
        } else {
            iz dot = -1;
            for (iz i = 0; i < name.len; i++) {
                dot = name.s[i]=='.' ? i : dot;
            }
            s8 ext = {name.s+dot+1, name.len-dot-1};
            if (dot>=0 && (zi_prefix(ext, "com") || zi_prefix(ext, "exe") ||
                           zi_prefix(ext, "btm") || zi_prefix(ext, "cmd") ||
                           zi_prefix(ext, "bat"))) {
                a[3] = 'x';
            }
        }
        break;

    default:
    unixlike:
        switch (x & 0170000) {
        case 0040000: a[0] = 'd'; break;
        case 0100000: a[0] = '-'; break;
        case 0120000: a[0] = 'l'; break;
        case 0060000: a[0] = 'b'; break;
        case 0020000: a[0] = 'c'; break;
        case 0010000: a[0] = 'p'; break;
        case 0140000: a[0] = 's'; break;
        default:      a[0] = '?';
        }
        for (i32 k = 0; k < 3; k++) {
            u32 m = x >> (6 - 3*k);
            a[1+3*k] = m & 4 ? 'r' : '-';
            a[2+3*k] = m & 2 ? 'w' : '-';
        }
        a[3] = x & 0100 ? (x & 04000 ? 's' : 'x') : (x & 04000 ? 'S' : '-');
        a[6] = x & 0010 ? (x & 02000 ? 's' : 'x') : (x & 02000 ? 'S' : '-');
        a[9] = x & 0001 ? (x & 01000 ? 't' : 'x') : (x & 01000 ? 'T' : '-');
    }

    // A version from 10.0 begins a column early, as UnZip's builds show
    // it with Unix's attributes (they abort with the others' instead,
    // their buffer overrun), and with VMS's only where those leave room
    iz at = ver<100 ? 12 : 11;
    if (vms < at) {
        bytecopy(r.s, a, at);
        r.len = at;
        zi_putu(&r, ver/10, 0, 0);
        zi_putc(&r, '.');
        zi_putu(&r, ver%10, 0, 0);
    } else {
        bytecopy(r.s, a, 15);
        r.len = 15;
    }
    return r;
}

// An entry's method as zi_short abbreviates it: "stor", "defN", "i8:3",
// or for one it does not know, "u" and its number, "u099" (UnZip 6.0
// overflows its buffer from u1000, where distributions' patches stop).
// Deflation's level and imploding's dictionary and trees come from
// flag bits 1 and 2.
static zistr zi_method(u32 method, u32 flags)
{
    static struct { u16 id; char name[5]; } const methods[] = {
        {0, "stor"}, {1, "shrk"}, {2, "re:1"}, {3, "re:2"}, {4, "re:3"},
        {5, "re:4"}, {6, "i#:#"}, {7, "tokn"}, {8, "def#"}, {9, "d64#"},
        {10, "dcli"}, {12, "bzp2"}, {14, "lzma"}, {18, "ters"},
        {19, "lz77"}, {97, "wavp"}, {98, "ppmd"},
    };
    zistr r = {0};
    for (i32 k = 0; k < countof(methods); k++) {
        if (methods[k].id == method) {
            zi_puts(&r, methods[k].name);
        }
    }
    if (!r.len) {
        zi_putc(&r, 'u');
        zi_putu(&r, method, 3, 1);
    } else if (method == 6) {
        r.s[1] = flags & 2 ? '8' : '4';
        r.s[3] = flags & 4 ? '3' : '2';
    } else if (method==8 || method==9) {
        r.s[3] = (u8)"NXFS"[flags>>1 & 3];
    }
    return r;
}

// The two characters after an entry's size (zi_short): t for text by
// its internal attributes, else b, in upper case if encrypted; then x
// if it has central extra fields (xlen bytes, Zip64's counted), or, from
// Unix, OS/2 HPFS, or NTFS, external attribute bit 15, the sign of an
// old Unix field kept only in the local header, else -; and in upper
// case, X, or for none, l, with a data descriptor (flag bit 3).
static zistr zi_flags(u32 flags, u32 intattr, u32 extattr, u32 made, iz xlen)
{
    zistr r    = {0};
    u32   host = uz_host(made);
    b32   enc  = flags & 1;
    b32   desc = flags & 8;
    b32   xf   = xlen || ((extattr & 0x8000) &&
                          (host==UZ_UNIX || host==ZI_HPFS || host==ZI_NTFS));
    zi_putc(&r, intattr & 1 ? (enc ? 'T' : 't') : (enc ? 'B' : 'b'));
    zi_putc(&r, xf ? (desc ? 'X' : 'x') : (desc ? 'l' : '-'));
    return r;
}

// An entry's time as zi_time shows it outside -v, from the fields
// {year, month, day, hour, minute, second}, a DOS time's as stored, or
// a local time: "yy-Mon-dd hh:mm", a month out of range as three digits
// ("80-000-00 00:00"); or with -T, "yyyymmdd.hhmmss", the month a
// number however large.
static zistr zi_date(i32 const tm[6], b32 decimal)
{
    static char const months[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    zistr r = {0};
    u32   f[6];
    for (i32 k = 0; k < 6; k++) {
        f[k] = (u32)tm[k];
    }
    if (decimal) {
        zi_putu(&r, f[0], 4, 1);
        zi_putu(&r, f[1], 2, 1);
        zi_putu(&r, f[2], 2, 1);
        zi_putc(&r, '.');
        zi_putu(&r, f[3], 2, 1);
        zi_putu(&r, f[4], 2, 1);
        zi_putu(&r, f[5], 2, 1);
        return r;
    }
    zi_putu(&r, f[0]%100, 2, 1);
    zi_putc(&r, '-');
    if (f[1]<1 || f[1]>12) {
        zi_putu(&r, f[1], 3, 1);
    } else {
        zi_puts(&r, months[f[1]-1]);
    }
    zi_putc(&r, '-');
    zi_putu(&r, f[2], 2, 1);
    zi_putc(&r, ' ');
    zi_putu(&r, f[3], 2, 1);
    zi_putc(&r, ':');
    zi_putu(&r, f[4], 2, 1);
    return r;
}

// A signed number, as "%wd".
static void zi_putd(zistr *z, i32 v, iz w)
{
    u32 m = v<0 ? -(u32)v : (u32)v;
    iz  n = 1;
    for (u32 t = m; t >= 10; t /= 10, n++) {}
    for (w -= n + (v<0); w > 0; w--) {
        zi_putc(z, ' ');
    }
    if (v < 0) {
        zi_putc(z, '-');
    }
    zi_putu(z, m, 0, 0);
}

// -m's percentage, from a compression factor in tenths of a percent
// (uz_ratio): "%3d%%" of factor+5 over ten, truncated as C divides
// ("-99%" for -99.5%).
static zistr zi_percent(i32 factor)
{
    zistr r = {0};
    zi_putd(&r, (factor + 5)/10, 3);
    zi_putc(&r, '%');
    return r;
}

// The totals line's factor: "%s%d.%d%%", the sign apart ("-50.0%").
static zistr zi_factor(i32 factor)
{
    zistr r = {0};
    u32   m = factor<0 ? -(u32)factor : (u32)factor;
    if (factor < 0) {
        zi_putc(&r, '-');
    }
    zi_putu(&r, m/10, 0, 0);
    zi_putc(&r, '.');
    zi_putu(&r, m%10, 0, 0);
    zi_putc(&r, '%');
    return r;
}

// -v, the verbose report (zi_end_central, zi_long)

// Text longer than a field: a section's lines, or a subfield's
// description, in room the caller gives, past which it is cut short
// (which nothing here reaches: each part below is bounded).
typedef struct {
    u8 *s;
    iz  len;
    iz  cap;
} zitext;

static zitext zt_new(arena *a, iz cap)
{
    zitext t = {0};
    t.s   = newstr(a, cap);
    t.cap = cap;
    return t;
}

static s8 zt_text(zitext *t)
{
    return (s8){t->s, t->len};
}

static void zt_putc(zitext *t, u8 c)
{
    if (t->len < t->cap) {
        t->s[t->len++] = c;
    }
}

static void zt_puts(zitext *t, char const *s)
{
    for (; *s; s++) {
        zt_putc(t, (u8)*s);
    }
}

static void zt_str(zitext *t, s8 s)
{
    for (iz i = 0; i < s.len; i++) {
        zt_putc(t, s.s[i]);
    }
}

// A number in a base, as "%wu", or zero-padded to w digits, as "%.wX",
// "%.wx", or "%0wo".
static void zt_num(zitext *t, u64 v, u32 base, iz w, b32 zero, b32 upper)
{
    char const *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    u8  tmp[24];
    iz  n = 0;
    do {
        tmp[n++] = (u8)digits[v%base];
    } while (v /= base);
    for (; w > n; w--) {
        zt_putc(t, zero ? '0' : ' ');
    }
    while (n) {
        zt_putc(t, tmp[--n]);
    }
}

static void zt_putu(zitext *t, u64 v)
{
    zt_num(t, v, 10, 0, 0, 0);
}

static void zt_hex(zitext *t, u64 v, iz w, b32 upper)
{
    zt_num(t, v, 16, w, 1, upper);
}

// "%lld"
static void zt_putd(zitext *t, i64 v)
{
    if (v < 0) {
        zt_putc(t, '-');
    }
    zt_putu(t, v<0 ? -(u64)v : (u64)v);
}

// A number in decimal and in hexadecimal, as FmZofft gives offsets and
// sizes: "%wllu (%.16llXh)".
static void zt_both(zitext *t, u64 v, iz w)
{
    zt_num(t, v, 10, w, 0, 0);
    zt_puts(t, " (");
    zt_hex(t, v, 16, 1);
    zt_puts(t, "h)");
}

// What -v's end-record section shows (zi_end_central): the file's size,
// where its end record lies, the Zip64 end record if used, and where the
// central directory says it should, and the end record's fields, as
// tugz takes them, from the 32-bit record or the Zip64 one.
typedef struct {
    i64 size;     // of the file, any data before the archive included
    i64 real;     // offset of the end record, or the Zip64 end record
    u64 cdoff;    // as recorded, not shifted
    u64 cdsize;
    u64 count;    // entries
    u64 ndisk;    // entries on this disk
    u32 disk;     // this disk's number
    u32 cddisk;   // the central directory's first
} ziend;

// The end-record section, after the comment's: the sizes and offsets in
// decimal and hexadecimal, then the central directory, in the words for
// a single-part archive, or for a disk number other than 0, a multi-part
// one. The expected offset wraps, as UnZip's sum does.
static void zi_end_text(zitext *t, ziend const *z)
{
    zt_puts(t, "\nEnd-of-central-directory record:\n"
               "-------------------------------\n\n"
               "  Zip archive file size:               ");
    zt_both(t, (u64)z->size, 11);
    zt_puts(t, "\n  Actual end-cent-dir record offset:   ");
    zt_both(t, (u64)z->real, 11);
    zt_puts(t, "\n  Expected end-cent-dir record offset: ");
    zt_both(t, z->cdoff + z->cdsize, 11);
    zt_puts(t, "\n  (based on the length of the central directory and its"
               " expected offset)\n\n");
    if (!z->disk) {
        zt_puts(t, "  This zipfile constitutes the sole disk of a single-part"
                   " archive; its\n  central directory contains ");
        zt_putu(t, z->count);
        zt_puts(t, z->count==1 ? " entry.\n" : " entries.\n");
        zt_puts(t, "  The central directory is ");
        zt_both(t, z->cdsize, 0);
        zt_puts(t, " bytes long,\n  and its (expected) offset in bytes from"
                   " the beginning of the zipfile\n  is ");
        zt_both(t, z->cdoff, 0);
        zt_puts(t, ".\n\n");
    } else {
        zt_puts(t, "  This zipfile constitutes disk ");
        zt_putu(t, (u64)z->disk + 1);
        zt_puts(t, " of a multi-part archive.  The central\n  directory"
                   " starts on disk ");
        zt_putu(t, (u64)z->cddisk + 1);
        zt_puts(t, " at an offset within that archive part\n  of ");
        zt_both(t, z->cdoff, 0);
        zt_puts(t, " bytes.  The entire\n  central directory is ");
        zt_both(t, z->cdsize, 0);
        zt_puts(t, " bytes long.\n  ");
        zt_putu(t, z->ndisk);
        zt_puts(t, z->ndisk==1 ? " of the archive entries is"
                               : " of the archive entries are");
        zt_puts(t, " contained within this zipfile volume,\n  out of a total"
                   " of ");
        zt_putu(t, z->count);
        zt_puts(t, z->count==1 ? " entry.\n\n" : " entries.\n\n");
    }
}

// The line before an entry's, by which zi_long tells of data between
// entries: "There are an extra N bytes preceding this file.", signed,
// where its offset is not where the entry before it ended, *endprev,
// unless that is unknown (0). Then *endprev is where this one ends, as
// its central header tells: past a local header taken to hold the same
// name and extra fields, and its data, in unsigned arithmetic that
// wraps, as UnZip's does. Its subfields adjust that (zi_subfield,
// zi_oldux).
static void zi_preceding(zitext *t, zentry const *e, u64 *endprev)
{
    u64 off = (u64)e->offset;
    if (off!=*endprev && *endprev) {
        zt_puts(t, "  There are an extra ");
        zt_putd(t, (i64)(off - *endprev));
        zt_puts(t, " bytes preceding this file.\n\n");
    }
    *endprev = off + ZIP_LOCAL_LEN + (u64)e->name.len + (u64)e->cextra.len +
               (u64)e->csize;
}

// A host's long name, by the upper byte of a version, made by or needed
// to extract (zi_long's os[]). Hosts 20 to 29 have none: "made by" shows
// as UnZip's builds print a null string, "(null)"; "needed" as unknown,
// like the hosts past 30.
static void zi_hostlong(zitext *t, u32 host, b32 needed)
{
    static char const *const names[UZ_HOSTS] = {
        "MS-DOS, OS/2 or NT FAT", "Amiga", "VMS", "Unix", "VM/CMS",
        "Atari ST", "OS/2 or NT HPFS", "Macintosh HFS", "Z-System", "CP/M",
        "TOPS-20", "NTFS", "SMS/QDOS", "Acorn RISC OS", "Win32 VFAT", "MVS",
        "BeOS", "Tandem NSK", "Theos", "Mac OS/X (Darwin)",
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        "AtheOS",
    };
    if (host<UZ_HOSTS && names[host]) {
        zt_puts(t, names[host]);
    } else if (host<UZ_HOSTS && !needed) {
        zt_puts(t, "(null)");
    } else {
        zt_puts(t, "unknown (");
        zt_putu(t, host);
        zt_putc(t, ')');
    }
}

// A method's long name (zi_long's method[]), or "unknown (N)".
static void zi_methodlong(zitext *t, u32 method)
{
    static struct { u16 id; char name[24]; } const methods[] = {
        {0, "none (stored)"}, {1, "shrunk"}, {2, "reduced (factor 1)"},
        {3, "reduced (factor 2)"}, {4, "reduced (factor 3)"},
        {5, "reduced (factor 4)"}, {6, "imploded"}, {7, "tokenized"},
        {8, "deflated"}, {9, "deflated (enhanced-64k)"},
        {10, "imploded (PK DCL)"}, {12, "bzipped"}, {14, "LZMA-ed"},
        {18, "tersed (IBM)"}, {19, "LZ77-compressed (IBM)"},
        {97, "WavPacked"}, {98, "PPMd-ed"},
    };
    for (i32 k = 0; k < countof(methods); k++) {
        if (methods[k].id == method) {
            zt_puts(t, methods[k].name);
            return;
        }
    }
    zt_puts(t, "unknown (");
    zt_putu(t, method);
    zt_putc(t, ')');
}

// A time as zi_time shows it under -v, from the fields {year, month,
// day, hour, minute, second}: "%u %s %u %02u:%02u:%02u", a month out of
// range as three digits.
static void zi_vdate(zitext *t, i32 const tm[6])
{
    static char const months[12][4] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    zt_putu(t, (u32)tm[0]);
    zt_putc(t, ' ');
    if (tm[1]<1 || tm[1]>12) {
        zt_num(t, (u32)tm[1], 10, 3, 1, 0);
    } else {
        zt_puts(t, months[tm[1]-1]);
    }
    zt_putc(t, ' ');
    zt_putu(t, (u32)tm[2]);
    for (i32 k = 3; k < 6; k++) {
        zt_putc(t, k==3 ? ' ' : ':');
        zt_num(t, (u32)tm[k], 10, 2, 1, 0);
    }
}

// A line's label, from its start, and room to column 50, where zi_long's
// values begin.
static void zt_label(zitext *t, iz start, char const *label)
{
    zt_puts(t, label);
    while (t->len-start<50 && t->len<t->cap) {
        zt_putc(t, ' ');
    }
}

// The external attributes' lines as zi_long shows them, by host:
// - VMS: "(RWED,RWED,RE,R)" from the Unix bits, for system (as owner),
//   owner, group, and other, with the mode in octal;
// - Amiga: its type, then hsparwed;
// - THEOS: its type's name, then a letter for each bit not set;
// - MS-DOS and the hosts with its attributes (OS/2 HPFS, NTFS, VFAT,
//   Acorn, VM/CMS, MVS): the upper 24 bits in hexadecimal;
// - any other host, Unix's: its type (d - l b c p s, else ?) and rwx,
//   set-ID as s or S, but l for the group's without x, and sticky as t
//   or T;
// then the MS-DOS attributes, the low byte: none, read-only, or words.
static void zi_vattribs(zitext *t, u32 made, u32 extattr)
{
    u32 host  = uz_host(made);
    u32 x     = extattr>>16 & 0xffff;
    u8  a[24];
    iz  n     = 0;
    iz  start = t->len;
    switch (host) {
    case UZ_VMS: {
        u8  w[12] = {0};
        u32 bits[] = {0400, 0200, 0100, 040, 020, 010, 04, 02, 01};
        for (i32 k = 0; k < 3; k++) {
            w[4*k+0] = x & bits[3*k+0] ? 'R' : 0;
            w[4*k+1] = x & bits[3*k+1] ? 'W' : 0;
            w[4*k+2] = x & bits[3*k+2] ? 'E' : 0;
            w[4*k+3] = x & bits[3*k+1] ? 'D' : 0;
        }
        a[n++] = '(';
        for (i32 g = 0; g < 4; g++) {  // system's, the owner's, first
            i32 k = g ? g-1 : 0;
            for (i32 i = 0; i < 4; i++) {
                if (w[4*k+i]) {
                    a[n++] = w[4*k+i];
                }
            }
            a[n++] = g<3 ? ',' : ')';
        }
        zt_puts(t, "  VMS file attributes (");
        zt_num(t, x, 8, 6, 1, 0);
        zt_label(t, start, " octal):");
    } break;

    case UZ_AMIGA:
        a[n++] = (x & 06000)==04000 ? 'd' : (x & 06000)==02000 ? '-' : '?';
        for (i32 k = 0; k < 8; k++) {
            a[n++] = x & 0200u>>k ? (u8)"hsparwed"[k] : '-';
        }
        zt_puts(t, "  Amiga file attributes (");
        zt_num(t, x, 8, 6, 1, 0);
        zt_label(t, start, " octal):");
        break;

    case UZ_THEOS: {
        static char const types[16][13] = {
            "???         ", "???         ", "???         ", "???         ",
            "Directory   ", "Library     ", "???         ", "???         ",
            "Sequential  ", "Direct      ", "Keyed       ", "Indexed     ",
            "???         ", " 86 program ", "286 program ", "386 program ",
        };
        u32 bits[] = {0x400, 0x800, 0x002, 0x004, 0x200, 0x040, 0x080, 0x100};
        bytecopy(a, types[x>>12], 12);
        n = 12;
        for (i32 k = 0; k < 8; k++) {
            a[n++] = x & bits[k] ? '.' : (u8)"HMWREXWR"[k];
        }
        zt_puts(t, "  Theos file attributes (");
        zt_hex(t, x, 4, 1);
        zt_label(t, start, " hex):");
    } break;

    case UZ_FAT: case ZI_HPFS: case ZI_NTFS: case ZI_VFAT: case UZ_ACORN:
    case ZI_CMS: case ZI_MVS:
        zt_label(t, start, "  non-MSDOS external file attributes:");
        zt_hex(t, extattr>>8, 6, 1);
        zt_puts(t, " hex\n");
        break;

    default:
        switch (x & 0170000) {
        case 0040000: a[0] = 'd'; break;
        case 0100000: a[0] = '-'; break;
        case 0120000: a[0] = 'l'; break;
        case 0060000: a[0] = 'b'; break;
        case 0020000: a[0] = 'c'; break;
        case 0010000: a[0] = 'p'; break;
        case 0140000: a[0] = 's'; break;
        default:      a[0] = '?';
        }
        for (i32 k = 0; k < 3; k++) {
            u32 m = x >> (6 - 3*k);
            a[1+3*k] = m & 4 ? 'r' : '-';
            a[2+3*k] = m & 2 ? 'w' : '-';
        }
        a[3] = x & 0100 ? (x & 04000 ? 's' : 'x') : (x & 04000 ? 'S' : '-');
        a[6] = x & 0010 ? (x & 02000 ? 's' : 'x') : (x & 02000 ? 'l' : '-');
        a[9] = x & 0001 ? (x & 01000 ? 't' : 'x') : (x & 01000 ? 'T' : '-');
        n = 10;
        zt_puts(t, "  Unix file attributes (");
        zt_num(t, x, 8, 6, 1, 0);
        zt_label(t, start, " octal):");
    }
    if (n) {
        zt_str(t, (s8){a, n});
        zt_putc(t, '\n');
    }

    u32 d = extattr & 0xff;
    start = t->len;
    zt_puts(t, "  MS-DOS file attributes (");
    zt_hex(t, d, 2, 1);
    zt_label(t, start, " hex):");
    if (d == 0) {
        zt_puts(t, "none");
    } else if (d == 1) {
        zt_puts(t, "read-only");
    } else {
        static char const words[8][5] = {
            "rdo ", "hid ", "sys ", "lab ", "dir ", "arc ", "lnk ", "exe",
        };
        for (i32 k = 0; k < 8; k++) {
            if (d & 1u<<k) {
                zt_puts(t, words[k]);
            }
        }
    }
    zt_putc(t, '\n');
}

// A labeled line of zi_long's.
static void zt_line(zitext *t, char const *label)
{
    zt_label(t, t->len, label);
}

// "%u.%u" of a version's lower byte.
static void zt_version(zitext *t, u32 v)
{
    zt_putu(t, (v & 0xff)/10);
    zt_putc(t, '.');
    zt_putu(t, (v & 0xff)%10);
    zt_putc(t, '\n');
}

// An entry's lines as zi_long shows them, after its name, through its
// attributes: its offset, as recorded, hosts and versions, method (and
// its sub-type), encryption, data descriptor, times, CRC, sizes, lengths,
// disk (always the first, the only one tugz reads), file type, and
// attributes. Its times are its DOS time's fields, and if given (local
// not null), its central extended timestamp's, in local time and UTC.
static void zi_long_text(zitext *t, zentry const *e, i32 const *local,
                         i32 const *utc)
{
    u32 flags = e->flags;
    zt_puts(t, "\n");
    zt_line(t, "  offset of local header from start of archive:");
    zt_putu(t, (u64)e->offset);
    zt_puts(t, "\n");
    zt_line(t, "");
    zt_putc(t, '(');
    zt_hex(t, (u64)e->offset, 16, 1);
    zt_puts(t, "h) bytes\n");
    zt_line(t, "  file system or operating system of origin:");
    zi_hostlong(t, e->made>>8, 0);
    zt_putc(t, '\n');
    zt_line(t, "  version of encoding software:");
    zt_version(t, e->made);
    zt_line(t, "  minimum file system compatibility required:");
    zi_hostlong(t, e->needed>>8, 1);
    zt_putc(t, '\n');
    zt_line(t, "  minimum software version required to extract:");
    zt_version(t, e->needed);
    zt_line(t, "  compression method:");
    zi_methodlong(t, e->method);
    zt_putc(t, '\n');
    if (e->method == 6) {
        zt_line(t, "  size of sliding dictionary (implosion):");
        zt_puts(t, flags & 2 ? "8K\n" : "4K\n");
        zt_line(t, "  number of Shannon-Fano trees (implosion):");
        zt_puts(t, flags & 4 ? "3\n" : "2\n");
    } else if (e->method==8 || e->method==9) {
        static char const sub[4][11] = {
            "normal\n", "maximum\n", "fast\n", "superfast\n",
        };
        zt_line(t, "  compression sub-type (deflation):");
        zt_puts(t, sub[flags>>1 & 3]);
    }
    zt_line(t, "  file security status:");
    zt_puts(t, flags & 1 ? "encrypted\n" : "not encrypted\n");
    zt_line(t, "  extended local header:");
    zt_puts(t, flags & 8 ? "yes\n" : "no\n");

    i32 tm[6];
    uz_dosdate(e->dostime, tm);
    zt_line(t, "  file last modified on (DOS date/time):");
    zi_vdate(t, tm);
    zt_putc(t, '\n');
    for (i32 k = 0; local && k < 2; k++) {
        zt_line(t, "  file last modified on (UT extra field modtime):");
        zi_vdate(t, k ? utc : local);
        zt_puts(t, k ? " UTC\n" : " local\n");
    }

    zt_line(t, "  32-bit CRC value (hex):");
    zt_hex(t, e->crc, 8, 0);
    zt_putc(t, '\n');
    zt_line(t, "  compressed size:");
    zt_putu(t, (u64)e->csize);
    zt_puts(t, " bytes\n");
    zt_line(t, "  uncompressed size:");
    zt_putu(t, (u64)e->usize);
    zt_puts(t, " bytes\n");
    zt_line(t, "  length of filename:");
    zt_putu(t, (u64)e->name.len);
    zt_puts(t, " characters\n");
    zt_line(t, "  length of extra field:");
    zt_putu(t, (u64)e->cextra.len);
    zt_puts(t, " bytes\n");
    zt_line(t, "  length of file comment:");
    zt_putu(t, (u64)e->comment.len);
    zt_puts(t, " characters\n");
    zt_line(t, "  disk number on which file begins:");
    zt_puts(t, "disk 1\n");
    zt_line(t, "  apparent file type:");
    zt_puts(t, e->intattr & 1 ? "text\n" : e->intattr & 2 ? "ebcdic\n" :
                                           "binary\n");
    zi_vattribs(t, e->made, e->extattr);
}

// The next subfield of an extra field, *x, as zi_long's loop takes it:
// false if fewer than the four bytes of a header are left; else its ID,
// its data, and the length its header states, more than the data's
// where that runs past the field's end, and *x moved past it.
static b32 zi_next(s8 *x, u32 *id, s8 *data, iz *stated)
{
    if (x->len < 4) {
        return 0;
    }
    *id     = get16(x->s);
    *stated = get16(x->s+2);
    iz n    = MIN(*stated, x->len-4);
    *data   = (s8){x->s+4, n};
    x->s   += 4 + n;
    x->len -= 4 + n;
    return 1;
}

// zi_long's error for a subfield whose stated length runs past the end
// of the extra field, with left bytes there. It goes to standard error
// (but with totals shown), after a newline if a line is unfinished.
static void zi_cut_text(zitext *t, u32 id, iz stated, iz left)
{
    zt_puts(t, "\n  error: EF data block (type 0x");
    zt_hex(t, id, 4, 0);
    zt_puts(t, ") size ");
    zt_putu(t, (u64)stated);
    zt_puts(t, " exceeds remaining extra field\n         space ");
    zt_putu(t, (u64)left);
    zt_puts(t, "; block length has been truncated.\n");
}

// Mac type and creator codes, eight bytes, as zi_showMacTypeCreator
// shows them: as characters if all are printable (in the C locale), else
// as two numbers.
static void zi_mactype(zitext *t, u8 const *p)
{
    b32 print = 1;
    for (i32 i = 0; i < 8; i++) {
        print &= p[i]>=0x20 && p[i]<0x7f;
    }
    zt_puts(t, ".\n    The associated file has type code `");
    if (print) {
        zt_str(t, (s8){(u8 *)p, 4});
        zt_puts(t, "' and creator code `");
        zt_str(t, (s8){(u8 *)p+4, 4});
    } else {
        u32 type    = (u32)p[0]<<24 | (u32)p[1]<<16 | (u32)p[2]<<8 | p[3];
        u32 creator = (u32)p[4]<<24 | (u32)p[5]<<16 | (u32)p[6]<<8 | p[7];
        zt_puts(t, "0x");
        zt_num(t, type, 16, 0, 0, 0);
        zt_puts(t, "' and creator code `0x");
        zt_num(t, creator, 16, 0, 0, 0);
    }
    zt_putc(t, '\'');
}

// Text from a subfield, n bytes up to any NUL, shown as names are, where
// UnZip writes it as it is.
static void zi_raw(zitext *t, u8 const *p, iz n, arena scratch)
{
    iz len = 0;
    for (; len<n && p[len]; len++) {}
    zt_str(t, uz_filter((s8){(u8 *)p, len}, &scratch));
}

// Subfield IDs (zi_long)
enum {
    ZI_ZIP64    = 0x0001,
    ZI_OS2      = 0x0009,
    ZI_ACL      = 0x4c41,
    ZI_NTSD     = 0x4453,
    ZI_IZVMS    = 0x4d49,
    ZI_UX       = 0x5855,
    ZI_UX2      = 0x7855,
    ZI_UT       = 0x5455,
    ZI_UPATH    = 0x7075,
    ZI_UCOMMENT = 0x6375,
    ZI_MAC3     = 0x334d,
    ZI_JLMAC    = 0x07c8,
    ZI_ZIPIT    = 0x2605,
    ZI_ZIPIT2   = 0x2705,
    ZI_SMARTZIP = 0x4d63,
    ZI_ATHEOS   = 0x7441,
    ZI_BEOS     = 0x6542,
    ZI_QDOS     = 0xfb4a,
    ZI_AOSVS    = 0x5356,
    ZI_TANDEM   = 0x4154,
    ZI_MD5      = 0x4b46,
};

// A subfield's name, zi_long's ef_fieldname.
static char const *zi_subname(u32 id)
{
    static struct { u16 id; char name[25]; } const names[] = {
        {0x0001, "PKWARE 64-bit sizes"}, {0x0007, "PKWARE AV"},
        {0x0009, "OS/2"}, {0x4c41, "OS/2 ACL"},
        {0x4453, "Security Descriptor"}, {0x000c, "PKWARE VMS"},
        {0x4d49, "Info-ZIP VMS"}, {0x000a, "PKWARE Win32"},
        {0x000d, "PKWARE Unix"}, {0x5855, "old Info-ZIP Unix/OS2/NT"},
        {0x7855, "Unix UID/GID (16-bit)"},
        {0x7875, "Unix UID/GID (any size)"}, {0x5455, "universal time"},
        {0x7075, "UTF8 path name"}, {0x6375, "UTF8 entry comment"},
        {0x334d, "new Info-ZIP Macintosh"},
        {0x07c8, "old Info-ZIP Macintosh"}, {0x2605, "ZipIt Macintosh"},
        {0x2705, "ZipIt Macintosh (short)"}, {0x4d63, "SmartZip Macintosh"},
        {0x4704, "VM/CMS"}, {0x470f, "MVS"}, {0x7441, "AtheOS"},
        {0x6542, "BeOS"}, {0xfb4a, "SMS/QDOS"}, {0x5356, "AOS/VS"},
        {0x4341, "Acorn SparkFS"}, {0x4b46, "Fred Kantor MD5"},
        {0x756e, "ASi Unix"}, {0x4154, "Tandem NSK"}, {0x6854, "Theos"},
    };
    for (i32 k = 0; k < countof(names); k++) {
        if (names[k].id == id) {
            return names[k].name;
        }
    }
    return "unknown";
}

// Whether a subfield's data begins with a signature of four characters.
static b32 zi_magic(u8 const *p, iz n, char const *sig)
{
    return n>=4 && get32(p)==get32((u8 const *)sig);
}

// One subfield of an entry's central extra field, its data d as zi_next
// gives it, described as zi_long describes it: its ID, name, and length,
// then what its decoder makes of its data, if long enough for it, else
// the data in hexadecimal, up to 24 bytes, or the first 20, then a
// period. Subfields that tell how the local header differs adjust where
// the entry ends, *endprev (zi_preceding): Zip64's, for the offset not
// there, if past 4 GiB; old Unix fields, for owners there; the access
// and creation times of a "UT"; Mac and BeOS attributes, unknown where
// compressed (0), as OS/2's and NT's security data are.
//
// Where UnZip writes text from the data as it is (a ZipIt or SmartZip
// name, a QDOS subtype, a VMS version), it is shown as names are; and a
// SmartZip name's length past its 31 bytes, past which UnZip writes,
// counts as 31.
static void zi_subfield(zitext *t, u32 id, s8 d, zentry const *e,
                        u64 *endprev, arena scratch)
{
    u32       host = uz_host(e->made);
    u8 const *p    = d.s;
    iz        n    = d.len;
    switch (id) {
    case ZI_ZIP64:
        if ((u64)e->offset >> 32) {
            *endprev -= n==8 ? 12 : 8;
        }
        break;
    case ZI_UX:
        if (host==UZ_UNIX && *endprev) {
            *endprev += 4;
        }
        break;
    case ZI_UX2:
        if (*endprev) {
            *endprev += 4;
        }
        break;
    }
    zt_puts(t, "\n  - A subfield with ID 0x");
    zt_hex(t, id, 4, 0);
    zt_puts(t, " (");
    zt_puts(t, zi_subname(id));
    zt_puts(t, ") and ");
    zt_putu(t, (u64)n);
    zt_puts(t, " data bytes");

    b32 done = 1;
    switch (id) {
    case ZI_OS2:
    case ZI_ACL:
    case ZI_NTSD:
        if ((done = n >= 4)) {
            zt_puts(t, ".\n    The local extra field has ");
            zt_putu(t, get32(p));
            zt_puts(t, id==ZI_OS2 ? " bytes of OS/2 extended attributes.\n"
                                    "    (May not match OS/2 \"dir\" amount"
                                    " due to storage method)" :
                       id==ZI_ACL ? " bytes of access control list"
                                    " information" :
                                    " bytes of NT security descriptor data");
            *endprev = 0;
        }
        break;

    case ZI_IZVMS:
        if ((done = n >= 8)) {
            static char const comp[4][19] = {
                "stored", "run-length encoded", "deflated", "compressed(?)",
            };
            static struct { char sig[5]; char name[8]; } const blocks[] = {
                {"VFAB", "FAB"}, {"VALL", "XABALL"}, {"VFHC", "XABFHC"},
                {"VDAT", "XABDAT"}, {"VRDT", "XABRDT"}, {"VPRO", "XABPRO"},
                {"VKEY", "XABKEY"}, {"VMSV", "version"},
            };
            char const *name = "unknown";
            for (i32 k = 0; k < countof(blocks); k++) {
                name = zi_magic(p, n, blocks[k].sig) ? blocks[k].name : name;
            }
            zt_puts(t, ".  The extra\n    field is ");
            zt_puts(t, comp[MIN(get16(p+4) & 7, 3)]);
            zt_puts(t, " and has ");
            zt_putu(t, get16(p+6));
            zt_puts(t, " bytes of VMS ");
            zt_puts(t, name);
            zt_puts(t, " information");
            if (zi_magic(p, n, "VMSV") && n>=16) {
                // " (V5.5)", cut short at a NUL, its parenthesis with it
                iz len = 0;
                for (; len<4 && p[12+len]; len++) {}
                zt_puts(t, " (");
                zi_raw(t, p+12, len, scratch);
                if (len == 4) {
                    zt_putc(t, ')');
                }
            }
        }
        break;

    case ZI_UT:
        if (n > 0) {
            static char const names[3][13] = {
                "modification", "access", "creation",
            };
            i32 num = 0;
            for (i32 k = 0; k < 3; k++) {
                if (!(p[0] & 1<<k)) {
                    continue;
                }
                zt_puts(t, num++ ? "/" : ".\n    The local extra field has"
                                         " UTC/GMT ");
                zt_puts(t, names[k]);
                if (k && *endprev) {
                    *endprev += 4;  // in the local "UT", not the central
                }
            }
            if (num) {
                zt_puts(t, num==1 ? " time" : " times");
            }
        }
        break;

    case ZI_UPATH:
    case ZI_UCOMMENT:
        if ((done = n >= 5)) {
            b32 all = n <= 29;
            zt_puts(t, all ? ".\n    The UTF8 data of the extra field (V"
                           : ". The first\n    24 UTF8 bytes in the extra"
                             " field (V");
            zt_putu(t, p[0]);
            zt_puts(t, ", ASCII name CRC `");
            zt_hex(t, get32(p+1), 8, 0);
            zt_puts(t, "') are:\n   ");
            for (iz i = 5; i < (all ? n : 29); i++) {
                zt_putc(t, ' ');
                zt_hex(t, p[i], 2, 0);
            }
        }
        break;

    case ZI_MAC3:
        if ((done = n >= 14)) {
            u32 ucsize = get32(p);
            u32 mflags = get16(p+4);
            b32 uncomp = mflags & 4;
            zt_puts(t, ".\n    The local extra field has ");
            zt_putu(t, ucsize);
            zt_puts(t, uncomp ? " bytes of uncompressed" : " bytes of"
                                                            " compressed");
            zt_puts(t, " Macintosh\n    finder attributes");
            if (!uncomp) {
                *endprev = 0;
            } else if (*endprev) {
                *endprev += ucsize;
            }
            zt_puts(t, ".\n    File is marked as ");
            zt_puts(t, mflags & 1 ? "Data-fork" : "Resource-fork");
            zt_puts(t, mflags & 8 ? ", File Dates are in 64 Bit"
                                  : ", File Dates are in 32 Bit");
            zi_mactype(t, p+6);
        }
        break;

    case ZI_ZIPIT2:
        if ((done = n>=5 && zi_magic(p, n, "ZPIT")) && n>=12) {
            zi_mactype(t, p+4);
        }
        break;

    case ZI_ZIPIT:
        if ((done = n>=5 && zi_magic(p, n, "ZPIT")) && n>=p[4]+13) {
            zt_puts(t, ".\n    The Mac long filename is ");
            zi_raw(t, p+5, p[4], scratch);
            zi_mactype(t, p+5+p[4]);
        }
        break;

    case ZI_JLMAC:
        if ((done = n>=40 && zi_magic(p, n, "JLEE"))) {
            zi_mactype(t, p+4);
            zt_puts(t, p[31] & 1 ? ".\n    File is marked as Data-fork"
                                 : ".\n    File is marked as Resource-fork");
        }
        break;

    case ZI_SMARTZIP:
        if ((done = n==64 && zi_magic(p, n, "dZip"))) {
            zi_mactype(t, p+4);
            zt_puts(t, ".\n    The Mac long filename is ");
            zi_raw(t, p+33, MIN(p[32], 31), scratch);
        }
        break;

    case ZI_ATHEOS:
    case ZI_BEOS:
        if ((done = n >= 5)) {
            u32 ucsize = get32(p);
            b32 uncomp = p[4] & 1;
            zt_puts(t, ".\n    The local extra field has ");
            zt_putu(t, ucsize);
            zt_puts(t, uncomp ? " bytes of uncompressed " : " bytes of"
                                                             " compressed ");
            zt_puts(t, id==ZI_ATHEOS ? "AtheOS file attributes"
                                     : "BeOS file attributes");
            if (!uncomp) {
                *endprev = 0;
            } else if (*endprev) {
                *endprev += ucsize;
            }
        }
        break;

    case ZI_QDOS:
        if ((done = n >= 4)) {
            zt_puts(t, ".\n    The QDOS extra field subtype is `");
            zt_str(t, uz_filter((s8){(u8 *)p, 4}, &scratch));
            zt_putc(t, '\'');
        }
        break;

    case ZI_AOSVS:
        if ((done = n >= 5)) {
            zt_puts(t, ".\n    The AOS/VS extra field revision is ");
            zt_putu(t, p[4]/10);
            zt_putc(t, '.');
            zt_putu(t, p[4]%10);
        }
        break;

    case ZI_TANDEM:
        if ((done = n == 20)) {
            static char const formats[6][16] = {
                "Unstructured", "Relative", "Entry Sequenced",
                "Key Sequenced", "Edit", "Object",
            };
            u32 type = (p[18] & 0x60u) >> 5;
            u32 code = (u32)p[0]<<8 | p[1];  // big-endian
            if (type == 0) {
                type = code==101 ? 4 : code==100 ? 5 : 0;
            }
            zt_puts(t, ".\n    The file was originally a Tandem ");
            zt_puts(t, formats[type]);
            zt_puts(t, " file, with file code ");
            zt_putu(t, code);
        }
        break;

    case ZI_MD5:
        if ((done = n >= 19)) {
            zt_puts(t, ".\n    The 128-bit MD5 signature is ");
            for (i32 i = 15; i >= 0; i--) {
                zt_hex(t, p[i], 2, 0);
            }
        }
        break;

    default:
        done = 0;
    }

    if (!done && n) {
        zt_puts(t, n<=24 ? ":\n   " : ".  The first\n    20 are:  ");
        for (iz i = 0; i < (n<=24 ? n : 20); i++) {
            zt_putc(t, ' ');
            zt_hex(t, p[i], 2, 0);
        }
    }
    zt_putc(t, '.');
}

// The lines after the extra field for an entry whose external attributes
// tell of an old Unix field ("UX") in its local header alone, bit 15
// (and bit 14 for its owner too): from Unix, OS/2 HPFS, or NTFS, it is
// there, its length adjusting where the entry ends (zi_preceding); from
// MS-DOS, it may be, if bit 14 is unset.
static void zi_oldux(zitext *t, zentry const *e, u64 *endprev)
{
    u32 host = uz_host(e->made);
    u32 x    = (e->extattr & 0xc000) >> 12;
    if (!(x & 8)) {
        return;
    } else if (host==UZ_UNIX || host==ZI_HPFS || host==ZI_NTFS) {
        zt_puts(t, "\n  There is a local extra field with ID 0x5855 (old"
                   " Info-ZIP Unix/OS2/NT) and\n  ");
        zt_putu(t, x & 12);
        zt_puts(t, x & 4 ? " data bytes (GMT modification/access times and"
                           " Unix UID/GID).\n"
                         : " data bytes (GMT modification/access times"
                           " only).\n");
        if (*endprev) {
            *endprev += x & 12;
        }
    } else if (host==UZ_FAT && !(x & 4)) {
        zt_puts(t, "\n  There may be a local extra field with ID 0x5855 (old"
                   " Info-ZIP Unix/OS2/NT) and\n  8 data bytes (GMT"
                   " modification/access times only).\n");
    }
}
