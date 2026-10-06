// tugz core: command line interface
//
// Exit status follows GNU gzip: 0 for success, 1 for errors, 2 for
// warnings (e.g. trailing garbage, file skipped). Errors take precedence.

typedef struct {
    arena perm;
    s8    name;   // program name without directory or extension
    s8   *args;   // excluding the program name
    i32   nargs;
} config;

enum {
    EXIT_OK   = 0,
    EXIT_ERR  = 1,
    EXIT_WARN = 2,
};

typedef struct {
    i32 level;
    b32 decompress;
    b32 test;
    b32 to_stdout;
    b32 force;
    b32 keep;
    b32 quiet;
    encoder *enc;  // when compressing, shared by every file
    decoder *dec;  // when decompressing or testing, likewise
} options;

static s8 const usage_text = S8(
    "usage: gzip [-123456789cdfhkqtV] [FILE]...\n"
    "Compress or decompress FILEs (or standard input).\n"
    "  -1..-9  compression level (default 6)\n"
    "  -c      write to standard output, keep input files\n"
    "  -d      decompress\n"
    "  -f      force: overwrite outputs, follow links, allow terminals,\n"
    "          and with -dc copy data that is not gzip unchanged\n"
    "  -h      print this message\n"
    "  -k      keep input files\n"
    "  -q      suppress warnings\n"
    "  -t      test compressed file integrity\n"
    "  -V      print version\n"
);

static i32 exit_combine(i32 a, i32 b)
{
    if (a==EXIT_ERR || b==EXIT_ERR) {
        return EXIT_ERR;
    }
    return a==EXIT_WARN || b==EXIT_WARN ? EXIT_WARN : EXIT_OK;
}

static s8 s8concat(arena *a, s8 x, s8 y)
{
    s8 r = {newbytes(a, x.len+y.len), x.len+y.len};
    bytecopy(r.s, x.s, x.len);
    bytecopy(r.s+x.len, y.s, y.len);
    return r;
}

static b32 ascii_iequals(s8 a, s8 b)
{
    if (a.len != b.len) {
        return 0;
    }
    for (iz i = 0; i < a.len; i++) {
        u8 x = a.s[i]>='A' && a.s[i]<='Z' ? a.s[i]+32 : a.s[i];
        u8 y = b.s[i]>='A' && b.s[i]<='Z' ? b.s[i]+32 : b.s[i];
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

static b32 has_suffix(s8 s, s8 suffix)
{
    if (s.len <= suffix.len) {
        return 0;  // a bare suffix is not a name with that suffix
    }
    s8 tail = {s.s + s.len - suffix.len, suffix.len};
    return ascii_iequals(tail, suffix);
}

// Output name for decompression, or a null string if unrecognized.
static s8 strip_suffix(arena *a, s8 path)
{
    s8 r = {0};
    if (has_suffix(path, S(".gz"))) {
        r = path;
        r.len -= 3;
    } else if (has_suffix(path, S(".tgz"))) {
        r = path;
        r.len -= 4;
        r = s8concat(a, r, S(".tar"));
    }
    return r;
}

static void message(arena scratch, s8 name, s8 msg)
{
    writer *w = newwriter(&scratch, 2, 512);
    writer_s8(w, S("gzip: "));
    if (name.len) {
        writer_s8(w, name);
        writer_s8(w, S(": "));
    }
    writer_s8(w, msg);
    writer_byte(w, '\n');
    writer_flush(w);
}

static s8 status_message(i32 status)
{
    switch (status) {
    case GZ_TRAILING: return S("decompression OK, trailing garbage ignored");
    case GZ_ENOTGZ:   return S("not in gzip format");
    case GZ_EMETHOD:  return S("unknown compression method");
    case GZ_EFLAGS:   return S("unknown header flags set");
    case GZ_EHCRC:    return S("header crc mismatch");
    case GZ_ETRUNC:   return S("unexpected end of file");
    case GZ_EDATA:    return S("invalid compressed data--format violated");
    case GZ_ECRC:     return S("invalid compressed data--crc error");
    case GZ_ELEN:     return S("invalid compressed data--length error");
    case GZ_EREAD:    return S("read error");
    case GZ_EWRITE:   return S("write error");
    }
    return S("unknown error");
}

static i32 report(options *o, s8 name, i32 status, arena scratch)
{
    if (status == GZ_OK) {
        return EXIT_OK;
    } else if (status == GZ_TRAILING) {
        if (!o->quiet) {
            message(scratch, name, status_message(status));
        }
        return EXIT_WARN;
    }
    message(scratch, name, status_message(status));
    return EXIT_ERR;
}

static i32 warn(options *o, s8 name, s8 msg, arena scratch)
{
    if (!o->quiet) {
        message(scratch, name, msg);
    }
    return EXIT_WARN;
}

// As in GNU gzip, forced decompression to standard output copies data
// that is not gzip through unchanged (zcat -f), and a forced test passes
// it. In place, it is still an error.
static i32 transform(options *o, i32 in, i32 out, b32 in_place,
                     arena scratch)
{
    b32 copy = o->force && !in_place;
    if (o->test) {
        return stream_decompress(o->dec, in, -1, copy, scratch);
    } else if (o->decompress) {
        return stream_decompress(o->dec, in, out, copy, scratch);
    }
    return gzip_compress(o->enc, in, out, o->level, scratch);
}

// Like GNU gzip, when using standard input, refuse to write compressed
// data to a terminal or to read it from one, unless forced. Named files
// with -c are not checked. Returns -1 to continue, or an exit status.
static i32 check_terminal(options *o, arena scratch)
{
    os *ctx = scratch.ctx;
    b32 compress = !o->decompress && !o->test;
    if (o->force) {
        return -1;
    } else if (compress && os_isatty(ctx, 1)) {
        message(scratch, (s8){0}, S(
            "compressed data not written to a terminal. "
            "Use -f to force compression."
        ));
        return EXIT_ERR;
    } else if (!compress && os_isatty(ctx, 0)) {
        message(scratch, (s8){0}, S(
            "compressed data not read from a terminal. "
            "Use -f to force decompression."
        ));
        return EXIT_ERR;
    }
    return -1;
}

static i32 process_file(options *o, s8 path, arena scratch)
{
    os *ctx = scratch.ctx;

    if (s8equals(path, S("-"))) {
        i32 r = check_terminal(o, scratch);
        if (r >= 0) {
            return r;
        }
        i32 status = transform(o, 0, 1, 0, scratch);
        return report(o, S("stdin"), status, scratch);
    }

    b32 in_place = !o->to_stdout && !o->test;
    if (in_place && !o->decompress && has_suffix(path, S(".gz"))) {
        return warn(o, path, S("already has .gz suffix -- unchanged"), scratch);
    }

    s8 outpath = {0};
    if (in_place) {
        outpath = o->decompress ? strip_suffix(&scratch, path)
                                : s8concat(&scratch, path, S(".gz"));
        if (!outpath.s) {
            return warn(o, path, S("unknown suffix -- ignored"), scratch);
        }
    }

    // The input of an in-place operation is deleted afterward, so only
    // plain files qualify. Forcing permits links, which are followed.
    i32 mode = OS_READ;
    if (in_place) {
        mode |= OS_REGULAR;
        mode |= o->force ? 0 : OS_NOFOLLOW|OS_ONELINK;
    }
    i32 in = os_open(ctx, path, mode, scratch);
    switch (in) {
    case OS_EISDIR:
        return warn(o, path, S("is a directory -- ignored"), scratch);
    case OS_ESYMLINK:
        return warn(o, path, S("is a symbolic link -- ignored"), scratch);
    case OS_ENOTREG:
        return warn(o, path, S("is not a directory or a regular file -- ignored"), scratch);
    case OS_ELINKS:
        return warn(o, path, S("has other links -- file ignored"), scratch);
    }
    if (in < 0) {
        message(scratch, path, S("cannot open for reading"));
        return EXIT_ERR;
    }

    if (!in_place) {
        i32 status = transform(o, in, 1, 0, scratch);
        os_close(ctx, in);
        return report(o, path, status, scratch);
    }

    i32 out = os_open(ctx, outpath, o->force ? OS_FORCE : OS_CREATE, scratch);
    if (out == OS_EEXIST) {
        os_close(ctx, in);
        return warn(o, outpath, S("already exists; not overwritten"), scratch);
    } else if (out < 0) {
        os_close(ctx, in);
        message(scratch, outpath, S("cannot open for writing"));
        return EXIT_ERR;
    }

    // The output is discarded on close unless explicitly kept, so that
    // failures and interruptions never leave a partial file behind.
    i32 status = transform(o, in, out, 1, scratch);
    b32 ok = status==GZ_OK || status==GZ_TRAILING;
    if (ok) {
        os_copymeta(ctx, in, out);
        os_keep(ctx, out);
    }
    // A failed close may mean lost data, so the input must survive it
    if (!os_close(ctx, out) && ok) {
        os_remove(ctx, outpath, scratch);
        status = GZ_EWRITE;
        ok = 0;
    }
    os_close(ctx, in);
    if (!ok) {
        return report(o, path, status, scratch);
    }

    i32 code = report(o, path, status, scratch);
    if (!o->keep && !os_remove(ctx, path, scratch)) {
        message(scratch, path, S("cannot remove input file"));
        code = EXIT_ERR;
    }
    return code;
}

static void print(arena scratch, i32 fd, s8 s)
{
    writer *w = newwriter(&scratch, fd, 512);
    writer_s8(w, s);
    writer_flush(w);
}

// Map a long option to its short equivalent, or 0 if unknown.
static i32 parse_long(s8 arg, arena scratch)
{
    static struct {
        s8  name;
        i32 which;
    } const longopts[] = {
        {S8("--best"),       '9'},
        {S8("--decompress"), 'd'},
        {S8("--fast"),       '1'},
        {S8("--force"),      'f'},
        {S8("--help"),       'h'},
        {S8("--keep"),       'k'},
        {S8("--quiet"),      'q'},
        {S8("--stdout"),     'c'},
        {S8("--test"),       't'},
        {S8("--to-stdout"),  'c'},
        {S8("--uncompress"), 'd'},
        {S8("--version"),    'V'},
    };
    for (iz i = 0; i < countof(longopts); i++) {
        if (s8equals(arg, longopts[i].name)) {
            return longopts[i].which;
        }
    }
    message(scratch, (s8){0}, s8concat(&scratch, S("unknown option: "), arg));
    return 0;
}

// Apply a single-letter option. Returns -1 to continue, or exit status.
static i32 apply_option(options *o, i32 c, arena scratch)
{
    switch (c) {
    case '1': case '2': case '3': case '4': case '5':
    case '6': case '7': case '8': case '9':
              o->level      = c - '0'; return -1;
    case 'c': o->to_stdout  = 1;       return -1;
    case 'd': o->decompress = 1;       return -1;
    case 'f': o->force      = 1;       return -1;
    case 'k': o->keep       = 1;       return -1;
    case 'q': o->quiet      = 1;       return -1;
    case 't': o->test       = 1;       return -1;
    case 'h':
        print(scratch, 1, usage_text);
        return EXIT_OK;
    case 'V':
        print(scratch, 1, S("gzip (tugz) 1.0\n"));
        return EXIT_OK;
    }
    print(scratch, 2, usage_text);
    return EXIT_ERR;
}

static b32 ascii_iprefix(s8 s, s8 prefix)
{
    s8 head = {s.s, MIN(s.len, prefix.len)};
    return ascii_iequals(head, prefix);
}

// Like GNU gzip, the program name selects a default mode: gunzip (or
// any un* name) decompresses, and zcat or gzcat decompress to standard
// output.
static void apply_name(options *o, s8 name)
{
    if (ascii_iprefix(name, S("un")) || ascii_iprefix(name, S("gun"))) {
        o->decompress = 1;
    } else if (ascii_iequals(name, S("zcat")) ||
               ascii_iequals(name, S("gzcat"))) {
        o->decompress = 1;
        o->to_stdout = 1;
    }
}

static i32 gzip_main(config *conf)
{
    arena *perm = &conf->perm;
    options o = {0};
    o.level = 6;
    apply_name(&o, conf->name);

    s8 *files = new(perm, conf->nargs+1, s8);
    i32 nfiles = 0;
    b32 endopts = 0;
    for (i32 i = 0; i < conf->nargs; i++) {
        s8 arg = conf->args[i];
        if (endopts || arg.len<2 || arg.s[0]!='-') {
            files[nfiles++] = arg;
        } else if (s8equals(arg, S("--"))) {
            endopts = 1;
        } else if (arg.s[1] == '-') {
            i32 r = apply_option(&o, parse_long(arg, *perm), *perm);
            if (r >= 0) {
                return r;
            }
        } else {
            for (iz j = 1; j < arg.len; j++) {
                i32 r = apply_option(&o, arg.s[j], *perm);
                if (r >= 0) {
                    return r;
                }
            }
        }
    }

    if (!nfiles) {
        files[nfiles++] = S("-");
    }
    if (o.decompress || o.test) {
        o.dec = stream_decoder(perm, FMT_GZIP);
    } else {
        o.enc = gzip_encoder(perm, o.level);
    }

    i32 code = EXIT_OK;
    for (i32 i = 0; i < nfiles; i++) {
        code = exit_combine(code, process_file(&o, files[i], *perm));
    }
    return code;
}
