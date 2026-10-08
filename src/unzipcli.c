// tugz unzip program: Info-ZIP UnZip compatible command line and driver
//
// Finds each archive named, reads its central directory (src/zipin.c),
// and lists, tests, or extracts the entries selected, decoding through
// src/inflate.c and applying src/unzip.c's rules. Messages, the streams
// they go to, and exit statuses follow UnZip 6.0's source, cited by file
// at each (unzip.c, process.c, list.c, extract.c, fileio.c), with the
// additions of Debian's build (ISO dates in listings, and its check for
// overlapped components), but for the departures listed in docs/notes.md.
//
// The run is staged so that each archive's memory is claimed before its
// output: options, then per archive the central directory, then the
// mode, which decodes each entry through one inflator claimed at the
// start. Extraction to disk shares the test's selection, checks, and
// decoding, its paths and the room for what it finishes at the end (links,
// then directories' attributes) planned beforehand.
//
// Nothing is written through a link: no directory on an entry's path,
// below the -d directory, may be one, and a file there is replaced by
// removing it and creating the new one exclusively. Between the check
// and the creation, another process could still put a link in the way.

// Exit statuses, UnZip's PK_* (unzip.h)
enum {
    PK_OK     = 0,
    PK_WARN   = 1,
    PK_ERR    = 2,   // an error in an entry; others were processed
    PK_BADERR = 3,   // a severe error in the archive
    PK_MEM    = 4,
    PK_NOZIP  = 9,   // no archive found
    PK_PARAM  = 10,  // bad or unsupported options
    PK_FIND   = 11,  // no matching entries
    PK_BOMB   = 12,  // overlapped components (Debian's)
    PK_DISK   = 50,  // a write failed
    PK_EOF    = 51,  // the archive ended early
    IZ_DIR    = 76,  // the "zipfile" was a directory (internal)
    IZ_UNSUP  = 81,  // entries skipped: unsupported method or encryption
};

// Platform interface for the unzip program, beyond src/io.c, src/dir.c,
// and src/zipin.c

// Whether a standard descriptor is a terminal (console).
static b32  os_isatty(os *, i32 fd);
// Why the last failed system call failed, in the words of the C
// library's strerror, or an empty string if unknown.
static s8   os_error(os *);

// Extraction. Paths are those the driver has checked: no directory on
// the way to one, beyond the -d directory, is a link (src/zipin.c's
// linkless), and none of these follows a link at its end.

// Create a directory, with the defaults for a new one (umask, inherited
// access control). Returns false on failure, os_error saying why.
static b32  os_mkdir(os *, s8 path, arena scratch);
// The process's umask (POSIX), or zero.
static u32  os_umask(os *);
// Unix seconds from a broken-down local time {year, month 1-12, day,
// hour, minute, second}, fields out of range carried over, as for an
// MS-DOS time: the inverse of os_localtime, daylight saving time taken
// as the zone had it then.
static i64  os_mktime(os *, i32 const tm[6]);

// Attributes to give an extracted file, directory, or link: those of
// flags, each failure reported by its own flag, with the reason in
// why[], indexed by the flag's bit (OS_AWHY reasons in all).
enum {
    OS_AOWNER = 1 << 0,  // uid and gid (POSIX)
    OS_AMODE  = 1 << 1,  // mode (POSIX) or attributes (Windows)
    OS_ATIMES = 1 << 2,  // modification and access times
    OS_ALINK  = 1 << 3,  // the link itself (os_symlink)
    OS_ASGID  = 1 << 4,  // keep a directory's set-group-ID bit (POSIX)
    OS_AWHY   = 4,
};
typedef struct {
    i64 mtime;
    i64 atime;
    u32 mode;     // permission bits, 07777, umask already applied
    u32 dosattr;  // read-only, hidden, system, archive (Windows)
    u32 uid;
    u32 gid;
    i32 flags;
} osattrs;
// Give a created file, still open and not yet kept, its attributes: on
// POSIX, its owner, then mode, then times. Returns the flags that
// failed, their reasons copied into the arena.
static i32  os_setattrs(os *, i32 fd, osattrs *, s8 *why, arena *);
// As os_setattrs, for a directory, through a handle that refuses a link
// at its end: its owner, then times, then mode, as UnZip orders them.
static i32  os_setdirattrs(os *, s8 path, osattrs *, s8 *why, arena *);
// Create a symbolic link to target, which must not exist, and, given
// OS_AOWNER, give the link its owner. On Windows, where links need
// privileges, a regular file holding the target. Returns the flags that
// failed, OS_ALINK for the link itself.
static i32  os_symlink(os *, s8 target, s8 path, osattrs *, s8 *why,
                       arena *);

static void os_oom(os *ctx)
{
    s8 msg = S("error:  not enough memory\n");
    os_write(ctx, 2, msg.s, msg.len);
    os_exit(ctx, PK_MEM);
}

// The two arenas share one region, perm growing up from its bottom and
// scratch down from its top. The platform's os_extend gives each more.
typedef struct {
    arena perm;
    arena scratch;   // allocates downward
    s8   *args;      // excluding the program name
    i32   nargs;
    s8    unzipenv;  // UNZIP, if set: options before the arguments
    s8    unzipopt;  // UNZIPOPT, if set: when UNZIP has none
    b32   windows;   // Windows conventions: names, wildcards
} unzipconfig;

static s8 const unzip_usage = S8(
    "tugz unzip " TUGZ_VERSION ", a subset of Info-ZIP UnZip 6.0\n"
    "usage: unzip [-opts[modifiers]] file[.zip] [list] [-x xlist] "
    "[-d exdir]\n"
    "  Default action is to extract files in list, except those in xlist,"
    " to exdir;\n"
    "  file[.zip] may be a wildcard, or - for standard input.\n"
    "\n"
    "  -p  extract files to pipe, no messages     -l  list files"
    " (short format)\n"
    "  -f  freshen existing files, create none    -t  test compressed"
    " archive data\n"
    "  -u  update files, create if necessary      -z  display archive"
    " comment only\n"
    "  -v  list verbosely/show version info       -c  extract files to"
    " stdout, named\n"
    "  -x  exclude files that follow (in xlist)   -d  extract files into"
    " exdir\n"
    "modifiers:\n"
    "  -n  never overwrite existing files         -q  quiet mode"
    " (-qq => quieter)\n"
    "  -o  overwrite files WITHOUT prompting      -j  junk paths"
    " (no directories)\n"
    "  -C  match filenames case-insensitively     -V  retain VMS version"
    " numbers\n"
    "  -D  skip restoring directory times         -DD skip restoring all"
    " times\n"
);
static s8 const unzip_usage_posix = S8(
    "  -X  restore UID/GID info                   -K  keep setuid/setgid/"
    "tacky modes\n"
);
static s8 const unzip_usage_tail = S8(
    "See \"unzip -hh\" for more help.  Examples:\n"
    "  unzip data1 -x joe   => extract all files except joe from zipfile"
    " data1.zip\n"
    "  unzip -p foo | more  => send contents of foo.zip via pipe into"
    " program more\n"
    "  unzip -fo foo ReadMe => quietly replace existing ReadMe if archive"
    " file newer\n"
);

static s8 const unzip_help = S8(
    "Options may come before the archive, as in Info-ZIP UnZip, or after\n"
    "it, as in busybox: an argument after the archive made only of option\n"
    "letters (\"-q\", \"-qo\") is taken for options, not a member name.\n"
    "-d exdir may appear anywhere, and -x starts a list of members to\n"
    "exclude, which runs to the end. A leading - negates the option that\n"
    "follows it (\"--q\" cancels one -q from UNZIP).\n"
    "\n"
    "The archive file[.zip] is tried as given, then with .zip and .ZIP\n"
    "appended. A wildcard (*, ?, [...]) in its name processes every match.\n"
    "An archive of - is read from standard input, and implies -n unless\n"
    "-o is given.\n"
    "\n"
    "Member names in list and xlist are wildcards: * matches any run of\n"
    "characters, / included, ? one character, [...] a set, and \\ escapes.\n"
    "\n"
    "Listings show dates as YYYY-MM-DD. Entries compressed by methods other\n"
    "than stored and deflated, and encrypted entries, are skipped.\n"
    "Options UNZIP, or if it has none, UNZIPOPT, apply first.\n"
    "\n"
    "Exit status: 0 success, 1 warnings, 2 errors in some entries, 3 a\n"
    "damaged archive, 4 out of memory, 9 no archive found, 10 bad options,\n"
    "11 no matching entries, 12 overlapped entries (a zip bomb), 50 a\n"
    "write error, 51 an archive cut short, 81 entries skipped (unsupported\n"
    "method or encryption).\n"
);

// How a message is written (UnZip's MSG_* flags, unzpriv.h)
enum {
    MSG_STDERR = 0x01,  // to standard error, but standard output under -t
    MSG_LNEWLN = 0x20,  // a newline first, unless at the start of a line
    MSG_TNEWLN = 0x40,  // a newline last, unless the message ends one
};

// The output written at a time, as UnZip's slide with Deflate64
enum { UZ_WSIZE = 1 << 16 };

// UnZip's overwrite modes (G.overwrite_mode), for the whole run
enum { OVERWRT_QUERY, OVERWRT_ALWAYS, OVERWRT_NEVER };

// The longest link target held for its link, PATH_MAX on Linux, beyond
// which no system takes one
enum { UZ_LINKMAX = 4096 };

// A link, deferred until the files are extracted, with the empty file
// that holds its place meanwhile, as UnZip's slinkentry with the file
// holding its target
typedef struct {
    s8      path;
    s8      target;
    osattrs attrs;   // the owner, with -X
    u64     dev;     // the placeholder's identity
    u64     ino[2];
} xlink;

typedef struct {
    xlink *data;
    iz     len;
    iz     cap;
} xlinks;

// A directory created by its own entry, to be given its attributes once
// the files are in it, as UnZip's uxdirattr
typedef struct {
    s8      path;
    osattrs attrs;
} xdir;

typedef struct {
    xdir *data;
    iz    len;
    iz    cap;
} xdirs;

// An entry's extraction, planned before any output
typedef struct {
    uzpath map;     // its path below the -d directory
    s8     full;    // that with the -d directory, for a link or directory
    u8    *target;  // room for a link's target, or null
} xentry;

typedef struct {
    os     *ctx;
    arena   perm;
    b32     windows;
    i32     crccpu;

    // Options, as UnZip's uO
    i32     vflag;   // -l 1, -v 2 and more
    i32     qflag;   // -q, -qq; -p adds 999
    i32     zflag;
    i32     Dflag;
    i32     Xflag;
    i32     ovall;   // -o
    b32     ovnone;  // -n
    b32     tflag;
    b32     cflag;   // -c, -p
    b32     fflag;
    b32     uflag;
    b32     jflag;
    b32     Cflag;
    b32     Vflag;
    b32     Kflag;
    i32     showhelp;
    b32     hasexdir;
    s8      exdir;
    b32     extract;  // UnZip's extract_flag: none of -c -l -p -t -v -z

    s8      zipspec;  // the archive argument
    s8s     fspecs;   // members to process, if any
    s8s     xspecs;   // members to exclude
    b32     xlist;    // -x was given
    b32     allfiles; // UnZip's process_all_files: no list, no -x

    // Output
    writer *out;      // standard output, flushed before standard error
    b32     sol;      // at the start of a line, on either stream
    b32     dup;      // -t errors also to standard error (UnZip's)

    // The run
    inflator *inf;
    u8     *window;   // decoded data, UZ_WSIZE bytes at a time
    s8s     matches;  // wildcard archive names
    b32     noecrec;  // an archive tried had no end record
    s8      zipfn;    // the archive being processed

    // Extraction
    i32     overwrite; // OVERWRT_*: -o, -n, or the prompt's A or N
    reader *answers;   // the prompt's, standard input, unless the archive
    u32     umask;
    s8      root;      // the -d directory and a '/'
    b32     rooted;    // which is there
    s8      checked;   // known to be directories, through its last '/'
    iz      checkcap;  // room for that
    iz      room;      // that extracting an entry takes
    b32     slashed;   // the archive's backslashes were warned of
    xlinks  links;     // links to create once the files are extracted
    xdirs   dirs;      // directories to give attributes after that

    // The archive being processed
    i64     nentries; // entries read from its central directory
    i32     cderr;    // what ended the reading early: CD_*
    i64     oldshift; // the shift undone to find an entry (UnZip's)
} unzip;

// What ended reading a central directory before its entries' count, as
// UnZip finds the first only once it gets there
enum {
    CD_OK,
    CD_SIG,   // an entry's header was invalid (or had no name)
    CD_NONAME,
    CD_END,   // a warning: the directory ended elsewhere than it says
};

// Write a message as UnZip's UzpMessagePrnt does (fileio.c): with flags
// MSG_*, to standard output, or to standard error, unless testing (-t),
// when everything goes to standard output, so that redirecting it keeps
// the whole report, but errors are repeated on standard error when that
// alone is a terminal. Whether the last message ended a line is tracked
// across both streams.
static void info(unzip *u, i32 flags, s8 msg)
{
    b32 err = (flags & MSG_STDERR) && !u->tflag;
    b32 dup = (flags & MSG_STDERR) && u->tflag && u->dup;
    u8  nl  = '\n';
    if (err || dup) {
        writer_flush(u->out);
    }
    if ((flags & MSG_LNEWLN) && !u->sol) {
        if (err) {
            os_write(u->ctx, 2, &nl, 1);
        } else {
            writer_byte(u->out, nl);
        }
        if (dup) {
            os_write(u->ctx, 2, &nl, 1);
        }
        u->sol = 1;
    }
    b32 trail = (flags & MSG_TNEWLN) &&
                (msg.len ? msg.s[msg.len-1]!='\n' : !u->sol);
    for (i32 k = 0; k < 2; k++) {
        s8 part = k ? (trail ? (s8){&nl, 1} : (s8){0}) : msg;
        if (!part.len) {
            continue;
        } else if (err) {
            os_write(u->ctx, 2, part.s, part.len);
        } else {
            writer_s8(u->out, part);
        }
        if (dup) {
            os_write(u->ctx, 2, part.s, part.len);
        }
        u->sol = part.s[part.len-1] == '\n';
    }
}

// Formatting, as UnZip's printf formats

// s left-justified in a field of width w, as "%-ws".
static s8 lj(arena *a, s8 s, iz w)
{
    if (s.len >= w) {
        return s;
    }
    s8 r = {newstr(a, w), w};
    bytecopy(r.s, s.s, s.len);
    bytefill(r.s+s.len, ' ', w-s.len);
    return r;
}

// s right-justified in a field of width w, as "%ws".
static s8 rj(arena *a, s8 s, iz w)
{
    if (s.len >= w) {
        return s;
    }
    s8 r = {newstr(a, w), w};
    bytefill(r.s, ' ', w-s.len);
    bytecopy(r.s+w-s.len, s.s, s.len);
    return r;
}

// An unsigned number, as "%wu", or with zero padding, "%0wu".
static s8 unum(arena *a, u64 v, iz w, b32 zero)
{
    u8  tmp[24];
    u8 *e = tmp + countof(tmp);
    u8 *p = e;
    do {
        *--p = (u8)('0' + v%10);
    } while (v /= 10);
    for (; e-p < w && zero; *--p = '0') {}
    s8 r = {newstr(a, e-p), e-p};
    bytecopy(r.s, p, r.len);
    return rj(a, r, w);
}

static s8 hex8(arena *a, u32 v)
{
    s8 r = {newstr(a, 8), 8};
    for (i32 i = 7; i >= 0; i--, v >>= 4) {
        r.s[i] = "0123456789abcdef"[v & 15];
    }
    return r;
}

// "s" after a count other than one.
static s8 plural(u64 n)
{
    return n==1 ? S("") : S("s");
}

// UnZip's ratio (list.c): the compression factor in tenths of a percent,
// negative for growth.
static i32 ratio(u64 uc, u64 c)
{
    if (!uc) {
        return 0;
    } else if (uc > 2000000) {
        u64 denom = uc / 1000;
        return uc>=c ? (i32)((uc-c + (denom>>1)) / denom)
                     : -(i32)((c-uc + (denom>>1)) / denom);
    }
    return uc>=c ? (i32)((1000*(uc-c) + (uc>>1)) / uc)
                 : -(i32)((1000*(c-uc) + (uc>>1)) / uc);
}

// The compression factor as list.c shows it: "%c%d%%", or "100%".
static s8 cfactor(arena *a, u64 uc, u64 c)
{
    i32 f = ratio(uc, c);
    u8  sgn = f<0 ? '-' : ' ';
    f = f<0 ? (-f + 5)/10 : (f + 5)/10;
    if (f == 100) {
        return S("100%");
    }
    return JOIN(a, (s8){&sgn, 1}, unum(a, (u64)f, 0, 0), S("%"));
}

static s8 cstr8(char const *z)
{
    s8 r = {(u8 *)z, 0};
    for (; z[r.len]; r.len++) {}
    return r;
}

// A name or other string from the archive, made safe to display.
static s8 shown(unzip *u, s8 name, arena *a)
{
    (void)u;
    return uz_filter(name, a);
}

// Display text from the archive, its comment or an entry's, as UnZip's
// do_string (fileio.c) does (DISPLAY): up to any NUL, without carriage
// returns or ^S, and with an escape shown as "^[". Other control
// characters, and what is not UTF-8, are shown as in names (uz_filter),
// where UnZip writes them raw. A newline follows unless it ends one.
static void show_text(unzip *u, s8 text, arena scratch)
{
    s8 t = {newstr(&scratch, text.len), 0};
    for (iz i = 0; i < text.len && text.s[i]; i++) {
        u8 c = text.s[i];
        if (c!='\r' && c!=0x13) {
            t.s[t.len++] = c;
        }
    }
    s8 r = {0};
    for (iz i = 0; i < t.len;) {
        iz j = i;
        for (; j<t.len && t.s[j]!='\n' && t.s[j]!='\t'; j++) {}
        s8 part = shown(u, (s8){t.s+i, j-i}, &scratch);
        r = JOIN(&scratch, r, part, j<t.len ? (s8){t.s+j, 1} : S(""));
        i = j + 1;
    }
    info(u, 0, r);
    info(u, MSG_TNEWLN, S(""));
}

static s8 const end_sig_msg = S8(
    "\nnote:  didn't find end-of-central-dir signature at end of central"
    " dir.\n"
);

static s8 const report_msg = S8(
    "  (please check that you have transferred or created the zipfile in"
    " the\n"
    "  appropriate BINARY mode and that you have compiled UnZip properly)\n"
);

// Options

static i32 show_usage(unzip *u, b32 error)
{
    s8 msg = JOIN(&u->perm, unzip_usage,
                  u->windows ? S("") : unzip_usage_posix, unzip_usage_tail);
    info(u, error ? MSG_STDERR : 0, msg);
    return error ? PK_PARAM : PK_OK;
}

static void show_version(unzip *u)
{
    s8 line = unzip_usage;
    for (line.len = 0; line.s[line.len++] != '\n';) {}
    info(u, 0, line);
}

static s8 const must_exdir =
    S8("error:  must specify directory to which to extract with -d option\n");

// Option letters that UnZip takes but tugz does not, refused
static b32 unsupported(unzip *u, u8 c)
{
    s8 refused = S("aBEFiIJLMNOPQsSTUWYZ$:^2/");
    return zip_has(refused, c) || (u->windows && (c=='K' || c=='X'));
}

// Option letters taken after the archive, as busybox takes them: those
// supported, but for -d and -x, which there are Info-ZIP's.
static b32 late_option(unzip *u, s8 arg)
{
    s8 letters = S("-bcCDefhjKlnopqtuvVXz");
    if (arg.len<2 || arg.s[0]!='-') {
        return 0;
    }
    for (iz i = 1; i < arg.len; i++) {
        if (!zip_has(letters, arg.s[i]) || unsupported(u, arg.s[i])) {
            return 0;
        }
    }
    return 1;
}

static i32 minus(i32 v, i32 by)
{
    return MAX(v-by, 0);
}

// Apply an argument of option letters, as UnZip's uz_opts (unzip.c),
// where -d may take the next argument, args[*i+1]. Unknown letters set
// *error, for the usage to follow. Returns 0 or an exit status.
static i32 apply_options(unzip *u, s8 *args, i32 nargs, i32 *i, b32 *error,
                         arena scratch)
{
    s8  arg = args[*i];
    i32 neg = 0;
    for (iz k = 1; k < arg.len; k++) {
        u8 c = arg.s[k];
        switch (c) {
        case '-':
            neg++;
            break;
        case 'b':
            neg = 0;  // -b, the default, does nothing
            break;
        case 'c':
            u->cflag = !neg;
            neg = 0;
            break;
        case 'C':
            u->Cflag = !neg;
            neg = 0;
            break;
        case 'd':
            if (neg) {
                info(u, MSG_STDERR, must_exdir);
                return PK_PARAM;
            } else if (u->hasexdir) {
                info(u, MSG_STDERR, S("error:  -d option used more than once "
                                      "(only one exdir allowed)\n"));
                return PK_PARAM;
            }
            u->exdir = (s8){arg.s+k+1, arg.len-k-1};
            if (!u->exdir.len) {
                if (*i+1>=nargs || (args[*i+1].len && args[*i+1].s[0]=='-')) {
                    info(u, MSG_STDERR, must_exdir);
                    return PK_PARAM;
                }
                u->exdir = args[++*i];
            }
            u->hasexdir = 1;
            k = arg.len;
            break;
        case 'D':
            u->Dflag = neg ? minus(u->Dflag, neg) : u->Dflag+1;
            neg = 0;
            break;
        case 'e':
        case 'x':
            break;  // extraction, the default
        case 'f':
            u->fflag = u->uflag = !neg;
            neg = 0;
            break;
        case 'h':
            if (!u->showhelp) {
                u->showhelp = k+1<arg.len && arg.s[k+1]=='h' ? 2 : 1;
            }
            break;
        case 'j':
            u->jflag = !neg;
            neg = 0;
            break;
        case 'K':
            u->Kflag = !neg;
            neg = 0;
            break;
        case 'l':
            u->vflag = neg ? minus(u->vflag, neg) : u->vflag+1;
            neg = 0;
            break;
        case 'n':
            u->ovnone = !neg;
            neg = 0;
            break;
        case 'o':
            u->ovall = neg ? minus(u->ovall, neg) : u->ovall+1;
            neg = 0;
            break;
        case 'p':
            u->cflag = !neg;
            u->qflag = neg ? minus(u->qflag, 999) : u->qflag+999;
            neg = 0;
            break;
        case 'q':
            u->qflag = neg ? minus(u->qflag, neg) : u->qflag+1;
            neg = 0;
            break;
        case 't':
            u->tflag = !neg;
            neg = 0;
            break;
        case 'u':
            u->uflag = !neg;
            neg = 0;
            break;
        case 'v':
            u->vflag = neg ? minus(u->vflag, neg) : u->vflag ? u->vflag+1 : 2;
            neg = 0;
            break;
        case 'V':
            u->Vflag = !neg;
            neg = 0;
            break;
        case 'X':
            u->Xflag = neg ? minus(u->Xflag, neg) : u->Xflag+1;
            neg = 0;
            break;
        case 'z':
            u->zflag = neg ? minus(u->zflag, neg) : u->zflag+1;
            neg = 0;
            break;
        default:
            if (unsupported(u, c)) {
                info(u, MSG_STDERR, JOIN(&scratch, S("error:  -"),
                     (s8){&arg.s[k], 1}, S(" option not supported\n")));
                return PK_PARAM;
            }
            *error = 1;
        }
    }
    return 0;
}

// Parse the arguments: options, the archive, then members, -x and its
// list, -d, and options again (busybox's), as UnZip's uz_opts and unzip
// do (unzip.c), with the environment's options first. Returns -1 to go
// on, or an exit status.
static i32 unzip_args(unzip *u, unzipconfig *conf, arena scratch)
{
    // UNZIP, or if it has none, UNZIPOPT (envargs.c)
    s8s args = env_args(&u->perm, conf->unzipenv, u->windows);
    if (!args.len) {
        args = env_args(&u->perm, conf->unzipopt, u->windows);
    }
    for (i32 i = 0; i < conf->nargs; i++) {
        *push(&u->perm, &args) = conf->args[i];
    }
    s8 *a = args.data;
    i32 n = (i32)args.len;

    b32 error = 0;
    i32 i     = 0;
    for (; i<n && a[i].len && a[i].s[0]=='-' && !zequals(a[i], S("-")); i++) {
        i32 r = apply_options(u, a, n, &i, &error, scratch);
        if (r) {
            return r;
        }
    }
    b32 zipfile = i < n;
    if (zipfile) {
        u->zipspec = a[i++];
    }

    // What follows the archive: Info-ZIP's -d exdir anywhere, -x then a
    // list running to the end, and members, and busybox's options
    for (; i < n; i++) {
        s8 arg = a[i];
        if (!u->hasexdir && arg.len>=2 && arg.s[0]=='-' && arg.s[1]=='d') {
            u->exdir = (s8){arg.s+2, arg.len-2};
            if (!u->exdir.len) {
                if (i+1 >= n) {
                    info(u, MSG_STDERR, must_exdir);
                    return PK_PARAM;
                }
                u->exdir = a[++i];
            }
            u->hasexdir = 1;
        } else if (zequals(arg, S("-x"))) {
            u->xlist = 1;
        } else if (late_option(u, arg)) {
            i32 r = apply_options(u, a, n, &i, &error, scratch);
            if (r) {
                return r;
            }
        } else {
            *push(&u->perm, u->xlist ? &u->xspecs : &u->fspecs) = arg;
        }
    }
    u->allfiles = !u->fspecs.len && !u->xlist;

    if (u->showhelp) {
        if (u->showhelp == 2) {
            info(u, 0, JOIN(&u->perm, unzip_usage, S("\n"), unzip_help));
            return PK_OK;
        }
        return show_usage(u, 0);
    }
    if ((u->cflag && (u->tflag || u->uflag)) || (u->tflag && u->uflag) ||
        (u->fflag && u->ovnone)) {
        info(u, MSG_STDERR, S("error:  -fn or any combination of -c, -l, -p,"
                              " -t, -u and -v options invalid\n"));
        error = 1;
    }
    if (u->ovall && u->ovnone) {
        info(u, MSG_STDERR,
             S("caution:  both -n and -o specified; ignoring -o\n"));
        u->ovall = 0;
    }
    if (!zipfile || error) {
        if (u->vflag>=2 && !zipfile) {
            show_version(u);  // "unzip -v": tugz's version, in a line
            return PK_OK;
        }
        return show_usage(u, conf->nargs || error);
    }
    u->extract = !u->cflag && !u->tflag && !u->vflag && !u->zflag;
    if (u->hasexdir && !u->extract) {
        info(u, MSG_STDERR, S("caution:  not extracting; -d ignored\n"));
    }
    if (zequals(u->zipspec, S("-")) && !u->ovall) {
        u->ovnone = 1;  // busybox's: no prompting, stdin being the archive
    }
    return -1;
}

// Selection

static i32 member_flags(unzip *u)
{
    return ZIP_SETS | (u->Cflag ? ZIP_FOLD : 0);
}

// Whether an entry, by its name, is selected by the member list and not
// excluded by -x, as extract.c and list.c match, noting in fm and xm,
// if given, the patterns that matched.
static b32 wanted(unzip *u, s8 name, u8 *fm, u8 *xm)
{
    if (u->allfiles) {
        return 1;
    }
    i32 flags = member_flags(u);
    b32 hit   = !u->fspecs.len;
    for (iz i = 0; !hit && i < u->fspecs.len; i++) {
        if (zip_match(u->fspecs.data[i], name, flags)) {
            hit = 1;
            if (fm) {
                fm[i] = 1;
            }
        }
    }
    for (iz i = 0; hit && i < u->xspecs.len; i++) {
        if (zip_match(u->xspecs.data[i], name, flags)) {
            hit = 0;
            if (xm) {
                xm[i] = 1;
            }
        }
    }
    return hit;
}

// An entry's name for matching and display: in Unicode, if it has that
// too, as Debian's UnZip shows it.
static s8 entry_name(zarchive *ar, zentry *e)
{
    return shown_name(ar, e);
}

// Report an invalid central header, the nth of those UnZip counts.
static void cd_error(unzip *u, i64 n, arena scratch)
{
    if (u->cderr == CD_NONAME) {
        info(u, MSG_STDERR, S(":  bad filename length (central)\n"));
        return;
    }
    info(u, MSG_STDERR, JOIN(&scratch, S("error:  expected central file "
         "header signature not found (file #"), unum(&scratch, (u64)n, 0, 0),
         S(").\n"), report_msg));
}

// Listing (list.c)

// An entry's time as list.c shows it: its central extended timestamp's
// in local time, else its DOS time's fields, unnormalized.
static void entry_time(unzip *u, zentry *e, i32 tm[6])
{
    uzizux x = uz_extra_izux(e->cextra, 1, e->dostime);
    if (x.flags & UZ_MTIME) {
        os_localtime(u->ctx, x.mtime, tm);
    } else {
        uz_dosdate(e->dostime, tm);
    }
}

// Year-month-day hour:minute, as Debian lists dates (DF_YMD).
static s8 iso_time(arena *a, i32 const tm[6])
{
    return JOIN(a, unum(a, (u32)tm[0], 2, 1), S("-"),
                unum(a, (u32)tm[1], 2, 1), S("-"),
                unum(a, (u32)tm[2], 2, 1), S(" "),
                unum(a, (u32)tm[3], 2, 1), S(":"),
                unum(a, (u32)tm[4], 2, 1));
}

// A method's name for -v, as list.c's method[], with deflation's level.
static s8 method_name(arena *a, zentry *e)
{
    static char const *const names[] = {
        "Stored", "Shrunk", "Reduce1", "Reduce2", "Reduce3", "Reduce4",
        "Implode", "Token", "Defl:#", "Def64#", "ImplDCL", "",
        "BZip2", "", "LZMA", "", "", "", "Terse", "IBMLZ77",
    };
    u32 m = e->method;
    s8  r = {0};
    if (m<countof(names) && names[m][0]) {
        r = JOIN(a, cstr8(names[m]));
    } else if (m == 97) {
        return S("WavPack");
    } else if (m == 98) {
        return S("PPMd");
    } else {
        return JOIN(a, S("Unk:"), unum(a, m, 3, 1));
    }
    if (m==ZIP_DEFLATE || m==9) {
        r.s[5] = (u8)"NXFS"[e->flags>>1 & 3];
    }
    return r;
}

static i32 list_files(unzip *u, zarchive *ar, arena scratch)
{
    b32 longhdr = u->vflag > 1;
    if (u->qflag < 2) {
        info(u, 0, longhdr
            ? S(" Length   Method    Size  Cmpr    Date    Time   CRC-32   Name\n"
                "--------  ------  ------- ---- ---------- ----- --------  ----\n")
            : S("  Length      Date    Time    Name\n"
                "---------  ---------- -----   ----\n"));
    }

    u64 members = 0;
    u64 tusize  = 0;
    u64 tcsize  = 0;
    for (i64 i = 0; i < u->nentries; i++) {
        arena   tmp  = scratch;
        zentry *e    = ar->entries + i;
        s8      name = entry_name(ar, e);
        if (!wanted(u, name, 0, 0)) {
            continue;
        }
        i32 tm[6];
        entry_time(u, e, tm);
        u64 usize = (u64)e->usize;
        u64 csize = (u64)e->csize - (e->flags & ZIP_FLAG_ENCRYPTED ? 12 : 0);
        s8  line  = {0};
        if (longhdr) {
            line = JOIN(&tmp, unum(&tmp, usize, 8, 0), S("  "),
                        lj(&tmp, method_name(&tmp, e), 7),
                        unum(&tmp, csize, 8, 0), S(" "),
                        rj(&tmp, cfactor(&tmp, usize, csize), 4), S(" "),
                        iso_time(&tmp, tm), S(" "), hex8(&tmp, e->crc),
                        S("  "));
        } else {
            line = JOIN(&tmp, unum(&tmp, usize, 9, 0), S("  "),
                        iso_time(&tmp, tm), S("   "));
        }
        info(u, 0, JOIN(&tmp, line, shown(u, name, &tmp), S("\n")));
        if (!u->qflag && e->comment.len) {
            show_text(u, e->comment, tmp);
        }
        tusize += usize;
        tcsize += csize;
        members++;
    }
    if (u->cderr==CD_SIG || u->cderr==CD_NONAME) {
        cd_error(u, u->nentries+1, scratch);
        return PK_BADERR;
    }

    if (u->qflag < 2) {
        arena tmp = scratch;
        s8    s   = plural(members);
        s8    n   = unum(&tmp, members, 0, 0);
        if (longhdr) {
            info(u, 0, JOIN(&tmp,
                S("--------          -------  ---                       "
                  "     -------\n"),
                unum(&tmp, tusize, 8, 0), S("         "),
                unum(&tmp, tcsize, 8, 0), S(" "),
                rj(&tmp, cfactor(&tmp, tusize, tcsize), 4),
                S("                            "), n, S(" file"), s,
                S("\n")));
        } else {
            info(u, 0, JOIN(&tmp,
                S("---------                     -------\n"),
                unum(&tmp, tusize, 9, 0), S("                     "),
                n, S(" file"), s, S("\n")));
        }
    }
    i32 err = PK_OK;
    if (u->cderr == CD_END) {
        info(u, MSG_STDERR, end_sig_msg);
        err = PK_WARN;
    }
    return err<=PK_WARN && !members ? PK_FIND : err;
}

// Testing and extracting (extract.c)

enum { DIR_BLKSIZ = 16384 };  // entries checked, then processed

// The version of the format this reads, UnZip's UNZIP_VERSION with
// Zip64, and VMS_UNZIP_VERSION.
enum { UZ_CANDO = 45, UZ_CANDO_VMS = 42 };

// Whether an entry's version, method, and encryption let it be read.
static b32 readable(zentry *e)
{
    u32 ver  = e->needed & 0xff;
    u32 host = e->needed >> 8;
    u32 can  = host==UZ_VMS ? UZ_CANDO_VMS : UZ_CANDO;
    return ver<=can && !(e->flags & ZIP_FLAG_ENCRYPTED) &&
           (e->method==ZIP_STORE || e->method==ZIP_DEFLATE);
}

// Whether an entry can be decoded, else saying why it is skipped, as
// extract.c's store_info.
static b32 store_info(unzip *u, zentry *e, s8 name, arena scratch)
{
    b32 show = !u->qflag;
    u32 ver  = e->needed & 0xff;
    u32 host = e->needed >> 8;
    s8  sk   = JOIN(&scratch, S("   skipping: "),
                    lj(&scratch, shown(u, name, &scratch), 22), S("  "));
    if ((host==UZ_VMS && ver>UZ_CANDO_VMS) || (host!=UZ_VMS && ver>UZ_CANDO)) {
        if (show) {
            u32 can = host==UZ_VMS ? UZ_CANDO_VMS : UZ_CANDO;
            info(u, MSG_STDERR, JOIN(&scratch, sk, S("need "),
                 host==UZ_VMS ? S("VMS") : S("PK"), S(" compat. v"),
                 unum(&scratch, ver/10, 0, 0), S("."),
                 unum(&scratch, ver%10, 0, 0), S(" (can do v"),
                 unum(&scratch, can/10, 0, 0), S("."),
                 unum(&scratch, can%10, 0, 0), S(")\n")));
        }
        return 0;
    }

    if (e->method!=ZIP_STORE && e->method!=ZIP_DEFLATE) {
        static struct { u16 id; char const *name; } const methods[] = {
            {0, "store"}, {1, "shrink"}, {2, "reduce"}, {3, "reduce"},
            {4, "reduce"}, {5, "reduce"}, {6, "implode"}, {7, "tokenize"},
            {8, "deflate"}, {9, "deflate64"}, {10, "DCL implode"},
            {12, "bzip2"}, {14, "LZMA"}, {18, "IBM/Terse"},
            {19, "IBM LZ77"}, {97, "WavPack"}, {98, "PPMd"},
        };
        if (show) {
            s8 msg = {0};
            for (i32 k = 0; k < countof(methods); k++) {
                if (methods[k].id == e->method) {
                    msg = JOIN(&scratch, sk, S("`"), cstr8(methods[k].name),
                               S("' method not supported\n"));
                }
            }
            if (!msg.s) {
                msg = JOIN(&scratch, sk, S("unsupported compression method "),
                           unum(&scratch, e->method, 0, 0), S("\n"));
            }
            info(u, MSG_STDERR, msg);
        }
        return 0;
    }

    if (e->flags & ZIP_FLAG_ENCRYPTED) {
        if (show) {
            info(u, MSG_STDERR, JOIN(&scratch, sk,
                                     S("encrypted (not supported)\n")));
        }
        return 0;
    }
    return 1;
}

// The spans of the entries to be read, sorted, for the overlap check.
typedef struct {
    i64 *beg;   // where each begins, ascending
    iz   len;
} zspans;

// Sort spans, their beginnings and ends, by their beginnings: a stable
// bottom-up merge sort.
static void sort_spans(i64 *beg, i64 *end, iz n, arena scratch)
{
    i64 *tb = new(&scratch, n, i64);
    i64 *te = new(&scratch, n, i64);
    for (iz w = 1; w < n; w *= 2) {
        for (iz lo = 0; lo < n; lo += 2*w) {
            iz mid = MIN(lo+w, n);
            iz hi  = MIN(lo+2*w, n);
            iz i = lo, j = mid, k = lo;
            while (i<mid || j<hi) {
                b32 right = i==mid || (j<hi && beg[j]<beg[i]);
                iz  from  = right ? j++ : i++;
                tb[k]   = beg[from];
                te[k++] = end[from];
            }
        }
        bytecopy(beg, tb, n*(iz)sizeof(*beg));
        bytecopy(end, te, n*(iz)sizeof(*end));
    }
}

// The offset of the first entry to be read past off, or the central
// directory's, which none may reach into.
static i64 next_span(zarchive *ar, zspans *s, i64 off)
{
    iz lo = 0;
    for (iz hi = s->len; lo < hi;) {
        iz mid = lo + (hi - lo)/2;
        if (s->beg[mid] <= off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo<s->len ? s->beg[lo] : ar->end.cdoff;
}

static s8 const bomb_msg = S8(
    "error: invalid zip file with overlapped components (possible zip bomb)\n"
);

// Read a line of standard input as C's fgets reads one into a buffer of
// cap bytes, as UnZip reads answers: up to cap-1 bytes, through a
// newline. Returns its length, or -1 at the end of the input, or where
// the input is the archive.
static iz read_answer(unzip *u, u8 *buf, iz cap)
{
    reader *r = u->answers;
    iz      n = 0;
    while (r && n<cap-1 && reader_fill(r)) {
        u8 c = r->buf[r->off++];
        buf[n++] = c;
        if (c == '\n') {
            break;
        }
    }
    return n ? n : -1;
}

// A write to an extracted file failed: ask, as UnZip's disk_error does,
// whether to go on with the next entry.
static b32 disk_error(unzip *u, s8 sname, arena scratch)
{
    info(u, MSG_STDERR|MSG_LNEWLN, JOIN(&scratch, sname,
         S(":  write error (disk full?).  Continue? (y/n/^C) ")));
    u8 answer[10];
    return read_answer(u, answer, countof(answer))>0 && answer[0]=='y';
}

// Where an entry's data goes when extracted to disk.
typedef struct {
    i32 fd;      // a file, or -1
    u8 *target;  // or a link's target, room for its size, or null
    b32 link;
    b32 goon;    // after a failed write, the next entry is wanted
} xout;

// Read and decode an entry's data, its local header found at l, to
// standard output with -c and -p, nowhere with -t, or to disk, out,
// as extract.c's extract_or_test_member, name being, on disk, its path.
// That leaves the line naming it unfinished. Returns a status, PK_DISK
// if output failed.
static i32 test_member(unzip *u, zarchive *ar, zentry *e, zlocal *l,
                       i64 usize, s8 name, xout *disk, arena scratch)
{
    s8  sname = shown(u, name, &scratch);
    b32 named = u->qflag != 0;  // the name precedes errors
    b32 store = e->method == ZIP_STORE;
    if (u->tflag) {
        if (!u->qflag) {
            info(u, 0, JOIN(&scratch, S("    testing: "),
                            lj(&scratch, sname, 22), S("  ")));
        }
    } else if (!u->qflag) {
        s8 verb = disk && disk->link && store ? S("    linking: ") :
                  store ? S(" extracting: ") : S("  inflating: ");
        info(u, 0, JOIN(&scratch, verb, lj(&scratch, sname, 22),
                        disk ? S("  ") : S("  \n")));
    }

    // Local extra fields are checked under -t before the data moves the
    // window from them (TestExtraField)
    u32 eflen = 0;
    u32 efrem = 0;
    for (iz i = 0; l->extra.len-i >= 4;) {
        u32 len = get16(l->extra.s+i+2);
        if ((iz)len > l->extra.len-i-4) {
            eflen = len;
            efrem = (u32)(l->extra.len-i-4);
            break;
        }
        i += 4 + len;
    }

    // Output goes out a window at a time, as UnZip flushes its slide, so
    // that an error in a short entry's data leaves none of it written.
    // Beyond its size, it is still decoded to check, but not written.
    zin *in    = &ar->in;
    i64  pos   = l->data;
    i64  left  = e->csize;
    i64  out   = 0;
    u32  crc   = 0;
    b32  bad   = 0;  // invalid compressed data
    zbuf b     = {0};
    inflator *s = u->inf;
    inflate_reset(s);
    b.out    = u->window;
    b.outlen = UZ_WSIZE;
    for (b32 done = 0; !done;) {
        if (!b.inlen && left) {
            u8 *p = 0;
            iz  n = (iz)MIN(left, ZIN_CAP);
            i32 got = zin_get(in, pos, n, &p);
            if (got < 0) {
                info(u, MSG_STDERR, S("error:  zipfile read error\n"));
                writer_flush(u->out);
                os_exit(u->ctx, PK_BADERR);  // as UnZip's readbyte
            } else if (!got) {
                bad = !store;  // the file has shrunk: as if at its end
                break;
            }
            b.in    = p;
            b.inlen = n;
            pos  += n;
            left -= n;
        }

        s8 data = {0};
        if (store) {
            data = (s8){(u8 *)b.in, b.inlen};
            b.inlen = 0;
            done = !left;
        } else {
            i32 r = inflate_stream(s, &b);
            if (r == GZ_OK) {
                done = 1;
            } else if (r==GZ_NEEDIN && !left) {
                done = bad = 1;  // ends with the entry's data unfinished
            } else if (r!=GZ_NEEDIN && r!=GZ_NEEDOUT) {
                done = bad = 1;
            }
            if (bad || (!done && b.outlen)) {
                continue;  // the window not yet full, nor the stream done
            }
            data = (s8){u->window, b.out - u->window};
            b.out    = u->window;
            b.outlen = UZ_WSIZE;
        }
        crc = crc32_update(crc, data.s, data.len, &u->crccpu);
        iz keep = (iz)MIN(data.len, MAX(usize-out, 0));
        out += data.len;
        if (u->cflag && keep) {
            writer_write(u->out, data.s, keep);
            if (u->out->err) {
                info(u, MSG_STDERR, JOIN(&scratch, sname,
                     S(":  write error (disk full?).\n")));
                return PK_DISK;
            }
        } else if (disk && disk->target) {
            bytecopy(disk->target+out-data.len, data.s, keep);
        } else if (disk && keep && disk->fd>=0 &&
                   !os_write(u->ctx, disk->fd, data.s, keep)) {
            disk->goon = disk_error(u, sname, scratch);
            return PK_DISK;
        }
    }
    b32 overrun = out > usize;  // more than it says it holds
    bad |= overrun && crc==e->crc;

    if (bad) {
        s8 what = S("invalid compressed data to inflate");
        if (named) {
            info(u, MSG_STDERR, JOIN(&scratch, S("  error:  "), what, S(" "),
                                     sname, S("\n")));
        } else {
            info(u, MSG_STDERR, JOIN(&scratch, S("\n  error:  "), what,
                                     S("\n")));
        }
        return PK_ERR;
    }
    if (crc != e->crc) {
        if (named) {
            info(u, MSG_STDERR, JOIN(&scratch, lj(&scratch, sname, 22),
                                     S(" ")));
        }
        info(u, MSG_STDERR, JOIN(&scratch, S(" bad CRC "), hex8(&scratch, crc),
                                 S("  (should be "), hex8(&scratch, e->crc),
                                 S(")\n")));
        return PK_ERR;
    }
    if (u->tflag) {
        if (eflen) {
            if (u->qflag) {
                info(u, MSG_STDERR, JOIN(&scratch, lj(&scratch, sname, 22),
                                         S(" ")));
            }
            info(u, MSG_STDERR, JOIN(&scratch,
                 S("bad extra-field entry:\n      EF block length ("),
                 unum(&scratch, eflen, 0, 0),
                 S(" bytes) exceeds remaining EF data ("),
                 unum(&scratch, efrem, 0, 0), S(" bytes)\n")));
            return PK_ERR;
        }
        if (!u->qflag) {
            info(u, 0, S(" OK\n"));
        }
    } else if (!u->qflag && !disk) {
        info(u, 0, S("\n"));
    }
    return PK_OK;
}

// Shift every entry's offset, and the spans, as UnZip changes its
// extra_bytes.
static void reshift(zarchive *ar, zspans *spans, i64 by)
{
    for (i64 i = 0; i < ar->end.count; i++) {
        ar->entries[i].offset += by;
    }
    for (iz i = 0; i < spans->len; i++) {
        spans->beg[i] += by;
    }
    ar->shift += by;
}

// Find an entry's local header as extract.c does, reporting a bad
// offset. Where data before the archive was taken to shift its offsets,
// a first entry not found that way is looked for without the shift, and
// should a later one then not be found, with it again. Returns 0 for a
// header found, else a status, with *stop set for one that ends the
// archive's processing.
static i32 find_local(unzip *u, zarchive *ar, zentry *e, i64 filnum,
                      zspans *spans, zlocal *l, b32 *stop, arena scratch)
{
    i32 err = PK_OK;
    for (b32 again = 0;; again = 1) {
        s8 why = {0};
        switch (zar_local(ar, e, l)) {
        case ZAR_OK:
            return err;
        case ZAR_EREAD:
            writer_flush(u->out);
            info(u, MSG_STDERR, S("error:  zipfile read error\n"));
            writer_flush(u->out);
            os_exit(u->ctx, PK_BADERR);  // as UnZip's readbyte
        case ZAR_EEOF:
            why = S("EOF");
            break;
        default:
            if (get32(l->fixed) == ZIP_LOCAL_SIG) {
                // Its data reaches into the central directory
                info(u, MSG_STDERR, bomb_msg);
                *stop = 1;
                return PK_BOMB;
            }
            why = S("local header sig");
        }
        info(u, MSG_STDERR, JOIN(&scratch, S("file #"),
             unum(&scratch, (u64)filnum, 0, 0), S(":  bad zipfile offset ("),
             why, S("):  "), znum(&scratch, e->offset), S("\n")));
        b32 retry = (filnum==1 && ar->shift) || (!ar->shift && u->oldshift);
        if (again || zequals(why, S("EOF")) || !retry) {
            return again || why.len==3 ? PK_BADERR : PK_ERR;
        }
        info(u, MSG_STDERR, S("  (attempting to re-compensate)\n"));
        if (ar->shift) {
            u->oldshift = ar->shift;
            reshift(ar, spans, -ar->shift);
        } else {
            reshift(ar, spans, u->oldshift);
        }
        err = PK_ERR;
    }
}

// Extracting to disk (extract.c's extract_or_test_entrylist, and
// unix/unix.c's mapname, checkdir, and close_outfile)

// Results of making an entry's directories, as UnZip's MPN_*
enum { MPN_OK, MPN_INF_SKIP, MPN_ERR_SKIP };

// Write a message as C's perror writes it, straight to standard error,
// as UnZip's do for a few failures.
static void perror_(unzip *u, s8 what, s8 why, arena scratch)
{
    writer_flush(u->out);
    s8 msg = JOIN(&scratch, what, S(": "), why, S("\n"));
    os_write(u->ctx, 2, msg.s, msg.len);
}

// Make the directories along full, a path below the -d directory: those
// before its last '/', and given all, full itself, as UnZip's checkdir
// makes them (APPEND_DIR), or with create false, find a missing one to
// skip the entry, silently. A link is not a directory, though one leads
// to a directory (a departure: UnZip follows it), so that no entry is
// written through one, but for the -d directory itself. Those found are
// remembered (u->checked), as by linkless, so that a run of entries in
// one directory examines it once. Sets *made if one was made. Returns
// MPN_*, an error reported, naming the entry by name.
static i32 make_dirs(unzip *u, s8 full, b32 all, b32 create, b32 *made,
                     s8 name, arena scratch)
{
    os *ctx  = u->ctx;
    s8  path = all ? JOIN(&scratch, full, S("/")) : full;
    iz  from = u->root.len;
    s8  done = u->checked;
    *made = 0;
    if (!linkless(ctx, path, from, &done, scratch)) {
        for (iz k = 0; k<path.len && k<done.len && path.s[k]==done.s[k]; k++) {
            from = path.s[k]=='/' ? MAX(k+1, from) : from;
        }
        for (iz k = from; k < path.len; k++) {
            if (path.s[k] != '/') {
                continue;
            }
            arena   tmp  = scratch;
            s8      dir  = {path.s, k};
            os_info st   = {0};
            if (os_stat(ctx, dir, 0, &st, tmp)) {
                if (st.type == FT_DIR) {
                    continue;
                }
                info(u, MSG_STDERR, JOIN(&tmp, S("checkdir error:  "),
                     shown(u, dir, &tmp), S(" exists but is not directory\n"
                     "                 unable to process "),
                     shown(u, name, &tmp), S(".\n")));
                return MPN_ERR_SKIP;
            } else if (!create) {
                return MPN_INF_SKIP;  // freshening: nothing there to freshen
            } else if (!os_mkdir(ctx, dir, tmp)) {
                s8 why = JOIN(&tmp, os_error(ctx));
                if (os_stat(ctx, dir, 0, &st, tmp) && st.type==FT_DIR) {
                    continue;  // made meanwhile
                }
                info(u, MSG_STDERR, JOIN(&tmp, S("checkdir error:  cannot "
                     "create "), shown(u, dir, &tmp), S("\n                 "),
                     why, S("\n                 unable to process "),
                     shown(u, name, &tmp), S(".\n")));
                return MPN_ERR_SKIP;
            }
            *made = 1;
        }
        iz last = 0;
        for (iz k = 0; k < path.len; k++) {
            last = path.s[k]=='/' ? k+1 : last;
        }
        done = (s8){path.s, last};
    }

    // Remember them, where the room planned for that holds them
    u->checked.len = 0;
    if (done.len <= u->checkcap) {
        bytemove(u->checked.s, done.s, done.len);
        u->checked.len = done.len;
    }
    return MPN_OK;
}

// An MS-DOS time as Unix seconds, in local time, as UnZip's
// dos_to_unix_time.
static i64 dos_unix(unzip *u, u32 dostime)
{
    i32 tm[6];
    uz_dosdate(dostime, tm);
    return os_mktime(u->ctx, tm);
}

// Results of check_for_newer
enum { DOES_NOT_EXIST = -1, EXISTS_AND_OLDER, EXISTS_AND_NEWER };

// Whether a file is at full, and if so, whether it is as new as the
// entry, as fileio.c's check_for_newer: by the entry's local extended
// timestamp, else its DOS time, to which the file's time is rounded up.
// A link, whose time does not count, is older, with a note.
static i32 check_for_newer(unzip *u, s8 full, uzizux *ux, u32 dostime,
                           arena scratch)
{
    os     *ctx  = u->ctx;
    os_info st   = {0};
    os_info lst  = {0};
    b32     note = !u->qflag && u->overwrite!=OVERWRT_ALWAYS;
    if (!os_stat(ctx, full, 1, &st, scratch)) {
        if (!os_stat(ctx, full, 0, &lst, scratch)) {
            return DOES_NOT_EXIST;
        }
        if (note) {
            info(u, 0, JOIN(&scratch, shown(u, full, &scratch),
                 S(" exists and is a symbolic link with no real file.\n")));
        }
        return EXISTS_AND_OLDER;
    }
    if (os_stat(ctx, full, 0, &lst, scratch) && lst.type==FT_LINK) {
        if (note) {
            info(u, 0, JOIN(&scratch, shown(u, full, &scratch),
                            S(" exists and is a symbolic link.\n")));
        }
        return EXISTS_AND_OLDER;
    }
    i64 existing = st.mtime;
    i64 archive  = ux->mtime;
    if (!(ux->flags & UZ_MTIME)) {
        existing += existing & 1;  // to MS-DOS's two seconds
        archive   = dos_unix(u, dostime);
    }
    return existing>=archive ? EXISTS_AND_NEWER : EXISTS_AND_OLDER;
}

// The longest new name read at the prompt, a line of UnZip's FILNAMSIZ
enum { UZ_NEWNAME = 4096 };

// Ask whether to replace the file at full, as extract.c does, reading
// the answer from standard input. Returns 'y' to replace it, 'n' to
// skip the entry, or 'r' with *rename the name to extract it as, read
// into buf (UZ_NEWNAME bytes), or with none read, a null string, for
// the name it had. A or N answers the rest of the run too.
static u8 ask_replace(unzip *u, s8 full, i32 *err, s8 *rename, u8 *buf,
                      arena scratch)
{
    for (;;) {
        arena tmp = scratch;
        info(u, MSG_STDERR, JOIN(&tmp, S("replace "), shown(u, full, &tmp),
             S("? [y]es, [n]o, [A]ll, [N]one, [r]ename: ")));
        u8 answer[10];  // as UnZip's answerbuf, a longer answer split
        iz n = read_answer(u, answer, countof(answer));
        if (n < 0) {
            info(u, MSG_STDERR, S(" NULL\n(EOF or read error, treating as "
                                  "\"[N]one\" ...)\n"));
            *err = MAX(*err, PK_WARN);  // not extracted: a warning
            answer[0] = 'N';
        }
        switch (answer[0]) {
        case 'r':
        case 'R':
            for (;;) {
                info(u, MSG_STDERR, S("new name: "));
                iz len = read_answer(u, buf, UZ_NEWNAME);
                if (len < 0) {
                    *rename = (s8){0};  // UnZip keeps the name it had
                    return 'r';
                }
                len -= buf[len-1] == '\n';
                if (len) {
                    *rename = (s8){buf, len};
                    return 'r';
                }
            }
        case 'A':
            u->overwrite = OVERWRT_ALWAYS;
            return 'y';
        case 'y':
        case 'Y':
            return 'y';
        case 'N':
            u->overwrite = OVERWRT_NEVER;
            return 'n';
        case 'n':
            return 'n';
        }
        s8 bad = {answer, n};
        if (answer[0]=='\n' || answer[0]=='\r') {
            bad = S("{ENTER}");
        } else {
            bad.len -= bad.s[bad.len-1] == '\n';
        }
        info(u, MSG_STDERR, JOIN(&tmp, S("error:  invalid response ["),
                                 shown(u, bad, &tmp), S("]\n")));
    }
}

// Report the attributes that os_setattrs, os_setdirattrs, or os_symlink
// failed to give the file at full, as UnZip words each: within the line
// naming it, with an item that is (inline), else alone. Returns a
// status, a warning for any failure but a file's mode, which UnZip only
// reports.
static i32 attr_failures(unzip *u, i32 failed, s8 full, osattrs *a, s8 *why,
                         b32 dir, b32 inline_, arena scratch)
{
    i32 err  = PK_OK;
    s8  name = shown(u, full, &scratch);
    if (failed & OS_AOWNER) {
        s8 ids = JOIN(&scratch, S("cannot set UID "),
                      unum(&scratch, a->uid, 0, 0), S(" and/or GID "),
                      unum(&scratch, a->gid, 0, 0));
        info(u, MSG_STDERR, inline_
             ? JOIN(&scratch, S(" (warning) "), ids, S("\n          "),
                    why[0])
             : JOIN(&scratch, S("warning:  "), ids, S(" for "), name,
                    S("\n          "), why[0], S("\n")));
        err = dir ? PK_WARN : err;
    }
    if (dir && (failed & OS_ATIMES)) {
        info(u, MSG_STDERR, JOIN(&scratch, S("warning:  cannot set modif./"
             "access times for "), name, S("\n          "), why[2],
             S("\n")));
        err = PK_WARN;
    }
    if (failed & OS_AMODE) {
        if (dir) {
            info(u, MSG_STDERR, JOIN(&scratch, S("warning:  cannot set "
                 "permissions for "), name, S("\n          "), why[1],
                 S("\n")));
            err = PK_WARN;
        } else {
            perror_(u, S("fchmod (file attributes) error"), why[1], scratch);
        }
    }
    if (!dir && (failed & OS_ATIMES)) {
        info(u, MSG_STDERR, inline_
             ? JOIN(&scratch, S(" (warning) cannot set modif./access times"
                                "\n          "), why[2])
             : JOIN(&scratch, S("warning:  cannot set modif./access times "
                    "for "), name, S("\n          "), why[2], S("\n")));
    }
    return err;
}

// Extract an entry, its local header found at l, its extraction planned
// in x, as extract.c's extract_or_test_entrylist and
// extract_or_test_member. Returns a status, with *stop set when a write
// failed and the user does not go on.
static i32 extract_member(unzip *u, zarchive *ar, zentry *e, xentry *x,
                          zlocal *l, i64 usize, b32 *stop, arena scratch)
{
    os    *ctx  = u->ctx;
    i32    err  = PK_OK;
    u32    dost = get32(l->fixed+10);  // the local header's, as UnZip's
    uzizux ux   = uz_extra_izux(l->extra, 0, dost);  // (the window moves)
    uzmode um   = uz_mode(e->extattr, e->made, e->name, e->cextra,
                          u->Kflag);
    b32    link = um.symlink && usize>0;
    i32    opts = (u->windows ? UZ_WINDOWS : 0) | (u->jflag ? UZ_JUNK : 0) |
                  (u->Vflag ? UZ_KEEPVER : 0);
    u32    mask = um.umask ? u->umask : 0;
    uzpath mp   = x->map;
    s8     full = x->full;  // planned for a directory with its '/'
    i32    have = DOES_NOT_EXIST;
    u8    *newname = newstr(&scratch, UZ_NEWNAME);  // from the prompt
    arena  mark    = scratch;  // reset for each new name's path
    full.len -= full.s && (mp.flags & UZ_DIR);
    for (b32 renamed = 0;; renamed = 1) {
        if (renamed || !full.s) {
            full = JOIN(&scratch, u->root, mp.path);
        }

        // A name from MS-DOS may separate with '\', and leading '/' are
        // dropped, as extract.c warns
        if ((mp.flags & UZ_BACKSLASH) && !u->slashed) {
            info(u, MSG_STDERR|MSG_LNEWLN, JOIN(&scratch, S("warning:  "),
                 u->zipfn, S(" appears to use backslashes as path "
                             "separators\n")));
            u->slashed = 1;
            err = MAX(err, PK_WARN);
        }
        if (!renamed && (mp.flags & UZ_ABSOLUTE)) {
            info(u, MSG_STDERR, JOIN(&scratch, S("warning:  stripped absolute"
                 " path spec from "), shown(u, mp.full, &scratch), S("\n")));
            err = MAX(err, PK_WARN);
        }

        // Its directories, as mapname makes them, then its warning of
        // ".." dropped, then a directory entry is done
        b32 isdir = (mp.flags & UZ_DIR) != 0;
        b32 made  = 0;
        s8  dirs  = full;
        b32 all   = isdir;
        b32 walk  = !isdir || mp.path.len;
        if (mp.flags & UZ_FAILED) {
            // A name that maps to nothing still has its directories
            s8 pre = mp.name;
            for (; pre.len && pre.s[pre.len-1]!='/'; pre.len--) {}
            uzpath dp = uz_mapname(pre, e->made, opts, &scratch);
            dirs = JOIN(&scratch, u->root, dp.path);
            all  = 1;
            walk = dp.path.len > 0;
        }
        if (walk) {
            i32 r = make_dirs(u, dirs, all, !u->fflag || renamed, &made,
                              mp.name, scratch);
            if (r == MPN_ERR_SKIP) {
                return MAX(err, PK_ERR);
            } else if (r == MPN_INF_SKIP) {
                return err;
            }
        }
        if ((mp.flags & UZ_DOTDOT) && !u->qflag) {
            info(u, 0, JOIN(&scratch, S("warning:  skipped \"../\" path "
                 "component(s) in "), shown(u, mp.name, &scratch), S("\n")));
            err = MAX(err, PK_WARN);
        }
        if (isdir) {
            if (!made) {
                return err;  // it was there: nothing to do
            }
            s8 path = !renamed && x->full.s ? x->full :
                                              JOIN(&u->perm, full, S("/"));
            if (!u->qflag) {
                info(u, 0, JOIN(&scratch, S("   creating: "),
                                shown(u, path, &scratch), S("\n")));
            }
            xdir *d = push(&u->perm, &u->dirs);
            d->path = path;
            d->attrs.mode   = um.mode & 07777 & ~mask;
            d->attrs.flags  = OS_AMODE;
            d->attrs.flags |= uz_host(e->made)!=UZ_UNIX ||
                              !(u->Xflag || u->Kflag) ? OS_ASGID : 0;
            if (u->Dflag <= 0) {
                d->attrs.mtime  = ux.flags & UZ_MTIME ? ux.mtime :
                                                        dos_unix(u, dost);
                d->attrs.atime  = ux.flags & UZ_ATIME ? ux.atime :
                                                        d->attrs.mtime;
                d->attrs.flags |= OS_ATIMES;
            }
            if (u->Xflag && (ux.flags & UZ_OWNER)) {
                d->attrs.uid    = ux.uid;
                d->attrs.gid    = ux.gid;
                d->attrs.flags |= OS_AOWNER;
            }
            return err;
        }
        if (mp.flags & UZ_FAILED) {
            info(u, MSG_STDERR, JOIN(&scratch, S("mapname:  conversion of "),
                 shown(u, mp.name, &scratch), S(" failed\n")));
            return MAX(err, PK_ERR);
        }

        // What is there already, and what to do about it
        have = check_for_newer(u, full, &ux, dost, scratch);
        b32 query = 0;
        b32 skip  = 0;
        switch (have) {
        case DOES_NOT_EXIST:
            skip = u->fflag && !renamed;  // freshening creates nothing
            break;
        case EXISTS_AND_OLDER:
            skip  = u->overwrite == OVERWRT_NEVER;
            query = !skip && u->overwrite!=OVERWRT_ALWAYS;
            break;
        case EXISTS_AND_NEWER:
            skip  = u->overwrite==OVERWRT_NEVER || (u->uflag && !renamed);
            query = !skip && u->overwrite!=OVERWRT_ALWAYS;
            break;
        }
        if (query) {
            s8 rename = {0};
            u8 a = ask_replace(u, full, &err, &rename, newname, scratch);
            if (a=='r' && rename.s) {
                // Each new name in the same room, however many are given
                scratch = mark;
                mp = uz_mapname(rename, e->made, opts, &scratch);
                continue;
            } else if (a == 'r') {
                mp.flags &= ~UZ_DOTDOT;  // the name it had, warned of
                continue;
            }
            skip = a == 'n';
        }
        if (skip) {
            return err;
        }
        break;
    }

    // Replace what is there, removing it, as open_outfile does, so as
    // never to write through a link, then creating the file anew, or for
    // a link, an empty file to hold its place until links are made
    if (have!=DOES_NOT_EXIST && !os_remove(ctx, full, scratch) &&
        !os_missing(ctx)) {
        info(u, MSG_STDERR, JOIN(&scratch, S("error:  cannot delete old "),
             shown(u, full, &scratch), S("\n        "), os_error(ctx),
             S("\n")));
        return MAX(err, PK_DISK);
    }
    i32 fd = os_open(ctx, full, OS_CREATE, scratch);
    if (fd < 0) {
        info(u, MSG_STDERR, JOIN(&scratch, S("error:  cannot create "),
             shown(u, full, &scratch), S("\n        "), os_error(ctx),
             S("\n")));
        return MAX(err, PK_DISK);
    }

    xout out = {fd, link ? x->target : 0, link, 0};
    i32  r   = test_member(u, ar, e, l, usize, full, &out, scratch);
    if (r > PK_WARN) {
        os_close(ctx, fd);  // discarded
        *stop = r==PK_DISK && !out.goon;
        return MAX(err, r);
    }

    if (link && !out.target) {
        os_close(ctx, fd);
        info(u, MSG_STDERR, JOIN(&scratch, S("warning:  symbolic link ("),
             shown(u, full, &scratch), S(") failed: target too long\n")));
    } else if (link) {
        // The placeholder is kept, and known by its identity
        os_info id = {0};
        b32 ok = os_fstat(ctx, fd, &id) && os_keep(ctx, fd);
        if (!os_close(ctx, fd) || !ok) {
            os_remove(ctx, full, scratch);
            out.goon = disk_error(u, shown(u, full, &scratch), scratch);
            *stop = !out.goon;
            return MAX(err, PK_DISK);
        }
        s8 target = {out.target, (iz)usize};
        if (!u->qflag) {
            info(u, 0, JOIN(&scratch, S("-> "), shown(u, target, &scratch),
                            S(" ")));
        }
        xlink *k = push(&u->perm, &u->links);
        k->path   = full.s==x->full.s ? full : JOIN(&u->perm, full);
        k->target = target;
        k->dev    = id.dev;
        k->ino[0] = id.ino[0];
        k->ino[1] = id.ino[1];
        if (u->Xflag && (ux.flags & UZ_OWNER)) {
            k->attrs.uid   = ux.uid;
            k->attrs.gid   = ux.gid;
            k->attrs.flags = OS_AOWNER;
        }
    } else {
        // Its attributes, then it is kept, as close_outfile gives them
        osattrs a = {0};
        a.mode    = um.mode & 07777 & ~mask;
        a.dosattr = uz_dosattr(e->extattr);
        a.flags   = OS_AMODE;
        if (u->Dflag <= 1) {
            a.mtime  = ux.flags & UZ_MTIME ? ux.mtime : dos_unix(u, dost);
            a.atime  = ux.flags & UZ_ATIME ? ux.atime : a.mtime;
            a.flags |= OS_ATIMES;
        }
        if (u->Xflag && (ux.flags & UZ_OWNER)) {
            a.uid    = ux.uid;
            a.gid    = ux.gid;
            a.flags |= OS_AOWNER;
        }
        s8  why[OS_AWHY] = {0};
        i32 failed = os_setattrs(ctx, fd, &a, why, &scratch);
        attr_failures(u, failed, full, &a, why, 0, !u->qflag, scratch);

        // A failed close may have lost data
        b32 kept = os_keep(ctx, fd);
        if (!kept || !os_close(ctx, fd)) {
            if (kept) {
                os_remove(ctx, full, scratch);
            } else {
                os_close(ctx, fd);
            }
            out.goon = disk_error(u, shown(u, full, &scratch), scratch);
            *stop = !out.goon;
            return MAX(err, PK_DISK);
        }
    }
    if (!u->qflag) {
        info(u, 0, S("\n"));
    }
    return err;
}

// Create the links deferred (extract.c's set_deferred_symlink), each
// where its placeholder still is, reached through no link. Returns a
// status, which, as UnZip's, failures leave alone.
static i32 finish_links(unzip *u, arena scratch)
{
    os *ctx = u->ctx;
    if (u->links.len && !u->qflag) {
        info(u, 0, S("finishing deferred symbolic links:\n"));
    }
    for (iz i = 0; i < u->links.len; i++) {
        arena   tmp  = scratch;
        xlink  *k    = u->links.data + i;
        s8      none = {0};
        os_info st   = {0};
        b32 ok = linkless(ctx, k->path, u->root.len, &none, tmp) &&
                 os_stat(ctx, k->path, 0, &st, tmp) && st.type==FT_FILE &&
                 !st.size && st.dev==k->dev && st.ino[0]==k->ino[0] &&
                 st.ino[1]==k->ino[1];
        if (!ok) {
            info(u, MSG_STDERR, JOIN(&tmp, S("warning:  deferred symlink ("),
                 shown(u, k->path, &tmp), S(") failed:\n          invalid "
                 "placeholder file\n")));
            continue;
        }
        os_remove(ctx, k->path, tmp);
        if (!u->qflag) {
            info(u, 0, JOIN(&tmp, S("  "), lj(&tmp, shown(u, k->path, &tmp),
                 22), S(" -> "), shown(u, k->target, &tmp), S("\n")));
        }
        s8  why[OS_AWHY] = {0};
        i32 failed = os_symlink(ctx, k->target, k->path, &k->attrs, why,
                                &tmp);
        if (failed & OS_ALINK) {
            perror_(u, S("symlink error"), why[3], tmp);
        } else if (failed) {
            attr_failures(u, failed, k->path, &k->attrs, why, 0, 0, tmp);
        }
    }
    return PK_OK;
}

// Order paths as strcmp does, descending.
static b32 path_after(s8 a, s8 b)
{
    iz n = MIN(a.len, b.len);
    for (iz i = 0; i < n; i++) {
        if (a.s[i] != b.s[i]) {
            return a.s[i] > b.s[i];
        }
    }
    return a.len > b.len;
}

// Give the directories created by their own entries their attributes,
// deepest first, as extract.c does (SET_DIR_ATTRIB): their paths sorted
// in reverse, each with its '/'. Returns a status.
static i32 finish_dirs(unzip *u, arena scratch)
{
    os   *ctx = u->ctx;
    iz    n   = u->dirs.len;
    xdir *d   = u->dirs.data;
    xdir *t   = new(&scratch, n, xdir);
    for (iz w = 1; w < n; w *= 2) {  // a stable bottom-up merge sort
        for (iz lo = 0; lo < n; lo += 2*w) {
            iz mid = MIN(lo+w, n);
            iz hi  = MIN(lo+2*w, n);
            iz i = lo, j = mid, k = lo;
            while (i<mid || j<hi) {
                b32 right = i==mid || (j<hi && path_after(d[j].path,
                                                          d[i].path));
                t[k++] = right ? d[j++] : d[i++];
            }
        }
        bytecopy(d, t, n*(iz)sizeof(*d));
    }

    i32 err   = PK_OK;
    u64 nfail = 0;
    for (iz i = 0; i < n; i++) {
        arena tmp  = scratch;
        s8    path = {d[i].path.s, d[i].path.len-1};  // without its '/'
        s8    none = {0};
        s8    why[OS_AWHY] = {0};
        i32   failed = 0;
        if (!linkless(ctx, path, u->root.len, &none, tmp)) {
            failed = d[i].attrs.flags & (OS_AOWNER|OS_AMODE|OS_ATIMES);
            why[0] = why[1] = why[2] = S("Not a directory");
        } else {
            failed = os_setdirattrs(ctx, path, &d[i].attrs, why, &tmp);
        }
        if (failed) {
            i32 r = attr_failures(u, failed, d[i].path, &d[i].attrs, why, 1,
                                  0, tmp);
            err = err ? err : r;
            nfail++;
            info(u, MSG_STDERR, JOIN(&tmp, S("warning:  set times/attribs "
                 "failed for "), shown(u, d[i].path, &tmp), S("\n")));
        }
    }
    if (nfail && !u->qflag) {
        info(u, 0, JOIN(&scratch, S("     failed setting times/attribs for "),
                        unum(&scratch, nfail, 0, 0), S(" dir entries")));
    }
    return err;
}

// Find, check, and process one entry, the filnum'th, as extract.c's
// extract_or_test_entrylist. Returns a status, with *stop set for one
// that ends the archive's processing.
static i32 do_member(unzip *u, zarchive *ar, zentry *e, xentry *x,
                     i64 filnum, zspans *spans, b32 *stop, arena scratch)
{
    s8     name = entry_name(ar, e);
    zlocal l    = {0};
    i32    err  = find_local(u, ar, e, filnum, spans, &l, stop, scratch);
    if (*stop || err>PK_ERR || (err && !l.data)) {
        return err;
    }
    if (l.data+e->csize > next_span(ar, spans, e->offset)) {
        info(u, MSG_STDERR, bomb_msg);  // reaches into the next entry
        *stop = 1;
        return PK_BOMB;
    }

    // The names compare as Debian's UnZip compares them, each in Unicode
    // where its own extra fields give that
    s8 lname = l.name;
    if (!(e->flags & ZIP_FLAG_UTF8)) {
        u32 crc = crc32_update(0, l.name.s, l.name.len, &u->crccpu);
        s8  un  = zip_extra_upath(l.extra, crc);
        lname = un.s && zip_utf8(un)>=0 && !zip_has(un, 0) ? un : lname;
    }
    if (!zequals(lname, name)) {
        arena tmp = scratch;
        info(u, MSG_STDERR, JOIN(&tmp, shown(u, name, &tmp),
             S(":  mismatching \"local\" filename ("),
             shown(u, lname, &tmp), S("),\n         continuing with "
             "\"central\" filename version\n")));
        err = PK_WARN;
    }
    i64 usize = e->usize;
    if (e->method==ZIP_STORE && e->usize!=e->csize) {
        arena tmp = scratch;
        info(u, MSG_STDERR, JOIN(&tmp, shown(u, name, &tmp), S(":  ucsize "),
             znum(&tmp, e->usize), S(" <> csize "), znum(&tmp, e->csize),
             S(" for STORED entry\n         continuing with \"compressed\" "
               "size value\n")));
        usize = e->csize;
        err   = PK_WARN;
    }

    i32 r = 0;
    if (u->extract) {
        r = extract_member(u, ar, e, x, &l, usize, stop, scratch);
        return MAX(err, r);
    }
    r = test_member(u, ar, e, &l, usize, name, 0, scratch);
    *stop = r == PK_DISK;
    return MAX(err, r);
}

// Plan the extraction of the selected entries, before any output: their
// paths, room for the paths of links and directories, kept to finish
// them at the end, and for links' targets, and lists for those.
static xentry *plan_extract(unzip *u, zarchive *ar, arena *scratch)
{
    i64     count   = u->nentries;
    xentry *xs      = new(scratch, (iz)count, xentry);
    i32     opts    = (u->windows ? UZ_WINDOWS : 0) |
                      (u->jflag ? UZ_JUNK : 0) | (u->Vflag ? UZ_KEEPVER : 0);
    iz      nlinks  = 0;
    iz      ndirs   = 0;
    iz      longest = 0;
    iz      maxname = 0;
    for (i64 i = 0; i < count; i++) {
        zentry *e    = ar->entries + i;
        xentry *x    = xs + i;
        s8      name = entry_name(ar, e);
        if (!readable(e) || !wanted(u, name, 0, 0)) {
            continue;
        }
        maxname = MAX(maxname, name.len);
        x->map  = uz_mapname(name, e->made, opts, scratch);
        longest = MAX(longest, u->root.len + x->map.path.len + 1);
        if (x->map.flags & UZ_DIR) {
            if (x->map.path.len) {
                x->full = JOIN(scratch, u->root, x->map.path, S("/"));
                ndirs++;
            }
            continue;
        } else if (x->map.flags & UZ_FAILED) {
            continue;
        }
        uzmode um   = uz_mode(e->extattr, e->made, e->name, e->cextra,
                              u->Kflag);
        i64    size = e->method==ZIP_STORE ? e->csize : e->usize;
        if (um.symlink && size>0) {
            x->full   = JOIN(scratch, u->root, x->map.path);
            x->target = size<=UZ_LINKMAX ? newstr(scratch, (iz)size) : 0;
            nlinks++;
        }
    }
    u->links    = (xlinks){new(scratch, nlinks, xlink), 0, nlinks};
    u->dirs     = (xdirs){new(scratch, ndirs, xdir), 0, ndirs};
    u->checked  = (s8){newstr(scratch, longest), 0};
    u->checkcap = longest;
    u->slashed  = 0;

    // The room that extracting an entry takes, its messages and paths, a
    // new name asked for, and sorting the directories at the end
    u->room = 16*(longest + maxname) + (1<<16) + ndirs*(iz)sizeof(xdir);
    return xs;
}

// Make the -d directory, if not there, as UnZip's checkdir (ROOT) makes
// it, once, in one level, unless freshening (a departure: UnZip then
// freshens the current directory). Returns a status.
static i32 make_root(unzip *u, arena scratch)
{
    s8      dir  = {u->root.s, u->root.len-1};  // as UnZip, without a '/'
    os_info st   = {0};
    if (u->rooted || dir.len<=0 || u->fflag) {
        return PK_OK;
    } else if (!os_stat(u->ctx, dir, 1, &st, scratch) ||
               st.type!=FT_DIR) {
        if (!os_mkdir(u->ctx, dir, scratch)) {
            info(u, MSG_STDERR, JOIN(&scratch, S("checkdir:  cannot create "
                 "extraction directory: "), shown(u, dir, &scratch),
                 S("\n           "), os_error(u->ctx), S("\n")));
            return PK_ERR;
        }
    }
    u->rooted = 1;
    return PK_OK;
}

// Test the selected entries, or extract them, to disk or standard
// output, as extract.c's extract_or_test_files.
static i32 extract_or_test(unzip *u, zarchive *ar, arena scratch)
{
    i64 count = u->nentries;
    u8 *fm    = new(&scratch, u->fspecs.len, u8);
    u8 *xm    = new(&scratch, u->xspecs.len, u8);

    xentry *xs = u->extract ? plan_extract(u, ar, &scratch) : 0;

    // The entries to be read must not overlap, nor reach into the
    // central directory, as Debian's UnZip finds them out: here, before
    // any are read, the least each could take, a local header and its
    // data, and once each local header is read, what it does take
    zspans spans = {new(&scratch, (iz)count, i64), 0};
    i64   *ends  = new(&scratch, (iz)count, i64);
    for (i64 i = 0; i < count; i++) {
        zentry *e = ar->entries + i;
        if (readable(e) && wanted(u, entry_name(ar, e), 0, 0)) {
            ends[spans.len]       = e->offset + ZIP_LOCAL_LEN + e->csize;
            spans.beg[spans.len++] = e->offset;
        }
    }
    sort_spans(spans.beg, ends, spans.len, scratch);
    for (iz k = 0; k < spans.len; k++) {
        i64 next = k+1<spans.len ? spans.beg[k+1] : ar->end.cdoff;
        if (ends[k] > next) {
            info(u, MSG_STDERR, bomb_msg);
            return PK_BOMB;
        }
    }

    i32  err     = PK_OK;
    i64  filnum  = 0;
    u64  skipped = 0;
    iz  *block   = new(&scratch, DIR_BLKSIZ, iz);
    iz   nblock  = 0;
    b32  stop    = 0;

    // Claimed, the room that extracting each entry takes, so that none is
    // claimed once files are written (but for a new name, from the
    // prompt, for a link or directory, kept to the end), then the -d
    // directory made
    if (u->extract) {
        arena probe = scratch;
        newbytes(&probe, u->room);
        i32 r = make_root(u, scratch);
        if (r) {
            return r;
        }
    }

    for (i64 i = 0; i<=count && !stop; i++) {
        if (i==count && (u->cderr==CD_SIG || u->cderr==CD_NONAME)) {
            // As the scan finds it, before the entries of its block
            cd_error(u, nblock + filnum + 1, scratch);
            err = PK_BADERR;
        } else if (i < count) {
            arena   tmp  = scratch;
            zentry *e    = ar->entries + i;
            s8      name = entry_name(ar, e);
            if (!wanted(u, name, fm, xm)) {
                continue;
            } else if (!store_info(u, e, name, tmp)) {
                skipped++;
                continue;
            }
            block[nblock++] = (iz)i;
            if (nblock < DIR_BLKSIZ) {
                continue;
            }
        }
        for (iz k = 0; k<nblock && !stop; k++) {
            zentry *e = ar->entries + block[k];
            i32     r = do_member(u, ar, e, xs ? xs+block[k] : 0, ++filnum,
                                      &spans, &stop, scratch);
            err = MAX(err, r);
        }
        nblock = 0;
    }

    // Links, deferred, then directories' attributes, even when stopped
    if (u->extract) {
        i32 r = finish_links(u, scratch);
        err = MAX(err, r);
        r = finish_dirs(u, scratch);
        err = MAX(err, r);
    }
    if (stop) {
        return err;  // no summary, as when UnZip stops early
    }
    if (u->cderr == CD_END) {
        info(u, MSG_STDERR, JOIN(&scratch, end_sig_msg, report_msg));
        err = MAX(err, PK_WARN);
    }

    for (iz i = 0; i < u->fspecs.len; i++) {
        if (!fm[i]) {
            arena tmp = scratch;
            info(u, MSG_STDERR, JOIN(&tmp, S("caution: filename not matched:"
                                             "  "), u->fspecs.data[i],
                                     S("\n")));
            err = err<=PK_WARN ? PK_FIND : err;
        }
    }
    for (iz i = 0; i < u->xspecs.len; i++) {
        if (!xm[i]) {
            arena tmp = scratch;
            info(u, MSG_STDERR, JOIN(&tmp, S("caution: excluded filename not "
                                             "matched:  "), u->xspecs.data[i],
                                     S("\n")));
        }
    }

    if (u->tflag && u->qflag<2) {
        arena tmp = scratch;
        if (err) {
            info(u, 0, JOIN(&tmp, S("At least one "),
                            err==PK_WARN ? S("warning-") : S(""),
                            S("error was detected in "), u->zipfn, S(".\n")));
        } else if (!filnum) {
            info(u, 0, JOIN(&tmp, S("Caution:  zero files tested in "),
                            u->zipfn, S(".\n")));
        } else if (u->allfiles && !skipped) {
            info(u, 0, JOIN(&tmp, S("No errors detected in compressed data "
                                    "of "), u->zipfn, S(".\n")));
        } else {
            info(u, 0, JOIN(&tmp, S("No errors detected in "), u->zipfn,
                            S(" for the "), unum(&tmp, (u64)filnum, 0, 0),
                            S(" file"), plural((u64)filnum),
                            S(" tested.\n")));
        }
        if (skipped) {
            info(u, 0, JOIN(&tmp, unum(&tmp, skipped, 0, 0), S(" file"),
                            plural(skipped), S(" skipped because of "
                            "unsupported compression or encoding.\n")));
        }
    }

    if (!filnum && err<=PK_WARN) {
        err = skipped ? IZ_UNSUP : PK_FIND;
    } else if (skipped && err<=PK_WARN) {
        err = IZ_UNSUP;
    }
    return err;
}

// Archives (process.c)

static s8 const no_endsig = S8(
    "  End-of-central-directory signature not found.  Either this file is"
    " not\n"
    "  a zipfile, or it constitutes one disk of a multi-part archive.  In"
    " the\n"
    "  latter case the central directory and zipfile comment will be found"
    " on\n"
    "  the last disk(s) of this archive.\n"
);

// The central directory's offset as the end record has it, undoing
// zip_find_end's taking an empty one at offset 0 to be where it is.
static i64 raw_cdoff(zarchive *ar)
{
    zend *e = &ar->end;
    u8   *p = 0;
    if (!e->count && !e->cdsize && e->end64<0 && e->cdoff==e->endpos &&
        zin_get(&ar->in, e->endpos+16, 4, &p)>0) {
        return get32(p);
    }
    return e->cdoff;
}

// Process one archive, at path, or with stdin, standard input, as
// process.c's do_seekable. With lastchance, a missing one is reported.
// Returns a status, or PK_NOZIP or IZ_DIR for none there.
static i32 do_archive(unzip *u, s8 path, b32 lastchance, b32 stdin,
                      arena scratch)
{
    zarchive ar     = {0};
    i32      fd     = -1;
    i64      size   = 0;
    b32      exe    = 0;
    os_info  info_  = {0};
    if (stdin) {
        // Read in place if a regular file, else spooled into memory
        if (os_fstat(u->ctx, 0, &info_) && info_.type==FT_FILE) {
            fd   = 0;
            size = info_.size;
        } else {
            s8 data = read_all(u->ctx, 0, &u->perm);
            if (!data.s) {
                info(u, MSG_STDERR, S("error:  zipfile read error\n"));
                return PK_BADERR;
            }
            zin_memory(&ar.in, data);
            size = data.len;
        }
    } else {
        b32 found = os_stat(u->ctx, path, 1, &info_, scratch);
        if (!found || info_.type==FT_DIR) {
            if (lastchance && u->qflag<3) {
                arena tmp = scratch;
                s8    w   = u->zipspec;
                s8    msg = u->noecrec
                    ? JOIN(&tmp, S("unzip:  cannot find zipfile directory in "
                           "one of "), w, S(" or\n        "), w,
                           S(".zip, and cannot find "), path, S(", period.\n"))
                    : JOIN(&tmp, S("unzip:  cannot find or open "), w, S(", "),
                           w, S(".zip or "), path, S(".\n"));
                info(u, MSG_STDERR, msg);
            }
            return found ? IZ_DIR : PK_NOZIP;
        }
        exe = (info_.mode & 0100) != 0;
        fd  = os_open(u->ctx, path, OS_READ|OS_REGULAR, scratch);
        if (fd == OS_ENOTREG) {
            fd = -2;  // a device or FIFO: read as an empty file
        } else if (fd < 0) {
            arena tmp = scratch;
            info(u, MSG_STDERR, JOIN(&tmp, S("error:  cannot open zipfile [ "),
                 path, S(" ]\n        "), os_error(u->ctx), S("\n")));
            return PK_NOZIP;
        } else if (os_fstat(u->ctx, fd, &info_)) {
            size = info_.size;
        }
    }
    u->zipfn = path;

    i32 err = PK_OK;
    if (!u->qflag) {
        info(u, 0, JOIN(&scratch, S("Archive:  "), path, S("\n")));
    }

    i32 r = fd==-2 ? ZAR_ENOEND : zar_open(&ar, u->ctx, fd, size, &u->perm);
    if (r==ZAR_EREAD || r==ZAR_EEOF) {
        info(u, MSG_STDERR, S("error:  zipfile read error\n"));
        err = r==ZAR_EREAD ? PK_BADERR : PK_EOF;
        goto done;
    } else if (r == ZAR_ENOEND) {
        if (u->qflag) {
            info(u, MSG_STDERR, JOIN(&scratch, S("["), path, S("]\n")));
        }
        info(u, MSG_STDERR, no_endsig);
        if (exe) {
            info(u, MSG_STDERR, JOIN(&scratch, S("note:  "), path,
                 S(" may be a plain executable, not an archive\n")));
        }
        if (fd >= 0) {
            os_close(u->ctx, fd);
        }
        if (lastchance || stdin) {
            return stdin ? PK_NOZIP : PK_ERR;
        }
        u->noecrec = 1;  // perhaps the wrong file: try NAME.zip
        return PK_NOZIP;
    }

    // The comment, as find_ecrec shows it, before checking the records
    if (ar.end.comment.len && (u->zflag || !u->qflag)) {
        show_text(u, ar.end.comment, scratch);
    }
    if (u->zflag) {
        goto done;
    }

    // Data before the archive, unaccounted for by its offsets, shifts
    // them, as UnZip's extra_bytes; offsets past it are an error
    if (r==ZAR_EMULTI && ar.end.end64<0 && (ar.end.disk || ar.end.cddisk)) {
        // Disk numbers in the end record alone, which UnZip takes for
        // the parts of an archive concatenated, warning of the last one
        zend end = ar.end;
        end.disk = end.cddisk = 0;
        switch (zip_check_end32(&end)) {
        case ZIP_OK:
            r = ZAR_OK;
            break;
        case ZIP_EPREFIX:
            r = ZAR_EPREFIX;
            break;
        default:
            r = ZAR_EFORMAT;
        }
        if (ar.end.disk) {
            info(u, MSG_STDERR, JOIN(&scratch, S("warning ["), path,
                 S("]:  zipfile claims to be last disk of a multi-part "
                   "archive;\n  attempting to process anyway, assuming all "
                   "parts have been concatenated\n  together in order.  "
                   "Expect \"errors\" and warnings...true multi-part "
                   "support\n  doesn't exist yet (coming soon).\n")));
            err = PK_WARN;
        }
        ar.end = end;
    }
    i64 cdoff = raw_cdoff(&ar);
    i64 real  = ar.end.end64>=0 ? ar.end.end64 : ar.end.endpos;
    i64 extra = real - (cdoff + ar.end.cdsize);
    if (r == ZAR_EMULTI) {
        info(u, MSG_STDERR, JOIN(&scratch, S("\nerror ["), path,
             S("]:  zipfile is part of multi-disk archive\n"
               "  (sorry, not yet supported).\n")));
        err = PK_FIND;
        goto done;
    } else if (r==ZAR_EFORMAT && extra<0) {
        info(u, MSG_STDERR, JOIN(&scratch, S("error ["), path,
             S("]:  missing "), znum(&scratch, -extra),
             S(" bytes in zipfile\n  (attempting to process anyway)\n")));
        err = PK_ERR;
    } else if (extra > 0) {
        if (!cdoff && ar.end.cdsize) {
            info(u, MSG_STDERR, JOIN(&scratch, S("error ["), path,
                 S("]:  NULL central directory offset\n"
                   "  (attempting to process anyway)\n")));
            ar.end.cdoff = extra;
            err = PK_ERR;
        } else {
            info(u, MSG_STDERR, JOIN(&scratch, S("warning ["), path,
                 S("]:  "), znum(&scratch, extra), S(" extra byte"),
                 plural((u64)extra), S(" at beginning or within zipfile\n"
                 "  (attempting to process anyway)\n")));
            ar.shift     = extra;
            ar.end.cdoff = cdoff + extra;
            err = PK_WARN;
        }
    }
    if (!cdoff && !ar.end.cdsize) {
        info(u, MSG_STDERR, JOIN(&scratch, S("warning ["), path,
                                 S("]:  zipfile is empty\n")));
        err = MAX(err, PK_WARN);
        goto done;
    }

    // A directory invalid past its first entry is processed up to there,
    // as UnZip finds it out once it gets there
    u->nentries = ar.end.count;
    u->cderr    = CD_OK;
    u->oldshift = 0;
    r = r==ZAR_EFORMAT ? ZAR_EFORMAT : zar_entries(&ar, &u->perm, scratch);
    switch (r) {
    case ZAR_OK:
        break;
    case ZAR_EREAD:
    case ZAR_EEOF:
        info(u, MSG_STDERR, S("error:  zipfile read error\n"));
        err = r==ZAR_EREAD ? PK_BADERR : PK_EOF;
        goto done;
    case ZAR_ENONAME:
    case ZAR_EFORMAT:
        u->nentries = r==ZAR_ENONAME ? ar.noname : ar.bad;
        u->cderr    = r==ZAR_ENONAME ? CD_NONAME :
                      ar.bad==ar.end.count ? CD_END : CD_SIG;
        if (u->nentries <= 0) {
            info(u, MSG_STDERR, JOIN(&scratch, S("error ["), path,
                 S("]:  start of central directory not found;\n"
                   "  zipfile corrupt.\n"), report_msg));
            err = PK_BADERR;
            goto done;
        }
    }
    for (i64 i = 0; i < u->nentries; i++) {
        s8 stale = {0};
        s8 un    = zar_uname(u->ctx, ar.entries+i, u->windows, &u->crccpu,
                             &stale, &u->perm, scratch);
        if (un.s) {
            *push(&u->perm, &ar.unames) = (zuname){(iz)i, un};
        }
    }

    if (u->vflag && !u->tflag && !u->cflag) {
        r = list_files(u, &ar, scratch);
    } else {
        r = extract_or_test(u, &ar, scratch);
    }
    err = MAX(err, r);

done:
    if (fd >= 0) {
        os_close(u->ctx, fd);
    }
    return err;
}

static b32 add_match(void *data, s8 path, os_dirent *entry, arena scratch)
{
    (void)entry;
    (void)scratch;
    unzip *u = data;
    *push(&u->perm, &u->matches) = JOIN(&u->perm, path);
    return 1;
}

// Process the archives named, as process.c's process_zipfiles: each
// match of a wildcard, or the name as given, then if that alone was not
// there, with .zip and .ZIP appended, summarizing a wildcard's.
static i32 process_zipfiles(unzip *u, arena scratch)
{
    s8 spec = u->zipspec;
    if (zequals(spec, S("-"))) {
        i32 r = do_archive(u, spec, 1, 1, scratch);
        return r==IZ_DIR ? PK_NOZIP : r;
    }

    b32 wild = zip_haswild(spec, ZIP_SETS);
    if (wild) {
        i32 flags = ZIP_SETS | (u->windows ? WILD_WINDOWS : WILD_NODOTS);
        expand_wild(u->ctx, spec, 0, flags, add_match, u, scratch);
    }
    if (!u->matches.len) {
        *push(&u->perm, &u->matches) = spec;  // tried as it is
    }

    i32  status = 0;
    i32  r      = 0;
    iz   win = 0, lose = 0, warn = 0, dirs = 0, miss = 0;
    s8   last = {0};
    byte *mark = u->perm.beg;
    for (iz i = 0; i < u->matches.len; i++) {
        last = u->matches.data[i];
        if (!u->qflag && r!=PK_NOZIP && r!=IZ_DIR && win+lose+warn+miss) {
            info(u, 0, S("\n"));
        }
        r = do_archive(u, last, 0, 0, scratch);
        u->perm.beg = mark;  // each archive's memory is reused
        if (r == PK_WARN) {
            warn++;
        } else if (r == IZ_DIR) {
            dirs++;
        } else if (r == PK_NOZIP) {
            miss++;
        } else if (r) {
            lose++;
        } else {
            win++;
        }
        status = r!=IZ_DIR ? MAX(status, r) : status;
    }

    if (!win && !warn && !lose && dirs+miss==1) {
        // Again with the suffixes, the last reporting its absence
        s8 zip = JOIN(&u->perm, last, S(".zip"));
        dirs = miss = 0;
        status = PK_OK;
        r = do_archive(u, zip, 0, 0, scratch);
        if (r==PK_NOZIP || r==IZ_DIR) {
            dirs += r == IZ_DIR;
            zip = JOIN(&u->perm, last, S(".ZIP"));
            r = do_archive(u, zip, 1, 0, scratch);
        }
        switch (r) {
        case PK_WARN:
            warn++;
            break;
        case IZ_DIR:
            dirs++;
            r = PK_NOZIP;
            break;
        case PK_NOZIP:
            break;
        default:
            if (r) {
                lose++;
            } else {
                win++;
            }
        }
        status = MAX(status, r);
    }

    if (wild && u->qflag<3) {
        arena tmp = scratch;
        i32   e   = MSG_STDERR;
        if ((miss+lose+warn>0 || win!=1) && !(u->tflag && u->qflag>1)) {
            info(u, e, S("\n"));
        }
        if (win>1 || (win==1 && dirs+miss+lose+warn>0)) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)win, 0, 0), S(" archive"),
                            win==1 ? S(" was") : S("s were"),
                            S(" successfully processed.\n")));
        }
        if (warn) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)warn, 0, 0), S(" archive"),
                            plural((u64)warn),
                            S(" had warnings but no fatal errors.\n")));
        }
        if (lose) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)lose, 0, 0), S(" archive"),
                            plural((u64)lose), S(" had fatal errors.\n")));
        }
        if (miss) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)miss, 0, 0), S(" file"),
                            plural((u64)miss),
                            S(" had no zipfile directory.\n")));
        }
        if (dirs == 1) {
            info(u, e, S("1 \"zipfile\" was a directory.\n"));
        } else if (dirs) {
            info(u, e, JOIN(&tmp, unum(&tmp, (u64)dirs, 0, 0),
                            S(" \"zipfiles\" were directories.\n")));
        }
        if (!win && !lose && !warn) {
            info(u, e, S("No zipfiles found.\n"));
        }
    }
    return status;
}

static i32 unzip_main(unzipconfig *conf)
{
    unzip *u = new(&conf->perm, 1, unzip);
    u->ctx     = conf->perm.ctx;
    u->perm    = conf->perm;
    u->windows = conf->windows;
    u->sol     = 1;
    u->out     = newwriter(&u->perm, 1, 1<<16);
    arena scratch = conf->scratch;

    i32 r = unzip_args(u, conf, scratch);
    if (r < 0) {
        u->dup = u->tflag && !os_isatty(u->ctx, 1) && os_isatty(u->ctx, 2);
        if (u->tflag || u->cflag || u->extract) {
            u->inf    = inflate_new(&u->perm);
            u->window = newbytes(&u->perm, UZ_WSIZE);
        }
        if (u->extract) {
            // The prompts' answers, unless standard input is the archive;
            // the -d directory (as UnZip's checkdir ROOT, dropping a '/')
            u->overwrite = u->ovnone ? OVERWRT_NEVER :
                           u->ovall  ? OVERWRT_ALWAYS : OVERWRT_QUERY;
            if (!zequals(u->zipspec, S("-"))) {
                u->answers = newreader(&u->perm, 0, 1<<12);
            }
            u->umask = os_umask(u->ctx);
            s8 dir = u->exdir;
            dir.len -= dir.len && dir.s[dir.len-1]=='/';
            u->root = u->hasexdir && u->exdir.len ? JOIN(&u->perm, dir, S("/"))
                                                  : S("");
        }
        r = process_zipfiles(u, scratch);
    }
    writer_flush(u->out);
    return r;
}
