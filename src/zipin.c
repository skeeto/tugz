// tugz archive reading, shared by the zip and unzip programs
//
// An existing archive's central directory and entries, read through a
// read-ahead window, with the string, environment, and path helpers that
// both programs use. Messages and exit statuses are each program's own.

// Platform interface shared by zip and unzip, beyond src/io.c and
// src/dir.c (platform/zipfs_*.c)

// Returns false if the path does not exist or cannot be examined, which
// os_missing then tells apart. With follow, symbolic links are followed;
// otherwise a link (on Windows, or a junction) is FT_LINK.
static b32  os_stat(os *, s8 path, b32 follow, os_info *, arena scratch);
// Whether the last os_stat failed because nothing is there (no such
// file, nor a directory on the way to it), rather than for a reason,
// such as an I/O error, that leaves what is there unknown.
static b32  os_missing(os *);
// As os_stat, for the file that an open descriptor reads.
static b32  os_fstat(os *, i32 fd, os_info *);
// A positioned read of exactly len bytes, which may not be mixed with
// os_read on the same descriptor. Returns 1, or 0 if the file ends
// first, which is no system error, or -1.
static i32  os_readat(os *, i32 fd, u8 *buf, iz len, i64 off);
// Broken-down local time {year, month, day, hour, minute, second}.
static void os_localtime(os *, i64 t, i32 tm[6]);
// A name in the OEM code page, or else the ANSI one, as UTF-8 (Windows),
// or a null string if that code page cannot decode it.
static s8   os_fromcp(os *, s8 name, b32 oem, arena *perm, arena scratch);

typedef struct {
    s8 *data;
    iz  len;
    iz  cap;
} s8s;

static s8 zjoin(arena *a, s8 const *parts, iz n)
{
    iz len = 0;
    for (iz i = 0; i < n; i++) {
        len += parts[i].len;
    }
    s8 r = {newstr(a, len), 0};
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
    s8 r = {newstr(a, e-p), e-p};
    bytecopy(r.s, p, r.len);
    return r;
}

static b32 zequals(s8 a, s8 b)
{
    return a.len==b.len && (!a.len || !__builtin_memcmp(a.s, b.s, (uz)a.len));
}

// Whether two strings are equal, ignoring ASCII case given ZIP_FOLD.
[[maybe_unused]] static b32 zfoldeq(s8 a, s8 b, i32 fold)
{
    if (!(fold & ZIP_FOLD)) {
        return zequals(a, b);
    }
    b32 eq = a.len == b.len;
    for (iz i = 0; eq && i < a.len; i++) {
        eq = zip_fold(a.s[i], fold) == zip_fold(b.s[i], fold);
    }
    return eq;
}

static b32 zisspace(u8 c)
{
    return c==' ' || (c>='\t' && c<='\r');
}

// Read an entire descriptor into memory, or return a null string.
static s8 read_all(os *ctx, i32 fd, arena *perm)
{
    s8 r  = {0};
    iz cap = 0;
    for (;;) {
        if (r.len==cap && at_tip(perm, r.s, cap)) {
            newstr(perm, cap);  // grow in place
            cap *= 2;
        } else if (r.len == cap) {
            iz ncap = cap ? 2*cap : 1<<12;
            u8 *buf = newstr(perm, ncap);
            bytecopy(buf, r.s, r.len);
            r.s = buf;
            cap = ncap;
        }
        iz n = os_read(ctx, fd, r.s+r.len, cap-r.len);
        if (n < 0) {
            return (s8){0};
        } else if (!n) {
            r.s = r.s ? r.s : (u8 *)"";
            return r;
        }
        r.len += n;
    }
}

// Split a ZIPOPT or ZIP value into arguments as Info-ZIP's envargs does:
// at whitespace, except that a word starting with a double quote runs to
// the next one, and both are dropped. On POSIX, a backslash within it is
// dropped too, and the byte after it kept, even a quote or a backslash.
// Info-ZIP's Windows port keeps backslashes, its path separators.
static s8s env_args(arena *perm, s8 env, b32 windows)
{
    s8s r   = {0};
    u8 *buf = newstr(perm, env.len);  // for quoted words
    for (iz i = 0;;) {
        for (; i<env.len && zisspace(env.s[i]); i++) {}
        if (i == env.len) {
            return r;
        }
        s8 *arg = push(perm, &r);
        if (env.s[i] == '"') {
            *arg = (s8){buf, 0};
            for (i++; i<env.len && env.s[i]!='"'; i++) {
                if (!windows && env.s[i]=='\\' && ++i==env.len) {
                    break;  // escaping nothing
                }
                arg->s[arg->len++] = env.s[i];
            }
            buf += arg->len;
            i += i < env.len;
        } else {
            iz beg = i;
            for (; i<env.len && !zisspace(env.s[i]); i++) {}
            *arg = (s8){env.s+beg, i-beg};
        }
    }
}

// Whether the directories along an entry's relative path are just that,
// none a link (on Windows, nor a junction), through which its name would
// reach beyond the current directory; a link at its end is followed, as
// for any path, unless -y. Those of the path checked before, through its
// last slash, are known to be directories, so that a run of entries in
// one directory examines it once, as are those ending before from, the
// length of a prefix the caller trusts (zip trusts none: 0).
[[maybe_unused]]
static b32 linkless(os *ctx, s8 path, iz from, s8 *checked, arena scratch)
{
    for (iz k = 0; k<path.len && k<checked->len; k++) {
        if (path.s[k] != checked->s[k]) {
            break;
        }
        from = path.s[k]=='/' ? MAX(k+1, from) : from;
    }
    iz last = 0;
    for (iz k = 0; k < path.len; k++) {
        if (path.s[k] != '/') {
            continue;
        }
        os_info info = {0};
        if (k>=from && (!os_stat(ctx, (s8){path.s, k}, 0, &info, scratch) ||
                        info.type!=FT_DIR)) {
            return 0;
        }
        last = k + 1;
    }
    *checked = last>from ? (s8){path.s, last} : *checked;
    return 1;
}

// The existing archive

// Its entries are read through a window, so that one read serves the
// headers and data of many small entries, rather than a read for each.
// An archive already in memory (unzip's spooled standard input) is
// instead one window that never moves (zin_memory).
typedef struct {
    os  *ctx;
    i32  fd;
    i64  limit;  // of read-ahead: the file's size, then where entries end
    u8  *buf;
    iz   cap;
    iz   len;    // bytes in the window
    iz   ahead;  // how far the next fill reads
    i64  pos;    // file offset of buf[0]
    b32  mem;    // the whole archive is in buf
} zin;

enum { ZIN_CAP = 1<<20, ZIN_PAGE = 1<<12 };

// Fill the window with the need bytes at off, at most its size, and as
// many more as the read-ahead takes, within its limit: the file as
// examined, and once the central directory is read, the entries. That
// doubles, from a page up to the window's size, with each fill that
// carries on from the last, as when entries are copied in file order,
// and starts over at a jump elsewhere, so that each entry out of order
// costs a page rather than a megabyte. Returns as os_readat: 1, or 0 if
// the file ends before the need bytes, or -1. If the read-ahead fails, as
// when the file has shrunk, the need bytes are read alone, so that it
// fails only where reading just those would.
static i32 zin_fill(zin *r, i64 off, iz need)
{
    b32 onward = off>=r->pos && off-r->pos<=r->len+r->ahead;
    r->ahead = onward ? MIN(2*r->ahead, r->cap) : ZIN_PAGE;
    i64 left = MAX(r->limit-off, 0);
    iz  n    = MAX(need, (iz)MIN(r->ahead, left));
    i32 got  = os_readat(r->ctx, r->fd, r->buf, n, off);
    if (got<=0 && n>need) {
        n   = need;
        got = os_readat(r->ctx, r->fd, r->buf, n, off);
    }
    r->pos = off;
    r->len = got>0 ? n : 0;
    return got;
}

// Read from an archive held in memory, rather than from a descriptor.
[[maybe_unused]] static void zin_memory(zin *r, s8 data)
{
    r->fd    = -1;
    r->buf   = data.s;
    r->len   = data.len;
    r->cap   = MAX(data.len, ZIN_CAP);
    r->limit = data.len;
    r->mem   = 1;
}

// Point *p at the n bytes at off, n at most the window's size, valid
// until its next use. Returns as os_readat.
static i32 zin_get(zin *r, i64 off, iz n, u8 **p)
{
    if (r->mem) {
        // The memory ends where a file would: early, as for a shrunk one
        b32 in = off>=0 && off<=r->len && n<=r->len-off;
        *p = r->buf + (in ? off : 0);
        return in;
    }
    assert(n <= r->cap);
    b32 hit = off>=r->pos && off-r->pos<=r->len-n;
    i32 got = hit ? 1 : zin_fill(r, off, n);
    *p = r->buf + (off - r->pos);
    return got;
}

// Entries' Unicode names, where those differ, by index: few entries have
// them, so these are listed in order rather than given every entry a slot.
typedef struct {
    iz index;
    s8 name;
} zuname;

typedef struct {
    zuname *data;
    iz      len;
    iz      cap;
} zunames;

typedef struct {
    zin     in;
    i64     beg;     // of the first entry, after any preamble
    zend    end;
    zentry *entries;
    zunames unames;
    i64     noname;  // the entry that ZAR_ENONAME refused
    i64     bad;     // the header that ZAR_EFORMAT refused, or -1
    i64     shift;   // added to entries' offsets (unzip's extra bytes)
} zarchive;

// Results of reading an archive (zar_open, zar_entries, zar_local)
enum {
    ZAR_OK,
    ZAR_EREAD,    // a read failed, as os_readat's -1
    ZAR_EEOF,     // the file ended early, as os_readat's 0: it has shrunk
    ZAR_ENOEND,   // no end of central directory record
    ZAR_EFORMAT,  // zip file structure invalid
    ZAR_EMULTI,   // split or spanned archive
    ZAR_EPREFIX,  // offsets do not account for data before the archive
    ZAR_ENONAME,  // an entry, noname, has a zero-length name
};

// The result of a failed read, os_readat's got.
static i32 zar_failed(i32 got)
{
    return got<0 ? ZAR_EREAD : ZAR_EEOF;
}

// An entry's Unicode name, if it differs from its stored name, else a
// null string.
static s8 entry_unicode(zarchive *ar, iz i)
{
    zunames *u  = &ar->unames;
    iz       lo = 0;
    for (iz hi = u->len; lo < hi;) {
        iz mid = lo + (hi - lo)/2;
        if (u->data[mid].index < i) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo<u->len && u->data[lo].index==i ? u->data[lo].name : (s8){0};
}

// An entry's name in Unicode, by which Info-ZIP also finds the entry for
// a file, when it differs from the stored name: from a Unicode path field
// (written by Info-ZIP's Windows port, WinZip, 7-Zip) that checks out,
// else on Windows, as Info-ZIP's port reads them, from the OEM code page
// for names made on DOS or Windows, and from the ANSI code page, the
// port's own, for others, such as PKZIP for Windows makes, unless they
// are UTF-8. Returns a null string if none. Where a field does not check
// out, *stale is the name by which Info-ZIP warns of it, else null.
static s8 zar_uname(os *ctx, zentry *e, b32 windows, i32 *crccpu, s8 *stale,
                    arena *perm, arena scratch)
{
    *stale = (s8){0};
    if (e->flags & ZIP_FLAG_UTF8) {
        return (s8){0};
    }
    u32 crc = crc32_update(0, e->name.s, e->name.len, crccpu);
    s8  u   = zip_extra_upath(e->cextra, crc);
    if (!u.s || zip_utf8(u)<0) {
        i32 utf8 = zip_utf8(e->name);
        b32 oem  = zip_oem_name(e);
        b32 cp   = windows && (oem ? utf8 : utf8<0);
        u = cp ? os_fromcp(ctx, e->name, oem, perm, scratch) : (s8){0};
    }
    if (zip_extra_upath_stale(e->cextra, crc)) {
        *stale = u.len ? u : e->name;  // Info-ZIP gives "(null)"
    }
    b32 bad = zip_has(u, 0) || zip_has(e->name, 0);
    return !u.len || bad || zequals(u, e->name) ? (s8){0} : u;
}

// Find the end records of the archive open at fd, size bytes as
// examined, unless it is in memory (zin_memory), through a window in
// perm, claimed, as all else is, before any output. The end record's
// comment, once found, is kept in perm, even should the records then
// prove invalid. Returns a ZAR code: for ZAR_EPREFIX, the records are
// kept as they read, for the caller to shift (zar_entries).
static i32 zar_open(zarchive *ar, os *ctx, i32 fd, i64 size, arena *perm)
{
    zin *in = &ar->in;
    ar->bad = -1;
    in->ctx = ctx;
    if (!in->mem) {
        in->fd    = fd;
        in->limit = size;
        in->cap   = ZIN_CAP;
        in->buf   = newbytes(perm, in->cap);  // like all else, before output
        in->ahead = ZIN_PAGE;
    }

    // The end records are among the final bytes, read into the window,
    // which for a small archive then holds its central directory too
    iz  n    = (iz)MIN(size, ZIP_END_LEN + ZIP_MAX16 + ZIP_LOC64_LEN);
    u8 *tail = 0;
    i32 got  = zin_get(in, size-n, n, &tail);
    if (got <= 0) {
        return zar_failed(got);
    }
    i32 r = zip_find_end(tail, n, size, &ar->end);
    if (r != ZIP_ENOEND) {
        ar->end.comment = JOIN(perm, ar->end.comment);  // the window moves
    }
    if (r==ZIP_OK && ar->end.end64>=0) {
        u8 *rec = 0;
        got = zin_get(in, ar->end.end64, ZIP_END64_LEN, &rec);
        if (got <= 0) {
            return zar_failed(got);
        }
        r = zip_parse_end64(rec, &ar->end);
    }
    switch (r) {
    case ZIP_OK:
        return ZAR_OK;
    case ZIP_ENOEND:
        return ZAR_ENOEND;
    case ZIP_EMULTI:
        return ZAR_EMULTI;
    case ZIP_EPREFIX:
        return ZAR_EPREFIX;
    }
    return ZAR_EFORMAT;
}

// Read the central directory that zar_open found, keeping its entries,
// all in perm. Given a shift (ar->shift), the end record's offset of the
// central directory is the caller's, already shifted, and entries'
// offsets are shifted as much. Entries' Unicode names are left to the
// caller (zar_uname). Returns a ZAR code: for ZAR_EFORMAT, the header
// that failed is ar->bad, the count of entries for a directory that does
// not end where it should.
static i32 zar_entries(zarchive *ar, arena *perm, arena scratch)
{
    // The central directory is read through the window, whole if it
    // fits, else a window at a time, and parsed a header at a time,
    // keeping only the names, extra fields (without Zip64), and comments,
    // packed, rather than the whole directory.
    zin *in    = &ar->in;
    i64 count  = ar->end.count;
    i64 off    = ar->end.cdoff;
    i64 cdend  = off + ar->end.cdsize;
    i64 cdoff  = ar->end.cdoff - ar->shift;  // as entries' offsets read
    u8 *h      = 0;
    i32 got    = zin_get(in, off, (iz)MIN(cdend-off, in->cap), &h);
    if (got <= 0) {
        return zar_failed(got);
    }
    in->ahead = in->cap;

    // Memory for the entries is claimed only for a directory that begins
    // with a header, as a file that only ends like an archive (sparse, or
    // damaged) does not, and is then not touched until each is parsed
    b32 head = cdend-off>=ZIP_CENTRAL_LEN && zip_central_varlen(h)>=0;
    if (count > ar->end.cdsize/ZIP_CENTRAL_LEN || (count && !head)) {
        ar->bad = 0;
        return ZAR_EFORMAT;
    } else if ((u64)count > (uz)-1>>1) {
        os_oom(in->ctx);  // larger than the address space (32-bit hosts)
    }
    iz each = sizeof(zentry);
    ar->entries = alloc(perm, (iz)count, each, _Alignof(zentry), 0);
    for (i64 i = 0; i < count; i++) {
        // Its fixed part tells its length, at most 192 KiB
        iz len = (iz)MIN(cdend-off, ZIP_CENTRAL_LEN);
        got = zin_get(in, off, len, &h);
        if (got>0 && len==ZIP_CENTRAL_LEN) {
            iz var = zip_central_varlen(h);
            len += (iz)MIN(MAX(var, 0), cdend-off-len);
            got = zin_get(in, off, len, &h);
        }
        if (got <= 0) {
            return zar_failed(got);
        }
        zentry *e = ar->entries + i;
        len = zip_parse_header(h, len, cdoff, e);
        if (len && !e->name.len) {
            // Refused, as by Info-ZIP, rather than kept for readers that
            // cannot name it
            ar->noname = i;
            return ZAR_ENONAME;
        }
        if (!len) {
            ar->bad = i;
            return ZAR_EFORMAT;
        }
        arena tmp = scratch;
        e->offset += ar->shift;
        e->name    = JOIN(perm, e->name);
        e->cextra  = JOIN(perm, zip_filter_extra(&tmp, e->cextra));
        e->comment = JOIN(perm, e->comment);
        off += len;
    }
    if (off != cdend) {
        ar->bad = count;
        return ZAR_EFORMAT;
    }
    in->limit = ar->end.cdoff;  // nothing past the entries is read again

    // Offsets may account for data before the first entry, such as a
    // self-extractor's stub after zip -A, or a zipapp's #! line, which
    // Info-ZIP keeps. Without entries, it precedes the central directory.
    ar->beg = ar->end.cdoff;
    for (i64 i = 0; i < ar->end.count; i++) {
        ar->beg = MIN(ar->beg, ar->entries[i].offset);
    }
    return ZAR_OK;
}

// Read the central directory of the archive open at fd, size bytes as
// examined, through a window in perm, keeping its end records and its
// entries, all in perm, as all else is claimed, before any output. Its
// entries' Unicode names are left to the caller (zar_uname). Returns a
// ZAR code.
[[maybe_unused]]
static i32 zar_read(zarchive *ar, os *ctx, i32 fd, i64 size, arena *perm,
                    arena scratch)
{
    i32 r = zar_open(ar, ctx, fd, size, perm);
    return r ? r : zar_entries(ar, perm, scratch);
}

// An entry's name for messages: in Unicode, if it has that too.
static s8 shown_name(zarchive *ar, zentry *e)
{
    s8 u = entry_unicode(ar, e - ar->entries);
    return u.s ? u : e->name;
}

// An entry's local header, as zar_local finds it.
typedef struct {
    u8  fixed[ZIP_LOCAL_LEN];
    s8  name;   // in the window, valid until its next use
    s8  extra;  // likewise
    i64 data;   // offset of the entry's data
} zlocal;

// Find an entry's local header and its data, which must lie, at the
// entry's compressed size, before the central directory. Returns a ZAR
// code: ZAR_EFORMAT for data out of bounds.
static i32 zar_local(zarchive *ar, zentry *e, zlocal *l)
{
    zin *r     = &ar->in;
    u8  *fixed = 0;
    i32  got   = zin_get(r, e->offset, ZIP_LOCAL_LEN, &fixed);
    if (got <= 0) {
        return zar_failed(got);
    }
    bytecopy(l->fixed, fixed, ZIP_LOCAL_LEN);  // before the window moves
    iz  varlen = zip_local_varlen(l->fixed);
    iz  nlen   = get16(l->fixed+26);
    i64 data   = e->offset + ZIP_LOCAL_LEN + varlen;
    if (varlen<0 || data>ar->end.cdoff || e->csize>ar->end.cdoff-data) {
        return ZAR_EFORMAT;
    }
    u8 *var = 0;
    got = zin_get(r, e->offset+ZIP_LOCAL_LEN, varlen, &var);
    if (got <= 0) {
        return zar_failed(got);
    }
    l->name  = (s8){var, nlen};
    l->extra = (s8){var+nlen, varlen-nlen};
    l->data  = data;
    return ZAR_OK;
}
