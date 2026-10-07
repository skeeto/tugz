// tugz unzip rules: entry names to paths, times, owners, modes, display
//
// No I/O, as in zip.c: the unzip program applies these. Each follows
// Info-ZIP UnZip 6.0's source, named at each function, and the behavior
// of its builds as recorded in test/unziptests.c: macOS's /usr/bin/unzip
// and Debian's (6.0-29, with Unicode, ISO dates, and owner restoring),
// taken where the two differ. Hosts are UnZip's numbers from the upper
// byte of "version made by", where anything past 30 counts as 31.

enum {
    UZ_FAT    = 0,
    UZ_AMIGA  = 1,
    UZ_VMS    = 2,
    UZ_UNIX   = 3,
    UZ_ATARI  = 5,
    UZ_QDOS   = 12,
    UZ_ACORN  = 13,
    UZ_BEOS   = 16,
    UZ_TANDEM = 17,
    UZ_THEOS  = 18,
    UZ_ATHEOS = 30,
    UZ_HOSTS  = 31,  // UnZip's NUM_HOSTS, for any later host
};

#define UZ_EXTRA_PKVMS   0x000c  // PKWARE's VMS
#define UZ_EXTRA_PKUNIX  0x000d  // PKWARE's Unix, laid out as "UX"
#define UZ_EXTRA_ASIUNIX 0x756e  // "nu": ASi's Unix, with a mode
#define UZ_DOS2038       0x74320000u  // UnZip's DOSTIME_2038_01_18

static u32 uz_host(u32 made)
{
    return MIN(made>>8, UZ_HOSTS);
}

// Options for uz_mapname.
enum {
    UZ_WINDOWS = 1 << 0,  // map for Windows file systems
    UZ_JUNK    = 1 << 1,  // -j: keep only the last component
    UZ_KEEPVER = 1 << 2,  // -V: keep a ";N" version suffix
};

// Results of uz_mapname, each a message of UnZip's. The warnings are
// status 1, a failure status 2.
enum {
    // "warning:  %s appears to use backslashes as path separators\n",
    // with the archive's name, once per archive, to stderr
    UZ_BACKSLASH = 1 << 0,
    // "warning:  stripped absolute path spec from %s\n", with
    // uzpath.full, to stderr
    UZ_ABSOLUTE  = 1 << 1,
    // "warning:  skipped \"../\" path component(s) in %s\n", with
    // uzpath.name, to stdout, before the entry's own line
    UZ_DOTDOT    = 1 << 2,
    // A directory entry, whose path, if empty, creates nothing, silently
    UZ_DIR       = 1 << 3,
    // "mapname:  conversion of %s failed\n", with uzpath.name, to
    // stderr: the entry is skipped
    UZ_FAILED    = 1 << 4,
};

typedef struct {
    s8  path;   // relative, '/'-separated, without a trailing '/'
    s8  full;   // the name, its '\' converted (UZ_BACKSLASH)
    s8  name;   // that, without leading '/' (UZ_ABSOLUTE)
    i32 flags;
} uzpath;

// Whether a name, up to any extension and without trailing spaces, is a
// Windows device's (CON, PRN, AUX, NUL, COM0-9 and LPT0-9 with the
// superscript digits too, CONIN$, CONOUT$, CLOCK$), in any case.
static b32 uz_device(u8 const *s, iz len)
{
    iz n = 0;
    for (; n<len && s[n]!='.'; n++) {}
    for (; n>0 && s[n-1]==' '; n--) {}
    u8 up[8] = {0};
    if (n > countof(up)) {
        return 0;
    }
    for (iz i = 0; i < n; i++) {
        up[i] = s[i]>='a' && s[i]<='z' ? (u8)(s[i] - 32) : s[i];
    }
    static u8 const names[][8] = {
        "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$",
        "COM", "LPT",  // followed by a digit
    };
    for (i32 k = 0; k < countof(names); k++) {
        b32 port = k >= countof(names)-2;
        iz  i    = 0;
        for (; i<countof(up) && names[k][i] && up[i]==names[k][i]; i++) {}
        if (i<countof(up) && names[k][i]) {
            continue;  // a mismatch
        } else if (!port && i==n) {
            return 1;
        } else if (port) {
            b32 digit = n==4 && up[3]>='0' && up[3]<='9';
            b32 super = n==5 && up[3]==0xc2 &&
                        (up[4]==0xb9 || up[4]==0xb2 || up[4]==0xb3);
            if (digit || super) {
                return 1;
            }
        }
    }
    return 0;
}

// Finish a Windows path component, p[beg..*n): drop trailing dots and
// spaces, which Windows would drop, so that a name ending in them cannot
// name another file (tugz's own rule), then put '_' before a device's
// name, as UnZip's maskDOSdevice puts it before a name that the system
// takes for a character device.
static void uz_wincomp(u8 *p, iz beg, iz *n)
{
    for (; *n>beg && (p[*n-1]=='.' || p[*n-1]==' '); (*n)--) {}
    if (*n>beg && uz_device(p+beg, *n-beg)) {
        bytemove(p+beg+1, p+beg, *n-beg);
        p[beg] = '_';
        (*n)++;
    }
}

// Map an entry's name to a relative path to extract it to, with UnZip's
// checks in its order (extract.c, extract_or_test_member), then its
// mapname (unix/unix.c, or win32/win32.c with UZ_WINDOWS):
// 1. Made on MS-DOS (FAT), without a '/', '\' separates (UZ_BACKSLASH).
// 2. Leading '/' are stripped (UZ_ABSOLUTE).
// 3. With UZ_JUNK, only what follows the last '/' is kept.
// 4. Empty and "." components are dropped, as are ".." (UZ_DOTDOT).
// 5. Control characters are dropped: on POSIX all but 0x20-0x7e and
//    0x80-0xfe; on Windows all below 0x20.
// 6. A final ";N", N any digits, is dropped, unless UZ_KEEPVER.
// 7. On POSIX, a final "." becomes "_", and ".." "__".
// 8. On Windows, ':', '\', '<', '>', '|', '"', '?', and '*' become '_';
//    trailing dots and spaces are dropped from components (not UnZip's:
//    UnZip relies on the system's dropping them); and device names get a
//    '_' in front (UnZip asks the system which are devices).
// 9. A file whose final component is left empty fails (UZ_FAILED).
// A name ending in '/' is a directory (UZ_DIR). The path never holds
// ".." components, a leading '/', NUL or control characters, nor on
// Windows drive letters, streams, or device names.
static uzpath uz_mapname(s8 name, u32 made, i32 opts, arena *perm)
{
    uzpath r = {0};
    b32 win  = opts & UZ_WINDOWS;

    s8 n = {newstr(perm, name.len), name.len};
    bytecopy(n.s, name.s, n.len);
    b32 slash = 0;
    for (iz i = 0; i < n.len; i++) {
        slash |= n.s[i] == '/';
    }
    if (uz_host(made)==UZ_FAT && !slash) {
        for (iz i = 0; i < n.len; i++) {
            if (n.s[i] == '\\') {
                n.s[i] = '/';
                r.flags |= UZ_BACKSLASH;
            }
        }
    }
    r.full = n;
    iz skip = 0;
    for (; skip<n.len && n.s[skip]=='/'; skip++) {}
    r.flags |= skip ? UZ_ABSOLUTE : 0;
    n.s   += skip;
    n.len -= skip;
    r.name = n;

    b32 dir = n.len && n.s[n.len-1]=='/';
    iz  i   = 0;
    if (opts & UZ_JUNK) {
        for (iz k = 0; k < n.len; k++) {
            i = n.s[k]=='/' ? k+1 : i;
        }
    }

    // Each component grows by at most a '_' before it
    u8 *p    = newstr(perm, 2*(n.len - i) + 1);
    iz  len  = 0;
    iz  beg  = 0;   // of the component being built
    iz  semi = -1;  // its last ';'
    for (; i < n.len; i++) {
        u8 c = n.s[i];
        if (c == '/') {
            s8 comp = {p+beg, len-beg};
            if (comp.len==1 && comp.s[0]=='.') {
                len = beg;
            } else if (comp.len==2 && comp.s[0]=='.' && comp.s[1]=='.') {
                len = beg;
                r.flags |= UZ_DOTDOT;
            } else if (win) {
                uz_wincomp(p, beg, &len);
            }
            if (len > beg) {
                p[len++] = '/';
            }
            beg  = len;
            semi = -1;
        } else if (win && (c==':' || c=='\\' || c=='<' || c=='>' ||
                           c=='|' || c=='"' || c=='?' || c=='*')) {
            p[len++] = '_';
        } else if (c == ';') {
            semi = len;
            p[len++] = c;
        } else if ((c>=0x20 && c<0x7f) || (c>=0x80 && c<0xff) ||
                   (win && c>=0x7f)) {
            p[len++] = c;
        }
    }

    if (dir) {
        len -= len > 0;  // the separator after the last component
        r.path   = (s8){p, len};
        r.flags |= UZ_DIR;
        return r;
    }

    if (!(opts & UZ_KEEPVER) && semi>=0) {
        iz k = semi + 1;
        for (; k<len && p[k]>='0' && p[k]<='9'; k++) {}
        len = k==len ? semi : len;
    }
    s8 comp = {p+beg, len-beg};
    if (win) {
        uz_wincomp(p, beg, &len);
    } else if (comp.len==1 && comp.s[0]=='.') {
        p[beg] = '_';
    } else if (comp.len==2 && comp.s[0]=='.' && comp.s[1]=='.') {
        p[beg] = p[beg+1] = '_';
    }
    if (len == beg) {
        r.flags |= UZ_FAILED;
        return r;
    }
    r.path = (s8){p, len};
    return r;
}

// The fields of an MS-DOS date<<16 | time as UnZip lists them, not
// normalized (list.c): {year 1980-2107, month 0-15, day 0-31, hour 0-31,
// minute 0-63, second 0-62}.
static void uz_dosdate(u32 dostime, i32 tm[6])
{
    tm[0] = (i32)(dostime >> 25) + 1980;
    tm[1] = (i32)(dostime>>21 & 15);
    tm[2] = (i32)(dostime>>16 & 31);
    tm[3] = (i32)(dostime>>11 & 31);
    tm[4] = (i32)(dostime>>5 & 63);
    tm[5] = (i32)(dostime & 31) * 2;
}

// Unix seconds from a broken-down UTC time {year, month 1-12, day, hour,
// minute, second}, the inverse of zip_gmtime. Fields out of range carry
// over, as with mktime: month 13 is January of the next year, day 0 the
// last of the month before, hour 24 midnight of the next day. As UnZip's
// dos_to_unix_time computes it (fileio.c, without mktime), so do the
// DOS times with months 1-13 through 2100 that its builds extract; it
// reads months 0, 14, and 15 from outside its table of days (ydays[]),
// and counts 2100 a leap year for the years after it, a day late.
static i64 uz_timegm(i32 const tm[6])
{
    i64 m = (i64)tm[1] - 1;
    i64 q = m / 12 - (m%12 < 0);
    i64 y = tm[0] + q;
    m = m - q*12 + 1;
    // Howard Hinnant's days_from_civil
    y -= m <= 2;
    i64 era  = (y >= 0 ? y : y-399) / 400;
    i64 yoe  = y - era*400;
    i64 doy  = (153*(m + (m>2 ? -3 : 9)) + 2)/5 + tm[2] - 1;
    i64 doe  = yoe*365 + yoe/4 - yoe/100 + doy;
    i64 days = era*146097 + doe - 719468;
    return days*86400 + (i64)tm[3]*3600 + (i64)tm[4]*60 + tm[5];
}

// Flags of uz_extra_izux: the times it found, and the owner.
enum {
    UZ_MTIME = 1 << 0,
    UZ_ATIME = 1 << 1,
    UZ_CTIME = 1 << 2,
    UZ_OWNER = 1 << 8,
};

typedef struct {
    i64 mtime;
    i64 atime;
    i64 ctime;
    u32 uid;
    u32 gid;
    i32 flags;
} uzizux;

// Read a 2, 4, or 8-byte owner ID, only if it fits 32 bits, as Debian's
// UnZip reads and restores them (read_ux3_value, close_outfile).
static b32 uz_id(u8 const *p, iz size, u32 *id)
{
    u64 v = 0;
    switch (size) {
    case 2: v = get16(p);  break;
    case 4: v = get32(p);  break;
    case 8: v = get64(p);  break;
    default: return 0;
    }
    *id = (u32)v;
    return v <= 0xffffffff;
}

// The times and owner among an entry's extra fields, local or central,
// as UnZip's ef_scan_for_izux (process.c) scans them:
// - An extended timestamp ("UT") gives the times its flags announce,
//   only the modification time in its central form. It overrides any
//   earlier times, and leaves later old Unix ("UX", or PKWARE's) fields
//   ignored. The last of several wins.
// - "UX" gives modification and access times, and from 12 bytes an
//   owner, if no "UT", "Ux" or "ux" came before it.
// - The newer Unix fields give only an owner: "Ux" two 16-bit IDs, in
//   exactly 4 bytes, unless after a "ux"; "ux", of version 1, IDs of 2,
//   4, or 8 bytes. Each drops any earlier owner, keeping times, as in
//   Debian's build (with IZ_HAVE_UXUIDGID, which UnZip 6.0 leaves out,
//   and fixes: macOS's drops "UX" times after a "Ux"). A "ux" whose IDs
//   run past its end has none (Debian's reads past it), nor one with an
//   ID past 32 bits (Debian's reads it but does not restore it).
// - Times are unsigned 32-bit. One with bit 31 set counts only with a
//   DOS time from 2038-01-18 on: else a modification time voids the
//   field's times, and another time just itself.
// Where UnZip leaves a time announced but unread (the access time of a
// central "UT"), it is not reported here.
static uzizux uz_extra_izux(s8 x, b32 central, u32 dostime)
{
    uzizux r      = {0};
    i32    flags  = 0;  // times in the low byte, as UnZip keeps them
    i32    newer  = 0;  // UnZip's have_new_type_eb: 1 after UT/Ux, 2 ux
    b32    compat = 0;  // high times are unsigned, the DOS time agreeing
    for (iz i = 0; x.len-i >= 4;) {
        u32 id  = get16(x.s+i);
        iz  len = get16(x.s+i+2);
        u8 *d   = x.s + i + 4;
        if (len > x.len-i-4) {
            break;
        }
        i += 4 + len;

        switch (id) {
        case ZIP_EXTRA_TIME:
            flags &= ~0xff;
            newer  = 1;
            if (len < 1) {
                break;
            }
            flags |= d[0];
            iz at = 1;
            if (flags & UZ_MTIME) {
                if (at+4 > len) {
                    flags &= ~UZ_MTIME;
                } else {
                    u32 t = get32(d+at);
                    at += 4;
                    compat = t>>31 && dostime>=UZ_DOS2038;
                    if (t>>31 && !compat) {
                        flags &= ~0xff;
                        break;
                    }
                    r.mtime = t;
                }
            }
            if (central) {
                flags &= ~(UZ_ATIME | UZ_CTIME);
                break;
            }
            i64 *times[] = {&r.atime, &r.ctime};
            i32  bits[]  = {UZ_ATIME, UZ_CTIME};
            for (i32 k = 0; k < 2; k++) {
                if (!(flags & bits[k])) {
                    continue;
                } else if (at+4 > len) {
                    flags &= ~bits[k];
                    continue;
                }
                u32 t = get32(d+at);
                at += 4;
                if (t>>31 && !compat) {
                    flags &= ~bits[k];
                } else {
                    *times[k] = t;
                }
            }
            break;

        case ZIP_EXTRA_UNIX2:  // "Ux"
            newer  = newer ? newer : 1;
            if (newer > 1) {
                break;
            }
            flags &= 0xff;
            if (len == 4) {
                r.uid  = get16(d);
                r.gid  = get16(d+2);
                flags |= UZ_OWNER;
            }
            break;

        case ZIP_EXTRA_UNIX:  // "ux"
            newer  = 2;
            flags &= 0xff;
            if (len>=7 && d[0]==1) {
                iz us = d[1];
                iz gs = 2+us < len ? d[2+us] : len;
                if (3+us+gs<=len && uz_id(d+2, us, &r.uid) &&
                    uz_id(d+3+us, gs, &r.gid)) {
                    flags |= UZ_OWNER;
                }
            }
            break;

        case ZIP_EXTRA_UNIX1:  // "UX"
        case UZ_EXTRA_PKUNIX:
            if (len<8 || newer) {
                break;
            }
            flags |= UZ_MTIME | UZ_ATIME;
            u32 m = get32(d+4);
            u32 a = get32(d);
            compat = m>>31 && dostime>=UZ_DOS2038;
            if (m>>31 && !compat) {
                flags &= ~0xff;
            }
            r.mtime = m;
            if (a>>31 && !compat && (flags & 0xff)) {
                flags &= ~UZ_ATIME;
            } else {
                r.atime = a;
            }
            if (len >= 12) {
                r.uid  = get16(d+8);
                r.gid  = get16(d+10);
                flags |= UZ_OWNER;
            }
            break;
        }
    }
    r.flags = flags & (UZ_MTIME | UZ_ATIME | UZ_CTIME | UZ_OWNER);
    return r;
}

// An entry's mode, as UnZip's mapattr derives it for POSIX
// (unix/unix.c), from the version made by, the external attributes,
// the name, and central extra fields:
// - From Unix-like hosts (Unix, VMS, Acorn, Atari, AtheOS, BeOS, QDOS,
//   Tandem, THEOS), the upper 16 bits; if zero, an ASi Unix field's
//   mode, else PKWARE VMS's or a short ASi field mean DOS attributes.
// - From Amiga, its read, write, and execute bits for all.
// - Otherwise, from the DOS attributes: write unless read-only, and
//   search for a directory (a name ending in '/' is one), for all,
//   unless, from MS-DOS, the upper 16 bits agree with them for the
//   owner: then those.
// - Without keep (-K), set-ID and sticky bits are dropped (filtattr).
// Modes made from DOS or Amiga attributes take the umask (umask set).
// The mode keeps its file type: a link is a link only from the hosts
// uz_symlink_host names, or from MS-DOS with agreeing attributes.
typedef struct {
    u32 mode;     // as st_mode, its type included
    b32 umask;    // apply the process's umask
    b32 symlink;
} uzmode;

static b32 uz_islink(u32 mode)
{
    return (mode & 0170000) == 0120000;
}

static b32 uz_symlink_host(u32 made);

static uzmode uz_mode(u32 extattr, u32 made, s8 name, s8 cextra, b32 keep)
{
    uzmode r    = {0};
    u32    host = uz_host(made);
    u32    tmp  = extattr;
    b32    dos  = 0;
    switch (host) {
    case UZ_AMIGA:
        tmp = tmp>>17 & 7;
        r.mode  = tmp<<6 | tmp<<3 | tmp;
        r.umask = 1;
        break;

    case UZ_THEOS:
        tmp &= 0xf1ffffff;
        tmp &= (tmp & 0xf0000000)==0x40000000 ? 0x41ffffff : 0x01ffffff;
        [[fallthrough]];
    case UZ_UNIX: case UZ_VMS: case UZ_ACORN: case UZ_ATARI:
    case UZ_ATHEOS: case UZ_BEOS: case UZ_QDOS: case UZ_TANDEM:
        r.mode = tmp >> 16;
        for (iz i = 0; !r.mode && !dos && cextra.len-i>=4;) {
            u32 id  = get16(cextra.s+i);
            iz  len = get16(cextra.s+i+2);
            if (len > cextra.len-i-4) {
                break;
            }
            if (id==UZ_EXTRA_ASIUNIX && len>=6) {
                r.mode = get16(cextra.s+i+8);
                break;
            }
            dos = id==UZ_EXTRA_ASIUNIX || id==UZ_EXTRA_PKVMS;
            i += 4 + len;
        }
        if (!dos) {
            r.symlink = uz_islink(r.mode) && uz_symlink_host(made);
            break;
        }
        [[fallthrough]];
    default:
        r.mode = host==UZ_FAT || dos ? tmp>>16 : 0;
        if (!(tmp & 0x10) && name.len && name.s[name.len-1]=='/') {
            tmp |= 0x10;
        }
        tmp = (u32)!(tmp & 1)<<1 | (tmp & 0x10)>>4;
        if ((r.mode & 0700) == (0400 | tmp<<6)) {
            r.symlink = uz_islink(r.mode) && host==UZ_FAT;
            break;
        }
        r.mode  = 0444 | tmp<<6 | tmp<<3 | tmp;
        r.umask = 1;
    }
    r.mode &= keep ? 0xffff : 0xffff & ~07000u;
    return r;
}

// Whether a host makes POSIX symbolic links, as UnZip's SYMLINK_HOST:
// Unix, VMS, Atari, BeOS, and AtheOS.
static b32 uz_symlink_host(u32 made)
{
    u32 h = uz_host(made);
    return h==UZ_UNIX || h==UZ_VMS || h==UZ_ATARI || h==UZ_BEOS ||
           h==UZ_ATHEOS;
}

// An entry's Windows file attributes, as UnZip's mapattr derives them
// for Windows (win32/win32.c), from the low byte of the external
// attributes whatever the host, archive set for files, limited to the
// bits it then sets (0x7f): read-only 0x01, hidden 0x02, system 0x04,
// and archive 0x20 among them.
static u32 uz_dosattr(u32 extattr)
{
    u32 a = extattr | (extattr & 0x10 ? 0 : 0x20);
    return a & 0x7f;
}

// A name made safe to display, after UnZip's fnfilter (extract.c):
// control characters below 0x20 as '^' and a letter ("^A", "^["), and
// DEL as '?'. Where UnZip relies on the locale for the rest, this takes
// names as UTF-8: C1 controls (U+0080-U+009F) and each byte of anything
// not UTF-8 also become '?'.
static s8 uz_filter(s8 s, arena *perm)
{
    s8 r = {newstr(perm, 2*s.len), 0};
    for (iz i = 0; i < s.len;) {
        u32 c = s.s[i];
        if (c < 0x20) {
            r.s[r.len++] = '^';
            r.s[r.len++] = (u8)(c + 64);
            i++;
            continue;
        } else if (c < 0x7f) {
            r.s[r.len++] = (u8)c;
            i++;
            continue;
        }
        iz n = c>=0xc2 && c<0xe0 ? 2 : c>=0xe0 && c<0xf0 ? 3 :
               c>=0xf0 && c<0xf5 ? 4 : 0;
        b32 ok = n && n<=s.len-i && zip_utf8((s8){s.s+i, n})==1;
        b32 c1 = ok && c==0xc2 && s.s[i+1]<0xa0;
        if (!ok || c1) {
            r.s[r.len++] = '?';
            i += ok ? n : 1;
        } else {
            bytecopy(r.s+r.len, s.s+i, n);
            r.len += n;
            i     += n;
        }
    }
    return r;
}
