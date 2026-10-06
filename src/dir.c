// tugz directories: listings, and wildcards expanded in paths
//
// zip lists directories to recurse into them, and on Windows, whose
// shells leave wildcards in arguments to programs, expands those here,
// as Info-ZIP's Windows port does, matching names by src/wild.c. The
// platform layer provides os_listdir and os_upcase.

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

typedef struct {
    os_dirent *data;
    iz         len;
    iz         cap;
} os_dirents;

// Entries within a directory, excluding . and .., in any order. Unless
// all, hidden and system entries (Windows) are left out, judged by the
// entry itself rather than a link's target. Returns null on error. The
// listing and any temporaries come from one arena.
static os_dirent *os_listdir(os *, s8 path, b32 all, iz *count, arena *);
// A name in upper case as the file system compares names ignoring case
// (Windows), beyond ASCII too, or the name itself. It and any temporaries
// come from one arena.
static s8   os_upcase(os *, s8 name, arena *);

// Append a zeroed slot to a dynamic array, returning a pointer to it.
#define push(a, s) \
    ((s)->len==(s)->cap ? grow(a, (void **)&(s)->data, &(s)->cap, \
                               sizeof(*(s)->data)) : (void)0, \
     (s)->data + (s)->len++)

// Whether data of len bytes ends where the arena's next allocation
// begins, so that it can grow in place.
static b32 at_tip(arena *a, void *data, iz len)
{
    return !a->down && data && (byte *)data+len==a->beg;
}

// Double an array's capacity: in place if it is the last allocation in
// an arena that grows up (perm), else by moving it, which leaves the
// old copy as garbage until the arena is freed (in perm, never). An
// empty one, which doubling would leave empty, moves to a fresh 16.
static void grow(arena *a, void **data, iz *cap, iz size)
{
    if (*cap && at_tip(a, *data, *cap*size)) {
        alloc(a, *cap, size, 1, 1);
        *cap *= 2;
        return;
    }
    iz   n = *cap ? 2 * *cap : 16;
    void *r = alloc(a, n, size, 16, 1);
    bytecopy(r, *data, *cap*size);
    *data = r;
    *cap  = n;
}

static i32 zcompare(s8 a, s8 b)
{
    iz n = MIN(a.len, b.len);
    i32 r = n ? __builtin_memcmp(a.s, b.s, (uz)n) : 0;
    return r ? r : a.len<b.len ? -1 : a.len>b.len;
}

// A directory listing sorted by name bytes, as pointers to its entries,
// which are large to move: a stable bottom-up merge sort.
static os_dirent **zsort(os_dirent *list, iz n, arena *a)
{
    os_dirent **v = new(a, n, os_dirent *);
    for (iz i = 0; i < n; i++) {
        v[i] = list + i;
    }
    arena       t   = *a;
    os_dirent **tmp = new(&t, n, os_dirent *);
    for (iz w = 1; w < n; w *= 2) {
        for (iz lo = 0; lo < n; lo += 2*w) {
            iz mid = MIN(lo+w, n);
            iz hi  = MIN(lo+2*w, n);
            iz i = lo, j = mid, k = lo;
            while (i<mid && j<hi) {
                b32 lt = zcompare(v[j]->name, v[i]->name) < 0;
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
    return v;
}

// The path with its bytes from beg to end replaced by name.
static s8 replace_part(arena *a, s8 path, iz beg, iz end, s8 name)
{
    s8 r  = {0};
    r.len = path.len - (end-beg) + name.len;
    r.s   = newstr(a, r.len);
    bytecopy(r.s, path.s, beg);
    bytecopy(r.s+beg, name.s, name.len);
    bytecopy(r.s+beg+name.len, path.s+end, path.len-end);
    return r;
}

static b32 nonascii(s8 s)
{
    for (iz i = 0; i < s.len; i++) {
        if (s.s[i] >= 0x80) {
            return 1;
        }
    }
    return 0;
}

// A match for expand_wild: its path, and its directory entry, if that
// describes it, else null. Returns whether the match counts.
typedef b32 wild_found(void *data, s8 path, os_dirent *entry, arena scratch);

// Expand wildcards in a path's components against the file system, as
// Windows shells do not, matching as Info-ZIP does there: ignoring case,
// by characters, with DOS rules, and if nowild (zip's -nw) only ?. Hidden
// and system entries are left out unless all. Each match goes to found,
// in name order. Returns the number of matches that found counted.
static iz expand_wild(os *ctx, s8 path, b32 all, b32 nowild,
                      wild_found *found, void *data, arena scratch)
{
    // Find the first component with a wildcard. A drive ends a component:
    // "C:*.c" lists the drive's current directory, "C:".
    u8 drive = path.len>=2 && path.s[1]==':' ? (u8)(path.s[0] | 0x20) : 0;
    iz beg = 0;
    iz end = 0;
    for (iz i = drive>='a' && drive<='z' ? 2 : 0;; i = end + 1) {
        beg = i;
        for (end = i; end<path.len && path.s[end]!='/' &&
                      path.s[end]!='\\'; end++) {}
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
    os_dirent *list = os_listdir(ctx, dir, all, &n, &scratch);
    if (!list) {
        return 0;
    }
    os_dirent **kids = zsort(list, n, &scratch);

    // Beyond ASCII, which ZIP_FOLD folds, case is ignored by comparing in
    // upper case, as the file system and Info-ZIP's port (by towupper)
    // compare names, where the pattern or the name needs it
    i32 flags = ZIP_FOLD | ZIP_DOS | ZIP_UTF8 | (nowild ? ZIP_NOWILD : 0);
    b32 wide  = nonascii(pat);
    s8  upat  = wide ? os_upcase(ctx, pat, &scratch) : pat;
    iz  count = 0;
    for (iz i = 0; i < n; i++) {
        arena tmp = scratch;
        s8    kid = kids[i]->name;
        b32   up  = wide || nonascii(kid);
        if (!zip_match(up ? upat : pat,
                       up ? os_upcase(ctx, kid, &tmp) : kid, flags)) {
            continue;
        }
        arena iter = scratch;  // forgets each match's strings
        s8    cand = replace_part(&iter, path, beg, end, kid);
        if (zip_haswild(rest, 0)) {
            count += expand_wild(ctx, cand, all, nowild, found, data, iter);
            continue;
        }
        // The listing describes the match only if nothing follows it, and
        // a bare name, like an argument, may instead name a device (NUL)
        os_dirent *entry = beg && !rest.len ? kids[i] : 0;
        if (found(data, cand, entry, iter)) {
            count++;
        }
    }
    return count;
}
