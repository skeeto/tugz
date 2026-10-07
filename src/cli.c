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
    b32 stop;      // a read or write error ends the run, as in GNU gzip
    encoder *enc;  // when compressing, shared by every file
    decoder *dec;  // when decompressing or testing, likewise
} options;

static s8 const usage_text = S8(
    "usage: gzip [-123456789cdfhknqtV] [FILE]...\n"
    "Compress or decompress FILEs (or standard input).\n"
    "  -1..-9  compression level (default 6)\n"
    "  -c      write to standard output, keep input files\n"
    "  -d      decompress\n"
    "  -f      force: overwrite outputs, follow links, allow terminals,\n"
    "          and with -dc copy data that is not gzip unchanged\n"
    "  -h      print this message\n"
    "  -k      keep input files\n"
    "  -n      neither save nor restore the name and time (as always)\n"
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

// The suffix of a compressed file's name, as written in it (regardless
// of case), or a null string: GNU gzip's list. A suffix alone, or alone
// after a directory separator (/ or \), is not a name with that suffix.
static s8 known_suffix(s8 path)
{
    static s8 const suffixes[] = {
        S8(".gz"), S8(".z"), S8("-gz"), S8("-z"), S8("_z"),
        S8(".tgz"), S8(".taz"),
    };
    for (iz i = 0; i < countof(suffixes); i++) {
        iz len = suffixes[i].len;
        if (path.len <= len) {
            continue;
        }
        s8 tail = {path.s + path.len - len, len};
        u8 prev = tail.s[-1];
        if (prev!='/' && prev!='\\' && ascii_iequals(tail, suffixes[i])) {
            return tail;
        }
    }
    return (s8){0};
}

// Output name for decompression, or a null string if unrecognized: the
// suffix is dropped, except that .tgz and .taz become .tar.
static s8 strip_suffix(arena *a, s8 path)
{
    s8 suffix = known_suffix(path);
    if (!suffix.s) {
        return suffix;
    }
    s8 r = {path.s, path.len - suffix.len};
    if (ascii_iequals(suffix, S(".tgz")) || ascii_iequals(suffix, S(".taz"))) {
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
    case GZ_ETRUNC:   return S("unexpected end of file");
    case GZ_EDATA:    return S("invalid compressed data--format violated");
    case GZ_ECRC:     return S("invalid compressed data--crc error");
    case GZ_ELEN:     return S("invalid compressed data--length error");
    }
    return S("unknown error");
}

static i32 warn(options *o, s8 name, s8 msg, arena scratch)
{
    if (!o->quiet) {
        message(scratch, name, msg);
    }
    return EXIT_WARN;
}

// A warning that runs on from the name, as some of GNU gzip's do.
static i32 warn_that(options *o, s8 name, s8 msg, arena scratch)
{
    s8 line = s8concat(&scratch, name, msg);
    return warn(o, (s8){0}, line, scratch);
}

static void writer_num(writer *w, u32 v, u32 base, i32 width)
{
    u8  buf[32];
    i32 len = 0;
    for (; v || len<width; v /= base) {
        buf[countof(buf) - ++len] = (u8)"0123456789abcdef"[v % base];
    }
    writer_write(w, buf+countof(buf)-len, len);
}

// Report a bad gzip header as GNU gzip does, from the bytes the decoder
// holds: the method byte, the flags, or the header check.
static void bad_header(decoder *z, s8 name, i32 status, arena scratch)
{
    writer *w = newwriter(&scratch, 2, 512);
    writer_s8(w, S("gzip: "));
    writer_s8(w, name);
    switch (status) {
    case GZ_EMETHOD:
        writer_s8(w, S(": unknown method "));
        writer_num(w, z->buf[2], 10, 1);
        writer_s8(w, S(" -- not supported"));
        break;
    case GZ_EFLAGS:
        if (z->buf[3] & 0x20) {
            writer_s8(w, S(" is encrypted -- not supported"));
        } else {
            writer_s8(w, S(" has flags 0x"));
            writer_num(w, z->buf[3], 16, 1);
            writer_s8(w, S(" -- not supported"));
        }
        break;
    case GZ_EHCRC:
        writer_s8(w, S(": header checksum 0x"));
        writer_num(w, (u32)(z->buf[0] | z->buf[1]<<8), 16, 4);
        writer_s8(w, S(" != computed checksum 0x"));
        writer_num(w, z->hcrc & 0xffff, 16, 4);
        break;
    }
    writer_byte(w, '\n');
    writer_flush(w);
}

// Why an os_* call just failed, as GNU gzip says in the system's words,
// or where those are unknown, a description.
static s8 reason(os *ctx, s8 otherwise)
{
    s8 why = os_error(ctx);
    return why.len ? why : otherwise;
}

// Report a transform's status, from reading in and writing out. As in
// GNU gzip, a read or write error names the file that failed, with the
// system's reason, and ends the run: as an error, or for a closed pipe,
// which the default SIGPIPE would have ended quietly, as a warning.
static i32 report(options *o, s8 in, s8 out, i32 status, arena scratch)
{
    os *ctx = scratch.ctx;
    switch (status) {
    case GZ_OK:
        return EXIT_OK;
    case GZ_TRAILING:
        return warn(o, in, status_message(status), scratch);
    case GZ_EREAD:
        o->stop = 1;
        message(scratch, in, reason(ctx, S("read error")));
        return EXIT_ERR;
    case GZ_EWRITE:
        o->stop = 1;
        if (os_pipeclosed(ctx)) {
            return warn(o, out, reason(ctx, S("broken pipe")), scratch);
        }
        message(scratch, out, reason(ctx, S("write error")));
        return EXIT_ERR;
    case GZ_EMETHOD:
    case GZ_EFLAGS:
    case GZ_EHCRC:
        bad_header(o->dec, in, status, scratch);
        return EXIT_ERR;
    }
    message(scratch, in, status_message(status));
    return EXIT_ERR;
}

// Transform a file to standard output, or test it. As in GNU gzip,
// forced decompression to standard output copies data that is not gzip
// through unchanged (zcat -f), and a forced test passes it.
static i32 transform(options *o, i32 in, i32 out, arena scratch)
{
    b32 copy = o->force;
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

// Open an input file. As in GNU gzip, decompressing or testing a missing
// name without a suffix tries it with suffixes in turn (zcat foo reads
// foo.gz). The path becomes the name found, or else the first one tried.
static i32 find_input(options *o, s8 *path, i32 mode, arena *scratch)
{
    os *ctx = scratch->ctx;
    i32 in = os_open(ctx, *path, mode, *scratch);
    if (in!=OS_ERR || !(o->decompress || o->test) || !os_missing(ctx) ||
        known_suffix(*path).s) {
        return in;
    }

    static s8 const tries[] = {S8(".gz"), S8(".z"), S8("-z"), S8(".Z")};
    s8 first = s8concat(scratch, *path, tries[0]);
    for (iz i = 0; i < countof(tries); i++) {
        s8 name = i ? s8concat(scratch, *path, tries[i]) : first;
        in = os_open(ctx, name, mode, *scratch);
        if (in!=OS_ERR || !os_missing(ctx)) {
            *path = name;
            return in;
        }
    }
    *path = first;
    return in;
}

static i32 process_file(options *o, s8 path, arena scratch)
{
    os *ctx = scratch.ctx;

    if (s8equals(path, S("-"))) {
        i32 r = check_terminal(o, scratch);
        if (r >= 0) {
            return r;
        }
        i32 status = transform(o, 0, 1, scratch);
        return report(o, S("stdin"), S("stdout"), status, scratch);
    }

    // The input of an in-place operation is deleted afterward, so only
    // plain files qualify. Forcing permits links, which are followed, and
    // the sticky bit, but as in GNU gzip, never set-ID bits.
    b32 in_place = !o->to_stdout && !o->test;
    i32 mode = OS_READ;
    if (in_place) {
        mode |= OS_REGULAR | OS_NOSETID;
        mode |= o->force ? 0 : OS_NOFOLLOW|OS_ONELINK|OS_NOSTICKY;
    }
    i32 in = find_input(o, &path, mode, &scratch);
    switch (in) {
    case OS_EISDIR:
        return warn_that(o, path, S(" is a directory -- ignored"), scratch);
    case OS_ESYMLINK:
        // An error, not a warning, as GNU gzip's refusal (ELOOP) is
        message(scratch, path, S("is a symbolic link -- ignored"));
        return EXIT_ERR;
    case OS_ENOTREG:
        return warn_that(o, path, S(" is not a directory or a regular file "
                                    "- ignored"), scratch);
    case OS_ESETUID:
        return warn_that(o, path, S(" is set-user-ID on execution - ignored"),
                         scratch);
    case OS_ESETGID:
        return warn_that(o, path, S(" is set-group-ID on execution - ignored"),
                         scratch);
    case OS_ESTICKY:
        return warn_that(o, path, S(" has the sticky bit set - file ignored"),
                         scratch);
    case OS_ELINKS:  // which GNU gzip counts
        return warn_that(o, path, S(" has other links -- file ignored"),
                         scratch);
    }
    if (in < 0) {
        message(scratch, path, reason(ctx, S("cannot open for reading")));
        return EXIT_ERR;
    }

    if (!in_place) {
        i32 status = transform(o, in, 1, scratch);
        i32 code = report(o, path, S("stdout"), status, scratch);
        os_close(ctx, in);
        return code;
    }

    // Like GNU gzip, check the name once the input qualifies. Leaving a
    // file named as compressed is no failure, and -f compresses it anyway.
    s8 outpath = {0};
    if (o->decompress) {
        outpath = strip_suffix(&scratch, path);
        if (!outpath.s) {
            // Under -q, GNU gzip leaves even the status at 0
            os_close(ctx, in);
            return o->quiet ? EXIT_OK
                            : warn(o, path, S("unknown suffix -- ignored"),
                                   scratch);
        }
    } else {
        s8 suffix = known_suffix(path);
        if (suffix.s && !o->force) {
            os_close(ctx, in);
            if (!o->quiet) {
                s8 msg = s8concat(&scratch, path, S(" already has "));
                msg = s8concat(&scratch, msg, suffix);
                msg = s8concat(&scratch, msg, S(" suffix -- unchanged"));
                message(scratch, (s8){0}, msg);
            }
            return EXIT_OK;
        }
        outpath = s8concat(&scratch, path, S(".gz"));
    }

    // Take the input's metadata now, as GNU gzip does, before reading
    // changes its access time
    osmeta *meta = os_getmeta(ctx, in, &scratch);
    if (!meta) {
        message(scratch, path, reason(ctx, S("cannot read metadata")));
        os_close(ctx, in);
        return EXIT_ERR;
    }

    // As in GNU gzip, read the header before creating the output, so that
    // input that is not gzip is the error, and never replaces a file
    reader *r = 0;
    if (o->decompress) {
        r = stream_reader(&scratch, in);
        if (!stream_header(o->dec, r)) {
            i32 status = stream_decode(o->dec, r, -1, 0);
            i32 code = report(o, path, outpath, status, scratch);
            os_close(ctx, in);
            return code;
        }
    }

    i32 out = os_open(ctx, outpath, o->force ? OS_FORCE : OS_CREATE, scratch);
    if (out == OS_EEXIST) {
        // A warning in GNU gzip's words, which it gives even under -q
        os_close(ctx, in);
        s8 msg = S(" already exists;\tnot overwritten");
        msg = s8concat(&scratch, outpath, msg);
        message(scratch, (s8){0}, msg);
        return EXIT_WARN;
    } else if (out < 0) {
        message(scratch, outpath, reason(ctx, S("cannot open for writing")));
        os_close(ctx, in);
        return EXIT_ERR;
    }

    // The output is discarded on close unless explicitly kept, so that
    // failures and interruptions never leave a partial file behind. In
    // place, data that is not gzip is an error even when forced.
    i32 status = r ? stream_decode(o->dec, r, out, 0)
                   : gzip_compress(o->enc, in, out, o->level, scratch);
    i32 code = report(o, path, outpath, status, scratch);
    if (status!=GZ_OK && status!=GZ_TRAILING) {
        os_close(ctx, out);
        os_close(ctx, in);
        return code;
    }

    // Failing to give the output the input's permissions or times loses
    // no data, so as in GNU gzip it is only a warning
    if (!os_setmeta(ctx, out, meta)) {
        s8 why = reason(ctx, S("cannot set metadata"));
        code = exit_combine(code, warn(o, outpath, why, scratch));
    }
    // A failed close may mean lost data, and an output that cannot be
    // kept is discarded when closed, so the input must survive either
    b32 kept = os_keep(ctx, out);
    if (!kept || !os_close(ctx, out)) {
        code = report(o, path, outpath, GZ_EWRITE, scratch);
        if (kept) {
            os_remove(ctx, outpath, scratch);
        } else {
            os_close(ctx, out);
        }
        os_close(ctx, in);
        return code;
    }
    os_close(ctx, in);

    // Failing to remove the input loses nothing, so as in GNU gzip it is
    // only a warning
    if (!o->keep && !os_remove(ctx, path, scratch)) {
        s8 why = reason(ctx, S("cannot remove input file"));
        code = exit_combine(code, warn(o, path, why, scratch));
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
        {S8("--no-name"),    'n'},
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
    case 'n':                          return -1;  // nothing to save
    case 'q': o->quiet      = 1;       return -1;
    case 't': o->test       = 1;       return -1;
    case 'h':
        print(scratch, 1, usage_text);
        return EXIT_OK;
    case 'V':
        print(scratch, 1, S("gzip (tugz) " TUGZ_VERSION "\n"));
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
    for (i32 i = 0; i<nfiles && !o.stop; i++) {
        code = exit_combine(code, process_file(&o, files[i], *perm));
    }
    return code;
}
