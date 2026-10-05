// tugz zip program: Info-ZIP compatible command line and archive driver
//
// Builds the archive in a temporary file beside the target, then renames
// it into place. Entries kept from an existing archive are copied without
// recompressing. Exit statuses follow Info-ZIP.

enum {
    ZE_OK    = 0,
    ZE_FORM  = 3,   // zip file structure invalid
    ZE_MEM   = 4,
    ZE_READ  = 11,  // could not read the existing archive
    ZE_NONE  = 12,  // nothing to do
    ZE_WRITE = 14,
    ZE_CREAT = 15,
    ZE_PARMS = 16,
    ZE_OPEN  = 18,  // some input files could not be read
};

// Platform interface for the zip program, beyond src/io.c

enum { FT_NONE, FT_FILE, FT_DIR, FT_LINK, FT_OTHER };

typedef struct {
    i32 type;
    i64 size;
    i64 mtime;  // Unix seconds
    i64 atime;
    u32 mode;   // POSIX st_mode (zero on Windows)
    u32 attr;   // DOS attributes (Windows)
    u32 uid;
    u32 gid;
    u64 dev;     // device and file ID identify a file, unless the ID
    u64 ino[2];  // is zero (unknown); 128 bits on Windows
} os_info;

// A directory entry, with what the listing itself tells of the file:
// info as os_stat would report it following links, but with an unknown
// identity, or type FT_NONE where the listing does not tell (always for
// links), in which case os_stat must examine it.
typedef struct {
    s8      name;
    os_info info;
} os_dirent;

// Returns false if the path does not exist or cannot be examined. With
// follow, symbolic links are followed; otherwise a link is FT_LINK.
static b32  os_stat(os *, s8 path, b32 follow, os_info *, arena scratch);
// Entries within a directory, excluding . and .., in any order. Unless
// all, hidden and system entries (Windows) are left out, judged by the
// entry itself rather than a link's target. Returns null on error.
static os_dirent *os_listdir(os *, s8 path, b32 all, iz *count,
                             arena *perm, arena scratch);
// Target of a symbolic link, or a null string on error.
static s8   os_readlink(os *, s8 path, arena *perm, arena scratch);
// Positioned reads and writes of exactly len bytes, which may not be
// mixed with os_read and os_write on the same descriptor.
static b32  os_readat(os *, i32 fd, u8 *buf, iz len, i64 off);
static b32  os_writeat(os *, i32 fd, u8 *buf, iz len, i64 off);
static b32  os_truncate(os *, i32 fd, i64 len);
// Close a created file and move it over path, keeping it. It takes the
// permissions of a file it replaces. The descriptor is closed even on
// failure, which discards the file.
static b32  os_commit(os *, i32 fd, s8 path, arena scratch);
// Broken-down local time {year, month, day, hour, minute, second}.
static void os_localtime(os *, i64 t, i32 tm[6]);
// Whether a standard descriptor is a terminal (console).
static b32  os_isatty(os *, i32 fd);

static void os_oom(os *ctx)
{
    s8 msg = S("\nzip error: Out of memory\n");
    os_write(ctx, 2, msg.s, msg.len);
    os_exit(ctx, ZE_MEM);
}

typedef struct {
    arena perm;
    s8   *args;     // excluding the program name
    i32   nargs;
    s8    epoch;    // SOURCE_DATE_EPOCH, if set
    s8    zipopt;   // ZIPOPT, if set: options before the arguments
    s8    zipenv;   // ZIP, if set: options when ZIPOPT has none
    b32   windows;  // Windows conventions: names, attributes, wildcards
} zipconfig;

static s8 const zip_usage = S8(
    "tugz zip 1.0, a subset of Info-ZIP Zip 3.0\n"
    "usage: zip [-options] archive[.zip] [path ...] [-x pattern ...]\n"
    "  -0..-9  store only, compress faster..better (default 6)\n"
    "  -r      recurse into directories\n"
    "  -q      quiet: no messages or warnings\n"
    "  -X      no extra attributes (times, uid/gid): deterministic output\n"
    "  -@      read paths from standard input, one per line\n"
    "  -j      junk directory names    -D  no directory entries\n"
);
static s8 const zip_usage_posix = S8(
    "  -y      store symbolic links as links\n"
);
static s8 const zip_usage_windows = S8(
    "  -S      include hidden and system files\n"
);
static s8 const zip_usage_tail = S8(
    "  -u      update if newer         -f  freshen: update existing only\n"
    "  -FS     filesync: update changed and delete missing entries\n"
    "  -d      delete entries matching patterns from the archive\n"
    "  -x pattern ...  exclude         -i pattern ...  include only\n"
    "  -nw     no wildcards            --  end of options\n"
    "  -v      version (alone)         -L  license\n"
    "ZIPOPT, or else ZIP, may hold options to apply first.\n"
    "SOURCE_DATE_EPOCH, if set, clamps times and makes them UTC.\n"
);

static s8 const zip_license = S8(
    "This is free and unencumbered software released into the public domain.\n"
    "It is provided \"as is\", without warranty of any kind. For more\n"
    "information, see the UNLICENSE file or <http://unlicense.org/>.\n"
);

enum { MODE_ADD, MODE_UPDATE, MODE_FRESHEN, MODE_SYNC, MODE_DELETE };

typedef struct {
    s8 *data;
    iz  len;
    iz  cap;
} s8s;

typedef struct {
    os_dirent *data;
    iz         len;
    iz         cap;
} os_dirents;

typedef struct {
    s8      path;  // on the file system
    s8      name;  // in the archive
    os_info info;
    u32     dostime;
} zfile;

typedef struct {
    zfile *data;
    iz     len;
    iz     cap;
} zfiles;

typedef struct zmap zmap;
struct zmap {
    zmap *child[4];
    s8    key;
    iz    value;
};

typedef struct dirid dirid;
struct dirid {
    dirid   *up;
    os_info *info;
};

typedef struct {
    os     *ctx;
    arena   perm;
    i32     level;
    i32     mode;
    b32     quiet;
    b32     recurse;
    b32     noextra;
    b32     junk;
    b32     nodirs;
    b32     symlinks;
    b32     hidden;
    b32     nowild;
    b32     names_stdin;
    b32     windows;
    b32     haveepoch;
    i64     epoch;
    s8      archive;
    s8s     paths;
    s8s     include;
    s8s     exclude;
    zfiles  files;
    zmap   *names;
    b32     duplicate;
    os_info arcinfo;
    b32     arcexists;
    i32     status;
    iz      nskipped;
    i64     bskipped;
} zip;

#define push(a, s) \
    ((s)->len==(s)->cap ? grow(a, (void **)&(s)->data, &(s)->cap, \
                               sizeof(*(s)->data)) : (void)0, \
     (s)->data + (s)->len++)

static void grow(arena *a, void **data, iz *cap, iz size)
{
    iz   n = *cap ? 2 * *cap : 16;
    void *r = alloc(a, n, size, 16, 0);
    bytecopy(r, *data, *cap*size);
    *data = r;
    *cap  = n;
}

static s8 zjoin(arena *a, s8 const *parts, iz n)
{
    iz len = 0;
    for (iz i = 0; i < n; i++) {
        len += parts[i].len;
    }
    s8 r = {newbytes(a, len), 0};
    for (iz i = 0; i < n; i++) {
        bytecopy(r.s+r.len, parts[i].s, parts[i].len);
        r.len += parts[i].len;
    }
    return r;
}
#define JOIN(a, ...) \
    zjoin(a, (s8[]){__VA_ARGS__}, countof(((s8[]){__VA_ARGS__})))

static s8 znum(arena *a, i64 v)
{
    u8  tmp[24];
    u8 *e = tmp + countof(tmp);
    u8 *p = e;
    u64 u = v<0 ? -(u64)v : (u64)v;
    do {
        *--p = (u8)('0' + u%10);
    } while (u /= 10);
    if (v < 0) {
        *--p = '-';
    }
    s8 r = {newbytes(a, e-p), e-p};
    bytecopy(r.s, p, r.len);
    return r;
}

static b32 zequals(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !__builtin_memcmp(a.s, b.s, (uz)a.len));
}

static b32 zisspace(u8 c)
{
    return c==' ' || (c>='\t' && c<='\r');
}

static i32 zcompare(s8 a, s8 b)
{
    iz n = MIN(a.len, b.len);
    i32 r = n ? __builtin_memcmp(a.s, b.s, (uz)n) : 0;
    return r ? r : a.len<b.len ? -1 : a.len>b.len;
}

// Stable bottom-up merge sort of directory entries by name bytes.
static void zsort(os_dirent *v, iz n, arena scratch)
{
    os_dirent *tmp = new(&scratch, n, os_dirent);
    for (iz w = 1; w < n; w *= 2) {
        for (iz lo = 0; lo < n; lo += 2*w) {
            iz mid = MIN(lo+w, n);
            iz hi  = MIN(lo+2*w, n);
            iz i = lo, j = mid, k = lo;
            while (i<mid && j<hi) {
                b32 lt = zcompare(v[j].name, v[i].name) < 0;
                tmp[k++] = lt ? v[j++] : v[i++];
            }
            while (i < mid) {
                tmp[k++] = v[i++];
            }
            while (j < hi) {
                tmp[k++] = v[j++];
            }
        }
        bytecopy(v, tmp, n*(iz)sizeof(*v));
    }
}

static u64 zhash(s8 s)
{
    u64 h = 0x100;
    for (iz i = 0; i < s.len; i++) {
        h ^= s.s[i];
        h *= 1111111111111111111u;
    }
    return h;
}

// Find a key's value, inserting it with value -1 if perm is not null.
static iz *zmap_upsert(zmap **m, s8 key, arena *perm)
{
    for (u64 h = zhash(key); *m; h <<= 2) {
        if (zequals(key, (*m)->key)) {
            return &(*m)->value;
        }
        m = &(*m)->child[h>>62];
    }
    if (!perm) {
        return 0;
    }
    *m = new(perm, 1, zmap);
    (*m)->key   = key;
    (*m)->value = -1;
    return &(*m)->value;
}

static void say(zip *z, i32 fd, s8 msg)
{
    os_write(z->ctx, fd, msg.s, msg.len);
}

static void warn(zip *z, s8 msg, s8 arg, arena scratch)
{
    if (!z->quiet) {
        say(z, 2, JOIN(&scratch, S("zip warning: "), msg, arg, S("\n")));
    }
}

// Report a fatal error and return its exit status.
static i32 fail(zip *z, i32 status, s8 msg, s8 arg, arena scratch)
{
    s8 tail = arg.s ? JOIN(&scratch, S(" ("), arg, S(")")) : S("");
    say(z, 2, JOIN(&scratch, S("\nzip error: "), msg, tail, S("\n")));
    return status;
}

static i32 badopt(zip *z, s8 kind, s8 opt, arena scratch)
{
    s8 msg = JOIN(&scratch, kind, S(" option '"), opt,
                  S("' not supported"));
    return fail(z, ZE_PARMS, S("Invalid command arguments"), msg, scratch);
}

// Read an entire descriptor into memory, or return a null string.
static s8 read_all(zip *z, i32 fd, arena *perm)
{
    s8 r  = {0};
    iz cap = 0;
    for (;;) {
        if (r.len == cap) {
            iz ncap = cap ? 2*cap : 1<<12;
            u8 *buf = newbytes(perm, ncap);
            bytecopy(buf, r.s, r.len);
            r.s = buf;
            cap = ncap;
        }
        iz n = os_read(z->ctx, fd, r.s+r.len, cap-r.len);
        if (n < 0) {
            return (s8){0};
        } else if (!n) {
            r.s = r.s ? r.s : (u8 *)"";
            return r;
        }
        r.len += n;
    }
}

// Append each name in a list (-@, @file) as Info-ZIP's getnam reads it:
// a line ends at any CR or LF, empty lines are skipped, and a name ends
// at a NUL. On Windows, trailing spaces and periods are dropped first.
static void push_lines(zip *z, s8s *list, s8 text)
{
    for (iz i = 0; i < text.len;) {
        iz j = i;
        for (; j<text.len && text.s[j]!='\n' && text.s[j]!='\r'; j++) {}
        s8 line = {text.s+i, j-i};
        i = j + 1;
        if (!line.len) {
            continue;
        }
        while (z->windows && line.len && (line.s[line.len-1]==' ' ||
                                         line.s[line.len-1]=='.')) {
            line.len--;
        }
        iz len = 0;
        for (; len<line.len && line.s[len]; len++) {}
        line.len = len;
        *push(&z->perm, list) = line;
    }
}

static b32 is_list_end(s8 arg)
{
    return (arg.len>1 && arg.s[0]=='-') || zequals(arg, S("@"));
}

// Add an element of a pattern list: a pattern, or @file, a file of them,
// one per line. Like Info-ZIP, patterns are normalized as names are, so
// that ./ and / prefixes, and Windows backslashes, match.
static i32 add_patterns(zip *z, s8s *list, s8 arg, arena scratch)
{
    iz first = list->len;
    if (!arg.len || arg.s[0]!='@') {
        *push(&z->perm, list) = arg;
    } else if (arg.len == 1) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"),
                    S("missing file after @"), scratch);
    } else {
        s8  path = {arg.s+1, arg.len-1};
        i32 fd   = os_open(z->ctx, path, OS_READ, scratch);
        s8  text = fd<0 ? (s8){0} : read_all(z, fd, &z->perm);
        if (fd >= 0) {
            os_close(z->ctx, fd);
        }
        if (!text.s) {
            s8 opt = list==&z->exclude ? S("x") : S("i");
            s8 msg = JOIN(&scratch, opt, S(" pattern file '"), arg, S("'"));
            return fail(z, ZE_OPEN, S("File not found or no read permission"),
                        msg, scratch);
        }
        push_lines(z, list, text);
    }
    for (iz i = first; i < list->len; i++) {
        list->data[i] = zip_name(&z->perm, list->data[i], z->windows);
    }
    return 0;
}

// Collect a pattern list option's values: an attached value, else the
// following arguments up to the next option or a lone @.
static i32 take_list(zip *z, s8s *list, s8 value, s8 *args, i32 nargs,
                     i32 *i, arena scratch)
{
    if (value.len) {
        return add_patterns(z, list, value, scratch);
    }
    i32 n = 0;
    for (; *i+1<nargs && !is_list_end(args[*i+1]); n++) {
        i32 err = add_patterns(z, list, args[++*i], scratch);
        if (err) {
            return err;
        }
    }
    if (*i+1<nargs && zequals(args[*i+1], S("@"))) {
        ++*i;
    }
    if (!n) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"),
                    S("missing pattern list"), scratch);
    }
    return 0;
}

static i32 set_mode(zip *z, i32 mode, arena scratch)
{
    if (z->mode!=MODE_ADD && z->mode!=mode) {
        return fail(z, ZE_PARMS, S("Invalid command arguments"),
                    S("conflicting modes"), scratch);
    }
    z->mode = mode;
    return 0;
}

static i32 usage(zip *z, i32 status)
{
    i32 fd = status ? 2 : 1;
    say(z, fd, zip_usage);
    say(z, fd, z->windows ? zip_usage_windows : zip_usage_posix);
    say(z, fd, zip_usage_tail);
    return status;
}

static void version(zip *z)
{
    s8 line = zip_usage;
    for (line.len = 0; line.s[line.len++] != '\n';) {}
    say(z, 1, line);
}

enum {
    OPT_NEGATE = 1 << 0,  // negatable: only -X, as in Info-ZIP
    OPT_LIST   = 1 << 1,  // takes a pattern list
    OPT_POSIX  = 1 << 2,  // not in Info-ZIP's Windows port
    OPT_WIN    = 1 << 3,  // only in Info-ZIP's Windows port
};

typedef struct {
    s8  name;   // short name, if any
    s8  lname;  // long name, if any
    s8  help;   // Info-ZIP's description, which its errors quote
    i32 flags;
} zoption;

// The supported subset of Info-ZIP's options, under its names
static zoption const zip_options[] = {
    {S8("0"),  S8("store"),          S8("store"),                       0},
    {S8("1"),  S8("compress-1"),     S8("compress 1"),                  0},
    {S8("2"),  S8("compress-2"),     S8("compress 2"),                  0},
    {S8("3"),  S8("compress-3"),     S8("compress 3"),                  0},
    {S8("4"),  S8("compress-4"),     S8("compress 4"),                  0},
    {S8("5"),  S8("compress-5"),     S8("compress 5"),                  0},
    {S8("6"),  S8("compress-6"),     S8("compress 6"),                  0},
    {S8("7"),  S8("compress-7"),     S8("compress 7"),                  0},
    {S8("8"),  S8("compress-8"),     S8("compress 8"),                  0},
    {S8("9"),  S8("compress-9"),     S8("compress 9"),                  0},
    {S8("d"),  S8("delete"),         S8("delete entries from archive"), 0},
    {S8("D"),  S8("no-dir-entries"),
               S8("no entries for dirs themselves (-x */)"),            0},
    {S8("FS"), S8("filesync"),
               S8("add/delete entries to make archive match OS"),       0},
    {S8("f"),  S8("freshen"),    S8("freshen existing archive entries"), 0},
    {S8("h"),  S8("help"),           S8("help"),                        0},
    {S8(""),   S8("more-help"),      S8("extended help"),               0},
    {S8("i"),  S8("include"),
               S8("include only files matching patterns"),       OPT_LIST},
    {S8("j"),  S8("junk-paths"),
               S8("strip paths and just store file names"),             0},
    {S8("L"),  S8("license"),        S8("display license"),             0},
    {S8("nw"), S8("no-wild"),  S8("no wildcards during add or update"), 0},
    {S8("p"),  S8("paths"),          S8("store paths"),                 0},
    {S8("q"),  S8("quiet"),          S8("quiet"),                       0},
    {S8("r"),  S8("recurse-paths"),  S8("recurse down listed paths"),   0},
    {S8("S"),  S8(""),         S8("include system and hidden"),   OPT_WIN},
    {S8("u"),  S8("update"),
               S8("update existing entries and add new"),               0},
    {S8("v"),  S8("verbose"),  S8("display additional information"),    0},
    {S8(""),   S8("version"),
               S8("(if no other args) show version information"),       0},
    {S8("x"),  S8("exclude"),
               S8("exclude files matching patterns"),            OPT_LIST},
    {S8("X"),  S8("strip-extra"),
               S8("-X- keep all ef, -X strip but critical ef"), OPT_NEGATE},
    {S8("y"),  S8("symlinks"),       S8("store symbolic links"), OPT_POSIX},
    {S8("@"),  S8("names-stdin"),
               S8("get file names from stdin, one per line"),           0},
};

// All of Info-ZIP's long option names, supported or not, for its
// abbreviations: a prefix of exactly one name stands for that name.
static char const zip_longnames[] =
    " store compress-1 compress-2 compress-3 compress-4 compress-5"
    " compress-6 compress-7 compress-8 compress-9 adjust-sfx temp-path"
    " entry-comments delete display-bytes display-counts display-dots"
    " display-globaldots dot-size display-usize display-volume"
    " no-dir-entries difference-archive encrypt fix fixfix fifo filesync"
    " freshen force-descriptors force-zip64 grow help more-help include"
    " junk-paths junk-sfx DOS-names to-crlf from-crlf logfile-path"
    " log-append log-info license move must-match suffixes no-wild"
    " latest-time output-file paths password quiet recurse-paths"
    " recurse-patterns regex split-size split-pause split-verbose"
    " split-bell show-command show-debug show-files show-options"
    " show-unicode show-just-unicode unicode from-date before-date test"
    " unzip-command update copy-entries verbose version wild-stop-dirs"
    " exclude strip-extra symlinks archive-comment compression-method"
    " names-stdin ";

// Two-letter short options, matched before single letters as Info-ZIP
// does. Only FS and nw are supported.
static char const two_letter[] =
    "FSnwFFFIDFACASMMRETTUNdbdcdddfdgdsdudvicjjlalflillsbscsdsfsosp"
    "susUsvttws";

// A supported option by its short or long name, or null.
static zoption const *find_option(zip *z, s8 name, b32 islong)
{
    for (iz i = 0; i < countof(zip_options); i++) {
        zoption const *o = zip_options + i;
        if (name.len && zequals(name, islong ? o->lname : o->name)) {
            b32 absent = o->flags & (z->windows ? OPT_POSIX : OPT_WIN);
            return absent ? 0 : o;
        }
    }
    return 0;
}

// Expand an abbreviated long option name. Returns false, having
// reported it, if it is ambiguous.
static b32 expand_long(zip *z, s8 *name, arena scratch)
{
    s8  found = {0};
    i32 count = 0;
    for (char const *p = zip_longnames+1; *p && name->len; p++) {
        s8 full = {(u8 *)p, 0};
        for (; p[full.len] != ' '; full.len++) {}
        p += full.len;
        if (zequals(full, *name)) {
            return 1;
        } else if (full.len>name->len &&
                   zequals((s8){full.s, name->len}, *name)) {
            found = full;
            count++;
        }
    }
    if (count > 1) {
        s8 msg = JOIN(&scratch, S("long option '"), *name, S("' ambiguous"));
        fail(z, ZE_PARMS, S("Invalid command arguments"), msg, scratch);
        return 0;
    }
    *name = count ? found : *name;
    return 1;
}

static i32 misused(zip *z, s8 opt, zoption const *o, s8 how, arena scratch)
{
    s8 msg = JOIN(&scratch, S("option '"), opt, S("' ("), o->help, S(") "),
                  how);
    return fail(z, ZE_PARMS, S("Invalid command arguments"), msg, scratch);
}

// Apply an option, as given by opt, taking a pattern list's values from
// value or the following arguments. Returns an exit status to stop with:
// an error, or -1 for success after an informational option.
static i32 apply_option(zip *z, zoption const *o, s8 opt, b32 negate,
                        s8 value, s8 *args, i32 nargs, i32 *i,
                        arena scratch)
{
    if (negate && !(o->flags & OPT_NEGATE)) {
        return misused(z, opt, o, S("not negatable"), scratch);
    }

    s8 key = o->name.len ? o->name : o->lname;
    if (key.len==1 && key.s[0]>='0' && key.s[0]<='9') {
        z->level = key.s[0] - '0';
        return 0;
    } else if (zequals(key, S("h")) || zequals(key, S("more-help"))) {
        return usage(z, 0) - 1;
    } else if (zequals(key, S("version"))) {
        version(z);
        return -1;
    } else if (zequals(key, S("L"))) {
        say(z, 1, zip_license);
        return -1;
    } else if (o->flags & OPT_LIST) {
        s8s *list = key.s[0]=='x' ? &z->exclude : &z->include;
        return take_list(z, list, value, args, nargs, i, scratch);
    }

    b32 on = !negate;
    if (zequals(key, S("q")))  { z->quiet       = on; return 0; }
    if (zequals(key, S("r")))  { z->recurse     = on; return 0; }
    if (zequals(key, S("X")))  { z->noextra     = on; return 0; }
    if (zequals(key, S("j")))  { z->junk        = on; return 0; }
    if (zequals(key, S("D")))  { z->nodirs      = on; return 0; }
    if (zequals(key, S("y")))  { z->symlinks    = on; return 0; }
    if (zequals(key, S("S")))  { z->hidden      = on; return 0; }
    if (zequals(key, S("nw"))) { z->nowild      = on; return 0; }
    if (zequals(key, S("@")))  { z->names_stdin = on; return 0; }
    if (zequals(key, S("u")))  { return set_mode(z, MODE_UPDATE,  scratch); }
    if (zequals(key, S("f")))  { return set_mode(z, MODE_FRESHEN, scratch); }
    if (zequals(key, S("FS"))) { return set_mode(z, MODE_SYNC,    scratch); }
    if (zequals(key, S("d")))  { return set_mode(z, MODE_DELETE,  scratch); }
    return 0;  // -p (store paths, the default) and -v (verbose): no effect
}

static i32 parse_args(zip *z, s8 *args, i32 nargs, arena scratch)
{
    b32 options = 1;
    for (i32 i = 0; i < nargs; i++) {
        s8 arg = args[i];
        if (!options || arg.len<2 || arg.s[0]!='-') {
            if (zequals(arg, S("-"))) {
                return fail(z, ZE_PARMS, S("Invalid command arguments"),
                            S("streaming with - not supported"), scratch);
            } else if (!z->archive.s) {
                z->archive = arg;
            } else {
                *push(&z->perm, &z->paths) = arg;
            }
            continue;
        }

        if (zequals(arg, S("--"))) {
            if (!z->archive.s) {
                return fail(z, ZE_PARMS, S("Invalid command arguments"),
                            S("can't use -- before archive name"), scratch);
            }
            options = 0;
        } else if (arg.s[1] == '-') {
            s8 name  = {arg.s+2, arg.len-2};
            s8 value = {0};
            for (iz k = 0; k < name.len; k++) {
                if (name.s[k] == '=') {
                    value = (s8){name.s+k+1, name.len-k-1};
                    name.len = k;
                    break;
                }
            }
            b32 negate = name.len>1 && name.s[name.len-1]=='-';
            name.len -= negate;
            if (!expand_long(z, &name, scratch)) {
                return ZE_PARMS;
            }
            zoption const *o = find_option(z, name, 1);
            if (!o) {
                return badopt(z, S("long"), name, scratch);
            } else if (value.s && !(o->flags & OPT_LIST)) {
                return misused(z, name, o, S("does not allow a value"),
                               scratch);
            }
            i32 err = apply_option(z, o, name, negate, value, args, nargs,
                                   &i, scratch);
            if (err) {
                return err;
            }
        } else {
            for (iz k = 1; k < arg.len;) {
                s8 opt = {arg.s+k, 1};
                for (iz t = 0; t < countof(two_letter)-1; t += 2) {
                    if (k+1<arg.len && arg.s[k]==two_letter[t] &&
                        arg.s[k+1]==two_letter[t+1]) {
                        opt.len = 2;
                        break;
                    }
                }
                k += opt.len;
                zoption const *o = find_option(z, opt, 0);
                if (!o) {
                    return badopt(z, S("short"), opt, scratch);
                }

                // A list's value is the rest of the argument, even "-"
                s8  value  = {0};
                b32 negate = 0;
                if (o->flags & OPT_LIST) {
                    value = (s8){arg.s+k, arg.len-k};
                    if (value.len && value.s[0]=='=') {
                        value.s++;
                        value.len--;
                    }
                    k = arg.len;
                } else {
                    negate = k<arg.len && arg.s[k]=='-';
                    k += negate;
                }
                i32 err = apply_option(z, o, opt, negate, value, args, nargs,
                                       &i, scratch);
                if (err) {
                    return err;
                }
            }
        }
    }
    return 0;
}

// Split a ZIPOPT or ZIP value into arguments as Info-ZIP's envargs does:
// at whitespace, and on POSIX a word that starts with a double quote
// runs to the next one, which is dropped, though one after a backslash
// is not (the backslash stays).
static s8s env_args(zip *z, s8 env)
{
    s8s r = {0};
    for (iz i = 0;;) {
        for (; i<env.len && zisspace(env.s[i]); i++) {}
        if (i == env.len) {
            return r;
        }
        s8 *arg = push(&z->perm, &r);
        if (!z->windows && env.s[i]=='"') {
            iz beg = ++i;
            for (; i<env.len && env.s[i]!='"'; i++) {
                i += env.s[i]=='\\' && i+1<env.len && env.s[i+1]=='"';
            }
            *arg = (s8){env.s+beg, i-beg};
            i += i < env.len;
        } else {
            iz beg = i;
            for (; i<env.len && !zisspace(env.s[i]); i++) {}
            *arg = (s8){env.s+beg, i-beg};
        }
    }
}

static u32 file_dostime(zip *z, i64 t)
{
    if (z->haveepoch) {
        t = MIN(t, z->epoch);
    }
    t = (t + 1) & ~(i64)1;  // round odd seconds up, as Info-ZIP does
    i32 tm[6];
    if (z->haveepoch) {
        zip_gmtime(t, tm);
    } else {
        os_localtime(z->ctx, t, tm);
    }
    return zip_dostime(tm);
}

// Pattern matching as Info-ZIP's: [sets] on Unix; on Windows, its DOS
// rules, ignoring case except against archive entries (-d) and when
// freshening.
static i32 match_flags(zip *z)
{
    i32 flags = z->nowild ? ZIP_NOWILD : 0;
    if (!z->windows) {
        return flags | ZIP_SETS;
    }
    b32 exact = z->mode==MODE_DELETE || z->mode==MODE_FRESHEN;
    return flags | ZIP_DOS | (exact ? 0 : ZIP_FOLD);
}

static b32 any_match(zip *z, s8s *patterns, s8 name)
{
    for (iz i = 0; i < patterns->len; i++) {
        if (zip_match(patterns->data[i], name, match_flags(z))) {
            return 1;
        }
    }
    return 0;
}

// Whether a name passes the -i and -x patterns.
static b32 included(zip *z, s8 name)
{
    if (z->include.len && !any_match(z, &z->include, name)) {
        return 0;
    }
    return !any_match(z, &z->exclude, name);
}

static b32 selected(zip *z, s8 name)
{
    b32 dir = name.len && name.s[name.len-1]=='/';
    if (dir && (z->nodirs || z->junk)) {
        return 0;
    }
    return included(z, name);
}

// Whether two files are known to be one: an unknown ID matches nothing.
static b32 same_file(os_info *a, os_info *b)
{
    return (a->ino[0] || a->ino[1]) && a->dev==b->dev &&
           a->ino[0]==b->ino[0] && a->ino[1]==b->ino[1];
}

// Whether a listing tells enough to skip os_stat, saving a handle per
// file on Windows. It lacks identity, which a directory needs to detect
// loops, and a file only if, being the archive's size, it may be that.
static b32 listed(zip *z, os_info *info)
{
    return info->type==FT_FILE &&
           (!z->arcexists || info->size!=z->arcinfo.size);
}

static b32 is_sep(zip *z, u8 c)
{
    return c=='/' || (z->windows && c=='\\');
}

static s8 basename(zip *z, s8 path)
{
    iz i = path.len;
    for (; i>0 && is_sep(z, path.s[i-1]); i--) {}
    path.len = i;
    for (; i>0 && !is_sep(z, path.s[i-1]); i--) {}
    return (s8){path.s+i, path.len-i};
}

// Add a file under its archive name, which -i and -x see whole, before
// -j junks its directories, as in Info-ZIP.
static void add_file(zip *z, s8 path, s8 name, os_info *info, arena scratch)
{
    if (z->arcexists && info->type==FT_FILE && same_file(info, &z->arcinfo)) {
        return;  // the archive itself
    } else if (!name.len || !selected(z, name)) {
        return;
    }
    name = z->junk ? basename(z, name) : name;

    iz *seen = zmap_upsert(&z->names, name, &z->perm);
    if (*seen >= 0) {
        s8 first = z->files.data[*seen].path;
        warn(z, S("  first full name: "), first, scratch);
        warn(z, S(" second full name: "), path, scratch);
        warn(z, S("name in zip file repeated: "), name, scratch);
        z->duplicate = 1;
        return;
    }
    *seen = z->files.len;

    zfile *f = push(&z->perm, &z->files);
    f->path    = path;
    f->name    = name;
    f->info    = *info;
    f->dostime = file_dostime(z, info->mtime);
}

static void scan(zip *z, s8 path, s8 name, os_info *info, dirid *up,
                 arena scratch)
{
    switch (info->type) {
    case FT_FILE:
    case FT_LINK:
        add_file(z, path, name, info, scratch);
        return;
    case FT_DIR:
        break;
    default:
        warn(z, S("skipping special file: "), path, scratch);
        return;
    }

    s8 dname = name;
    if (name.len && name.s[name.len-1]!='/') {
        dname = JOIN(&z->perm, name, S("/"));
    }
    add_file(z, path, dname, info, scratch);
    if (!z->recurse) {
        return;
    }

    for (dirid *d = up; d; d = d->up) {
        if (same_file(d->info, info)) {
            warn(z, S("skipping directory loop: "), path, scratch);
            return;
        }
    }
    dirid self = {up, info};

    iz         n    = 0;
    os_dirent *kids = os_listdir(z->ctx, path, z->hidden, &n, &scratch,
                                 scratch);
    if (!kids) {
        warn(z, S("could not read directory: "), path, scratch);
        z->status = ZE_OPEN;
        return;
    }
    zsort(kids, n, scratch);

    s8 sep = path.len && is_sep(z, path.s[path.len-1]) ? S("") : S("/");
    for (iz i = 0; i < n; i++) {
        s8 kpath = JOIN(&z->perm, path, sep, kids[i].name);
        s8 kname = JOIN(&z->perm, dname, kids[i].name);
        os_info *k = &kids[i].info;
        if (!listed(z, k) && !os_stat(z->ctx, kpath, !z->symlinks, k,
                                      scratch)) {
            warn(z, S("could not open for reading: "), kpath, scratch);
            z->status = ZE_OPEN;
            z->nskipped++;
            continue;
        }
        scan(z, kpath, kname, k, &self, scratch);
    }
}

static s8 arg_name(zip *z, s8 path)
{
    return zip_name(&z->perm, path, z->windows);
}

// Whether a file is hidden or system (Windows), which Info-ZIP leaves
// out unless -S, even when named. A directory is still scanned.
static b32 hidden_file(zip *z, os_info *info)
{
    return z->windows && !z->hidden && info->type!=FT_DIR &&
           (info->attr & 0x06);
}

// Expand wildcards in a path's components against the file system, as
// Windows shells do not, matching as Info-ZIP does there: ignoring case,
// with DOS rules, and with -nw only ?. Returns the number of matches.
static iz expand(zip *z, s8 path, arena scratch)
{
    // Find the first component with a wildcard. A drive ends a component:
    // "C:*.c" lists the drive's current directory, "C:".
    u8 drive = path.len>=2 && path.s[1]==':' ? (u8)(path.s[0] | 0x20) : 0;
    iz beg = 0;
    iz end = 0;
    for (iz i = drive>='a' && drive<='z' ? 2 : 0;; i = end + 1) {
        beg = i;
        for (end = i; end<path.len && !is_sep(z, path.s[end]); end++) {}
        s8 comp = {path.s+beg, end-beg};
        if (zip_haswild(comp, 0)) {
            break;
        } else if (end == path.len) {
            return 0;
        }
    }

    s8 dir  = beg ? (s8){path.s, beg} : S(".");
    s8 pat  = {path.s+beg, end-beg};
    s8 rest = {path.s+end, path.len-end};
    iz         n    = 0;
    os_dirent *kids = os_listdir(z->ctx, dir, z->hidden, &n, &scratch,
                                 scratch);
    if (!kids) {
        return 0;
    }
    zsort(kids, n, scratch);

    i32 flags = ZIP_FOLD | ZIP_DOS | (z->nowild ? ZIP_NOWILD : 0);
    iz  count = 0;
    for (iz i = 0; i < n; i++) {
        if (!zip_match(pat, kids[i].name, flags)) {
            continue;
        }
        s8 cand = JOIN(&z->perm, (s8){path.s, beg}, kids[i].name, rest);
        if (zip_haswild(rest, 0)) {
            count += expand(z, cand, scratch);
            continue;
        }
        // The listing describes the match only if nothing follows it, and
        // a bare name, like an argument, may instead name a device (NUL)
        os_info *info  = &kids[i].info;
        b32      known = beg && !rest.len && listed(z, info);
        if (known || os_stat(z->ctx, cand, !z->symlinks, info, scratch)) {
            scan(z, cand, arg_name(z, cand), info, 0, scratch);
            count++;
        }
    }
    return count;
}

static void scan_arg(zip *z, s8 arg, arena scratch)
{
    os_info info = {0};
    if (os_stat(z->ctx, arg, !z->symlinks, &info, scratch)) {
        if (!hidden_file(z, &info)) {
            scan(z, arg, arg_name(z, arg), &info, 0, scratch);
        }
    } else if (!z->windows || !zip_haswild(arg, 0) ||
               !expand(z, arg, scratch)) {
        warn(z, S("name not matched: "), arg, scratch);
    }
}

// The existing archive

typedef struct {
    i32     fd;
    i64     size;
    zend    end;
    zentry *entries;
} zarchive;

static i32 read_archive(zip *z, zarchive *ar, arena scratch)
{
    ar->fd = os_open(z->ctx, z->archive, OS_READ|OS_REGULAR, scratch);
    if (ar->fd < 0) {
        return fail(z, ZE_READ, S("Could not open archive"), z->archive,
                    scratch);
    }
    ar->size = z->arcinfo.size;

    iz  n    = (iz)MIN(ar->size, ZIP_END_LEN + ZIP_MAX16 + ZIP_LOC64_LEN);
    u8 *tail = newbytes(&z->perm, n);
    if (!os_readat(z->ctx, ar->fd, tail, n, ar->size-n)) {
        return fail(z, ZE_READ, S("Could not read archive"), z->archive,
                    scratch);
    }

    i32 r = zip_find_end(tail, n, ar->size, &ar->end);
    if (r==ZIP_OK && ar->end.end64>=0) {
        u8 rec[ZIP_END64_LEN];
        if (!os_readat(z->ctx, ar->fd, rec, ZIP_END64_LEN, ar->end.end64)) {
            return fail(z, ZE_READ, S("Could not read archive"), z->archive,
                        scratch);
        }
        r = zip_parse_end64(rec, &ar->end);
    }
    switch (r) {
    case ZIP_OK:
        break;
    case ZIP_ENOEND:
        warn(z, S("missing end signature--probably not a zip file"), S(""),
             scratch);
        // fallthrough
    case ZIP_EFORMAT:
        return fail(z, ZE_FORM, S("Zip file structure invalid"), z->archive,
                    scratch);
    case ZIP_EMULTI:
        return fail(z, ZE_FORM, S("Split archives not supported"),
                    z->archive, scratch);
    case ZIP_EPREFIX:
        return fail(z, ZE_FORM, S("Data before the archive (self-extractor)"
                    " not supported"), z->archive, scratch);
    }

    i64 cdsize = ar->end.cdsize;
    if (cdsize > (i64)(z->perm.end - z->perm.beg)) {
        os_oom(z->ctx);
    }
    u8 *cd = newbytes(&z->perm, (iz)cdsize);
    if (!os_readat(z->ctx, ar->fd, cd, (iz)cdsize, ar->end.cdoff)) {
        return fail(z, ZE_READ, S("Could not read archive"), z->archive,
                    scratch);
    }
    ar->entries = zip_parse_central(cd, (iz)cdsize, ar->end.count,
                                    ar->end.cdoff, &z->perm);
    if (!ar->entries) {
        return fail(z, ZE_FORM, S("Zip file structure invalid"), z->archive,
                    scratch);
    }
    return 0;
}

// Output: buffered positioned writes into the temporary file

typedef struct {
    os  *ctx;
    i32  fd;
    u8  *buf;
    iz   len;
    iz   cap;
    i64  pos;  // file offset of buf[0]
    b32  err;
} zout;

static void zout_flush(zout *w)
{
    if (w->len && !w->err) {
        w->err = !os_writeat(w->ctx, w->fd, w->buf, w->len, w->pos);
    }
    w->pos += w->len;
    w->len  = 0;
}

static void zout_write(zout *w, u8 const *p, iz n)
{
    while (n) {
        if (w->len == w->cap) {
            zout_flush(w);
        }
        iz take = MIN(n, w->cap-w->len);
        bytecopy(w->buf+w->len, p, take);
        w->len += take;
        p += take;
        n -= take;
    }
}

static i64 zout_tell(zout *w)
{
    return w->pos + w->len;
}

// Move the write position, such as back to the start of an entry. What
// followed is dropped if still buffered, else overwritten or truncated.
static void zout_seek(zout *w, i64 off)
{
    if (off>=w->pos && off-w->pos<=w->len) {
        w->len = (iz)(off - w->pos);
        return;
    }
    zout_flush(w);
    w->pos = off;
}

// Rewrite bytes already written, such as a local header.
static void zout_patch(zout *w, i64 off, u8 *p, iz n)
{
    if (off>=w->pos && off-w->pos<=w->len-n) {
        bytecopy(w->buf+(off-w->pos), p, n);  // still buffered
        return;
    }
    zout_flush(w);
    if (!w->err) {
        w->err = !os_writeat(w->ctx, w->fd, p, n, off);
    }
}

// Entry data source: a file, or memory (a symbolic link's target)

typedef struct {
    s8  path;
    s8  mem;
    iz  off;
    i32 fd;
    b32 err;
} zsrc;

static b32 src_open(zip *z, zsrc *s, arena scratch)
{
    s->off = 0;
    s->err = 0;
    if (s->mem.s) {
        return 1;
    }
    s->fd = os_open(z->ctx, s->path, OS_READ, scratch);
    return s->fd >= 0;
}

static void src_close(zip *z, zsrc *s)
{
    if (!s->mem.s && s->fd>=0) {
        os_close(z->ctx, s->fd);
        s->fd = -1;
    }
}

static iz src_read(zip *z, zsrc *s, u8 *buf, iz cap)
{
    if (s->mem.s) {
        iz n = MIN(cap, s->mem.len-s->off);
        bytecopy(buf, s->mem.s+s->off, n);
        s->off += n;
        return n;
    }
    iz n = os_read(z->ctx, s->fd, buf, cap);
    s->err |= n < 0;
    return n<0 ? 0 : n;
}

typedef struct {
    zout     *out;
    u8       *buf;  // input buffer
    iz        cap;
    deflator *def;  // reset for each entry
} zwork;

static void store_data(zip *z, zwork *k, zsrc *s, u32 *crc, i64 *usize)
{
    if (s->mem.s) {
        // Written directly, as it may be the input buffer itself
        *crc = crc32_update(*crc, s->mem.s, s->mem.len);
        *usize += s->mem.len;
        zout_write(k->out, s->mem.s, s->mem.len);
        return;
    }
    for (iz n; (n = src_read(z, s, k->buf, k->cap));) {
        *crc = crc32_update(*crc, k->buf, n);
        *usize += n;
        zout_write(k->out, k->buf, n);
    }
}

// Returns whether the first read got all the input, which is therefore
// still in the input buffer.
static b32 deflate_data(zip *z, zwork *k, zsrc *s, u32 *crc, i64 *usize)
{
    deflator *d = k->def;
    deflate_reset(d);
    iz first = -1;
    for (b32 more = 1; more;) {
        iz n = src_read(z, s, k->buf, k->cap);
        more = n > 0;
        first = first<0 ? n : first;
        *crc = crc32_update(*crc, k->buf, n);
        *usize += n;
        zbuf b = {k->buf, n, 0, 0};
        for (;;) {
            i32 r = deflate_stream(d, &b, more ? DEF_NONE : DEF_FINISH);
            s8  p = deflate_pending(d);
            zout_write(k->out, p.s, p.len);
            deflate_consume(d, p.len);
            if (r != GZ_NEEDOUT) {
                break;
            }
        }
    }
    return *usize == first;
}

// Unix time as a 32-bit field, as Info-ZIP stores it.
static u8 *put_time(u8 *p, zip *z, i64 t)
{
    t = z->haveepoch ? MIN(t, z->epoch) : t;
    return put32(p, (u32)t);
}

// Info-ZIP's extended timestamp and Unix ownership extra fields.
static void file_extras(zip *z, zfile *f, zentry *e)
{
    if (z->noextra) {
        return;
    }
    u8 *l = newbytes(&z->perm, 13+15);
    u8 *c = newbytes(&z->perm,  9+15);
    e->lextra.s = l;
    e->cextra.s = c;

    l = put16(l, ZIP_EXTRA_TIME);
    l = put16(l, 9);
    *l++ = 3;  // modification and access times
    l = put_time(l, z, f->info.mtime);
    l = put_time(l, z, f->info.atime);
    c = put16(c, ZIP_EXTRA_TIME);
    c = put16(c, 5);
    *c++ = 3;
    c = put_time(c, z, f->info.mtime);

    if (!z->windows) {
        for (i32 i = 0; i < 2; i++) {
            u8 **p = i ? &c : &l;
            *p = put16(*p, ZIP_EXTRA_UNIX);
            *p = put16(*p, 11);
            *(*p)++ = 1;  // version
            *(*p)++ = 4;
            *p = put32(*p, f->info.uid);
            *(*p)++ = 4;
            *p = put32(*p, f->info.gid);
        }
    }
    e->lextra.len = l - e->lextra.s;
    e->cextra.len = c - e->cextra.s;
}

static u32 file_extattr(zip *z, os_info *info)
{
    b32 dir = info->type == FT_DIR;
    if (z->windows) {
        return (info->attr & 0x37) | (dir ? 0x10 : 0);
    }
    b32 ro = !(info->mode & 0200);
    return info->mode<<16 | (dir ? 0x10 : 0) | (ro ? 0x01 : 0);
}

static u16 level_flags(i32 level)
{
    return level>=8 ? 2 : level<=2 ? 4 : 0;
}

// Compress a new entry. Returns false if its input could not be read,
// leaving the output where it began.
static b32 write_file(zip *z, zwork *k, zfile *f, zentry *e, arena scratch)
{
    zout *w = k->out;
    i64 start = zout_tell(w);

    i32 utf8 = zip_utf8(f->name);
    if (utf8 < 0 && z->windows) {
        warn(z, S("name is not valid UTF-8: "), f->path, scratch);
    }
    *e = (zentry){0};  // the slot may hold a failed attempt's fields
    e->name    = f->name;
    e->made    = z->windows ? 0x001e : 0x031e;  // FAT or Unix, Zip 3.0
    e->flags   = utf8>0 ? ZIP_FLAG_UTF8 : 0;
    e->dostime = f->dostime;
    e->extattr = file_extattr(z, &f->info);
    e->offset  = start;
    e->zip64   = f->info.type==FT_FILE && f->info.size>=ZIP_MAX32;
    file_extras(z, f, e);

    zsrc src = {0};
    src.path = f->path;
    src.fd   = -1;
    if (f->info.type == FT_DIR) {
        u8 *h = newbytes(&scratch, zip_local_len(e));
        zout_write(w, h, zip_local(h, e)-h);
        return 1;
    } else if (f->info.type == FT_LINK) {
        src.mem = os_readlink(z->ctx, f->path, &z->perm, scratch);
        if (!src.mem.s) {
            return 0;
        }
    }

    b32 empty = f->info.type==FT_FILE && !f->info.size;
    b32 tryz  = z->level>0 && !empty;
    if (tryz) {
        e->flags |= level_flags(z->level);
    }

    for (;;) {
        e->method = tryz ? ZIP_DEFLATE : ZIP_STORE;
        e->crc    = 0;
        e->usize  = 0;
        zout_seek(w, start);  // drop an abandoned attempt, even on failure
        if (!src_open(z, &src, scratch)) {
            return 0;
        }

        iz  hlen = zip_local_len(e);
        u8 *h    = newbytes(&scratch, hlen);
        zip_local(h, e);
        zout_write(w, h, hlen);
        i64 data = zout_tell(w);

        b32 buffered = 0;
        if (tryz) {
            buffered = deflate_data(z, k, &src, &e->crc, &e->usize);
        } else {
            store_data(z, k, &src, &e->crc, &e->usize);
        }
        e->csize = zout_tell(w) - data;
        src_close(z, &src);
        if (src.err) {
            zout_seek(w, start);
            return 0;
        }

        if (tryz && e->csize>=e->usize) {
            tryz = 0;  // compression did not help: store instead
            if (buffered) {
                src.mem = (s8){k->buf, (iz)e->usize};  // no need to reread
            }
            continue;
        } else if (!e->zip64 && e->usize>=ZIP_MAX32) {
            e->zip64 = 1;  // grew past 4 GiB while reading
            continue;
        }
        zip_local(h, e);
        zout_patch(w, start, h, hlen);
        return 1;
    }
}

// Copy an entry from the existing archive without recompressing it.
static i32 copy_entry(zip *z, zarchive *ar, zwork *k, zentry *old,
                      zentry *e, arena scratch)
{
    zout *w = k->out;
    u8 fixed[ZIP_LOCAL_LEN];
    if (!os_readat(z->ctx, ar->fd, fixed, ZIP_LOCAL_LEN, old->offset)) {
        return fail(z, ZE_READ, S("Could not read archive"), z->archive,
                    scratch);
    }
    iz varlen = zip_local_varlen(fixed);
    i64 data  = old->offset + ZIP_LOCAL_LEN + varlen;
    if (varlen<0 || data>ar->end.cdoff || old->csize>ar->end.cdoff-data) {
        return fail(z, ZE_FORM, S("Zip file structure invalid"), old->name,
                    scratch);
    }
    u8 *var = newbytes(&scratch, varlen);
    if (!os_readat(z->ctx, ar->fd, var, varlen, old->offset+ZIP_LOCAL_LEN)) {
        return fail(z, ZE_READ, S("Could not read archive"), z->archive,
                    scratch);
    }

    *e = *old;
    iz nlen = get16(fixed+26);
    s8 lextra = {var+nlen, varlen-nlen};
    e->lextra = zip_filter_extra(&scratch, lextra, z->noextra);
    if (z->noextra) {
        e->cextra = zip_filter_extra(&z->perm, e->cextra, 1);
    }
    e->offset = zout_tell(w);
    e->zip64  = e->usize>=ZIP_MAX32 || e->csize>=ZIP_MAX32;

    // Sizes are now known, so a descriptor is unnecessary, except that
    // traditional encryption checks against the time when it is present.
    u16  both = ZIP_FLAG_ENCRYPTED | ZIP_FLAG_DESCRIPTOR;
    b32  desc = (e->flags & both) == both;
    if (!desc) {
        e->flags &= ~ZIP_FLAG_DESCRIPTOR;
    }

    u8 *h = newbytes(&scratch, zip_local_len(e));
    zout_write(w, h, zip_local(h, e)-h);
    for (i64 off = 0; off < e->csize;) {
        iz n = (iz)MIN(k->cap, e->csize-off);
        if (!os_readat(z->ctx, ar->fd, k->buf, n, data+off)) {
            return fail(z, ZE_READ, S("Could not read archive"), z->archive,
                        scratch);
        }
        zout_write(w, k->buf, n);
        off += n;
    }
    if (desc) {
        u8 d[24];
        zout_write(w, d, zip_desc(d, e)-d);
    }
    return 0;
}

enum { ITEM_KEEP, ITEM_ADD, ITEM_UPDATE, ITEM_FRESHEN, ITEM_DELETE };

typedef struct {
    i32     kind;
    zentry *old;
    zfile  *file;
} zitem;

typedef struct {
    zitem *data;
    iz     len;
    iz     cap;
} zitems;

// Print a progress line, with the compression result for a new entry.
static void report(zip *z, s8 verb, s8 name, zentry *e, arena scratch)
{
    if (z->quiet) {
        return;
    }
    s8 how = S("");
    if (e) {
        b32 stored = e->method == ZIP_STORE;
        i32 pct = stored ? 0 : zip_percent(e->usize, e->csize);
        how = JOIN(&scratch, S(" ("), stored ? S("stored ") : S("deflated "),
                   znum(&scratch, pct), S("%)"));
    }
    say(z, 1, JOIN(&scratch, verb, name, how, S("\n")));
}

// Temporary file beside the archive, created discarded-on-close. For a
// new archive it has the permissions of a new file from the start.
static i32 create_temp(zip *z, s8 *path, arena scratch)
{
    iz cut = z->archive.len;
    for (; cut>0 && !is_sep(z, z->archive.s[cut-1]); cut--) {}
    s8 dir = {z->archive.s, cut};
    i32 mode = OS_CREATE | (z->arcinfo.type==FT_NONE ? OS_DEFPERMS : 0);
    for (i32 i = 0; i < 1000000; i++) {
        s8 num = znum(&scratch, 1000000 + i);
        *path = JOIN(&z->perm, dir, S("zi"), (s8){num.s+1, 6});
        i32 fd = os_open(z->ctx, *path, mode, scratch);
        if (fd != OS_EEXIST) {
            return fd;
        }
    }
    return OS_ERR;
}

static i32 write_archive(zip *z, zarchive *ar, zitems *items, arena scratch)
{
    s8  temp = {0};
    i32 fd   = create_temp(z, &temp, scratch);
    if (fd < 0) {
        return fail(z, ZE_CREAT, S("Could not create output file"), temp,
                    scratch);
    }

    zout w = {0};
    w.ctx = z->ctx;
    w.fd  = fd;
    w.cap = 1 << 20;
    w.buf = newbytes(&scratch, w.cap);

    zwork k = {0};
    k.out = &w;
    k.cap = 1 << 18;
    k.buf = newbytes(&scratch, k.cap);
    if (z->level) {
        arena a = subarena(&scratch, deflate_memsize());
        k.def = deflate_new(&a, z->level);
    }

    zentry *entries = new(&scratch, items->len, zentry);
    iz      count   = 0;
    for (iz i = 0; i < items->len; i++) {
        zitem  *it   = items->data + i;
        zentry *e    = entries + count;
        zentry *copy = 0;
        switch (it->kind) {
        case ITEM_DELETE:
            report(z, S("deleting: "), it->old->name, 0, scratch);
            break;
        case ITEM_KEEP:
            copy = it->old;
            break;
        default:
            if (write_file(z, &k, it->file, e, scratch)) {
                s8 verb = it->kind==ITEM_ADD    ? S("  adding: ") :
                          it->kind==ITEM_UPDATE ? S("updating: ") :
                                                  S("freshening: ");
                report(z, verb, e->name, e, scratch);
                count++;
                break;
            }
            warn(z, S("could not open for reading: "), it->file->path,
                 scratch);
            z->status = ZE_OPEN;
            if (it->old) {
                // Keep the entry it was to replace, as Info-ZIP does
                warn(z, S("will just copy entry over: "), it->old->name,
                     scratch);
                copy = it->old;
            } else {
                z->nskipped++;
                z->bskipped += it->file->info.size;
            }
        }
        if (copy) {
            i32 err = copy_entry(z, ar, &k, copy, e, scratch);
            if (err) {
                os_close(z->ctx, fd);
                return err;
            }
            count++;
        }
        if (w.err) {
            break;
        }
    }

    i64 cdoff = zout_tell(&w);
    for (iz i = 0; i < count; i++) {
        u8 *h = newbytes(&scratch, zip_central_len(entries+i));
        zout_write(&w, h, zip_central(h, entries+i)-h);
    }
    i64 cdsize  = zout_tell(&w) - cdoff;
    s8  comment = ar ? ar->end.comment : (s8){0};
    u8 *end = newbytes(&scratch, zip_end_len(count, cdsize, cdoff, comment));
    zout_write(&w, end, zip_end(end, count, cdsize, cdoff, comment)-end);
    zout_flush(&w);

    if (ar) {
        os_close(z->ctx, ar->fd);  // before replacing it
        ar->fd = -1;
    }
    if (w.err || !os_truncate(z->ctx, fd, zout_tell(&w))) {
        os_close(z->ctx, fd);
        return fail(z, ZE_WRITE, S("Output file write failure"), temp,
                    scratch);
    }
    if (!count) {
        warn(z, S("zip file empty"), S(""), scratch);
    }
    if (!os_commit(z->ctx, fd, z->archive, scratch)) {
        return fail(z, ZE_CREAT, S("Could not replace archive"), z->archive,
                    scratch);
    }
    return 0;
}

static b32 has_extension(zip *z, s8 path)
{
    for (iz i = path.len; i > 0; i--) {
        u8 c = path.s[i-1];
        if (c == '.') {
            return 1;
        } else if (is_sep(z, c) || (z->windows && c==':')) {
            return 0;
        }
    }
    return 0;
}

static i32 parse_epoch(zip *z, s8 s, arena scratch)
{
    if (!s.s) {
        return 0;
    }
    i64 v = 0;
    for (iz i = 0; i < s.len; i++) {
        if (s.s[i]<'0' || s.s[i]>'9' || v>(i64)1<<40) {
            return fail(z, ZE_PARMS, S("Invalid SOURCE_DATE_EPOCH"), s,
                        scratch);
        }
        v = v*10 + (s.s[i] - '0');
    }
    if (!s.len) {
        return fail(z, ZE_PARMS, S("Invalid SOURCE_DATE_EPOCH"), s, scratch);
    }
    z->haveepoch = 1;
    z->epoch     = v;
    return 0;
}

// Mark the entries matching a -d pattern that pass -i and -x, which
// Info-ZIP applies to deletions too. Returns whether any entry matched,
// marked or not.
static b32 mark_deletes(zip *z, zentry *entries, iz n, s8 pattern, b32 *hit)
{
    s8s one = {&pattern, 1, 1};
    b32 any = 0;
    for (iz i = 0; i < n; i++) {
        if (any_match(z, &one, entries[i].name)) {
            hit[i] |= included(z, entries[i].name);
            any = 1;
        }
    }
    return any;
}

// Key for finding an archive entry by name: on Windows, ignoring ASCII
// case, as Info-ZIP's name comparison does there.
static s8 entry_key(zip *z, s8 name, arena *a)
{
    if (!z->windows) {
        return name;
    }
    s8 key = {newbytes(a, name.len), name.len};
    for (iz i = 0; i < name.len; i++) {
        key.s[i] = zip_fold(name.s[i], ZIP_FOLD);
    }
    return key;
}

// Mark the entry for a -d name on disk, which Info-ZIP takes literally,
// wildcards and all: a directory names its "dir/" entry, unless -D, and
// a hidden or system file (Windows) nothing, unless -S.
static void mark_named(zip *z, zmap *old, s8 path, os_info *info, b32 *hit,
                       arena scratch)
{
    s8 name = zip_name(&scratch, path, z->windows);
    if (info->type == FT_DIR) {
        if (z->nodirs || !name.len) {
            return;
        } else if (name.s[name.len-1] != '/') {
            name = JOIN(&scratch, name, S("/"));
        }
    } else if (hidden_file(z, info)) {
        return;
    }
    iz *v = zmap_upsert(&old, entry_key(z, name, &scratch), 0);
    if (v && included(z, name)) {
        hit[*v] = 1;
    }
}

static i32 zip_main(zipconfig *conf)
{
    zip *z = new(&conf->perm, 1, zip);
    z->ctx     = conf->perm.ctx;
    z->level   = 6;
    z->windows = conf->windows;
    arena scratch = conf->perm;  // reset below, once perm is carved out

    // Without arguments, Info-ZIP streams standard input to standard
    // output, unless that is a terminal, which gets the usage instead
    if (!conf->nargs && os_isatty(z->ctx, 1)) {
        return usage(z, 0);
    }

    // Scratch takes half of the remaining memory
    iz half = (conf->perm.end - conf->perm.beg) / 2;
    z->perm = conf->perm;
    z->perm.end -= half;
    scratch.beg = z->perm.end;
    scratch.end = conf->perm.end;

    // Options from the environment precede the arguments, as in Info-ZIP:
    // those of ZIPOPT or, if it has none, of ZIP
    s8s env = env_args(z, conf->zipopt);
    env = env.len ? env : env_args(z, conf->zipenv);
    for (i32 i = 0; i < conf->nargs; i++) {
        *push(&z->perm, &env) = conf->args[i];
    }
    s8 *args  = env.data;
    i32 nargs = (i32)env.len;

    if (nargs==1 && zequals(args[0], S("-v"))) {
        version(z);
        return 0;
    }
    i32 err = parse_args(z, args, nargs, scratch);
    if (err) {
        return err<0 ? 0 : err;
    }
    err = parse_epoch(z, conf->epoch, scratch);
    if (err) {
        return err;
    }
    if (!z->archive.s) {
        s8 why = os_isatty(z->ctx, 1) ? S("cannot write zip file to terminal")
               : S("streaming to standard output not supported");
        return fail(z, ZE_PARMS, S("Invalid command arguments"), why, scratch);
    }
    if (z->mode==MODE_DELETE && (z->recurse || !z->level)) {
        warn(z, S("invalid option(s) used with -d; ignored."), S(""),
             scratch);
    }
    if (!has_extension(z, z->archive)) {
        z->archive = JOIN(&z->perm, z->archive, S(".zip"));
    }
    if (z->names_stdin) {
        s8 text = read_all(z, 0, &z->perm);
        if (!text.s) {
            return fail(z, ZE_READ, S("Could not read names"),
                        S("standard input"), scratch);
        }
        // These names come before the arguments, as in Info-ZIP
        s8s names = {0};
        push_lines(z, &names, text);
        for (iz i = 0; i < z->paths.len; i++) {
            *push(&z->perm, &names) = z->paths.data[i];
        }
        z->paths = names;
    }

    zarchive  arc = {0};
    zarchive *ar  = 0;
    z->arcexists = os_stat(z->ctx, z->archive, 1, &z->arcinfo, scratch) &&
                   z->arcinfo.type==FT_FILE && z->arcinfo.size>0;
    if (z->arcexists) {
        ar = &arc;
        err = read_archive(z, ar, scratch);
        if (err) {
            return err;
        }
    } else if (z->mode==MODE_DELETE || z->mode==MODE_FRESHEN) {
        warn(z, z->archive, S(" not found or empty"), scratch);
        return fail(z, ZE_NONE, S("Nothing to do!"), z->archive, scratch);
    }
    iz nold = ar ? (iz)ar->end.count : 0;

    // Existing entries by name, the first of any duplicates. On Windows,
    // a file then replaces an entry whose name differs only in case, as
    // in Info-ZIP, and the entry keeps its name.
    zmap *old = 0;
    for (iz i = 0; i < nold; i++) {
        s8  key = entry_key(z, ar->entries[i].name, &scratch);
        iz *v   = zmap_upsert(&old, key, &scratch);
        *v = *v<0 ? i : *v;
    }

    zitems items = {0};
    b32 changed = 0;
    if (z->mode == MODE_DELETE) {
        b32 *hit = new(&scratch, nold, b32);
        for (iz p = 0; p < z->paths.len; p++) {
            s8 arg = z->paths.data[p];
            os_info info = {0};
            if (os_stat(z->ctx, arg, !z->symlinks, &info, scratch)) {
                // Info-ZIP does not warn about names on disk
                mark_named(z, old, arg, &info, hit, scratch);
            } else {
                s8 pattern = zip_name(&scratch, arg, z->windows);
                if (!mark_deletes(z, ar->entries, nold, pattern, hit)) {
                    warn(z, S("name not matched: "), arg, scratch);
                }
            }
        }
        for (iz i = 0; i < nold; i++) {
            zitem *it = push(&z->perm, &items);
            it->kind = hit[i] ? ITEM_DELETE : ITEM_KEEP;
            it->old  = ar->entries + i;
            it->file = 0;
            changed |= hit[i];
        }
    } else {
        for (iz p = 0; p < z->paths.len; p++) {
            scan_arg(z, z->paths.data[p], scratch);
        }
        if (z->duplicate) {
            return fail(z, ZE_PARMS, S("Invalid command arguments"),
                        S("cannot repeat names in zip file"), scratch);
        } else if (z->mode==MODE_SYNC && !z->files.len) {
            // Rather than delete every entry, as for a misspelled path
            return fail(z, ZE_NONE, S("Nothing to do!"), z->archive, scratch);
        }

        for (iz i = 0; i < nold; i++) {
            zitem *it = push(&z->perm, &items);
            it->kind = z->mode==MODE_SYNC ? ITEM_DELETE : ITEM_KEEP;
            it->old  = ar->entries + i;
            it->file = 0;
        }

        for (iz i = 0; i < z->files.len; i++) {
            zfile *f = z->files.data + i;
            iz *v = zmap_upsert(&old, entry_key(z, f->name, &scratch), 0);
            if (!v) {
                if (z->mode != MODE_FRESHEN) {
                    zitem *it = push(&z->perm, &items);
                    it->kind = ITEM_ADD;
                    it->old  = 0;
                    it->file = f;
                    changed  = 1;
                }
                continue;
            }

            zitem  *it = items.data + *v;
            zentry *e  = it->old;
            b32 replace = 0;
            switch (z->mode) {
            case MODE_ADD:
                replace = 1;
                break;
            case MODE_UPDATE:
            case MODE_FRESHEN:
                replace = f->dostime > e->dostime;
                break;
            case MODE_SYNC:
                replace = f->dostime!=e->dostime ||
                          (f->info.type==FT_FILE && f->info.size!=e->usize);
                it->kind = ITEM_KEEP;
                break;
            }
            if (replace) {
                it->kind = z->mode==MODE_FRESHEN ? ITEM_FRESHEN : ITEM_UPDATE;
                it->file = f;
                f->name  = e->name;
                changed  = 1;
            }
        }
        for (iz i = 0; i < nold; i++) {
            changed |= items.data[i].kind == ITEM_DELETE;
        }
    }

    if (!changed) {
        if (z->files.len) {
            return z->status;  // already up to date
        }
        return fail(z, ZE_NONE, S("Nothing to do!"), z->archive, scratch);
    }

    err = write_archive(z, ar, &items, scratch);
    if (err) {
        return err;
    }
    if (z->nskipped) {
        warn(z, S("Not all files were readable"), S(""), scratch);
        s8 msg = JOIN(&scratch, S("  files/entries skipped: "),
                      znum(&scratch, z->nskipped), S(" ("),
                      znum(&scratch, z->bskipped), S(" bytes)\n"));
        if (!z->quiet) {
            say(z, 2, msg);
        }
    }
    return z->status;
}
