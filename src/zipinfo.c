// tugz zipinfo rules: ZipInfo mode's options and its listings' fields
//
// No I/O, as in unzip.c: the unzip program's ZipInfo mode (unzip -Z, or
// the program run by the name zipinfo) applies these. Each follows
// Info-ZIP UnZip 6.0's zipinfo.c (ZipInfo 3.00), named at each function,
// and the behavior of its builds as recorded in test/unziptests.c. The
// fields are short, each built in a zistr of its own.

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
