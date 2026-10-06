// tugz wildcards: matching names against patterns as Info-ZIP does
//
// For zip's patterns (-x, -i, and paths matched against entries) on
// every platform, and on Windows for wildcard arguments, which zip and
// gzip expand on disk (src/dir.c), as Windows shells do not.

enum {
    ZIP_SETS   = 1 << 0,  // [sets] and backslash escapes, as on Unix
    ZIP_FOLD   = 1 << 1,  // ASCII case-insensitive, for Windows file names
    ZIP_NOWILD = 1 << 2,  // only ? is a wildcard, as with Info-ZIP's -nw
    ZIP_DOS    = 1 << 3,  // a name without a period ends in one (Windows)
    ZIP_UTF8   = 1 << 4,  // ? is a UTF-8 character, not a byte (Windows)
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

// Length of the character at byte i of a name: of a UTF-8 sequence (or
// WTF-8, whose lone surrogates are characters on Windows), else 1.
static iz zip_charlen(s8 s, iz i)
{
    u8 c = zip_at(s, i);
    iz n = c>=0xc2 && c<0xe0 ? 2 : c>=0xe0 && c<0xf0 ? 3 :
           c>=0xf0 && c<0xf5 ? 4 : 1;
    if (n > s.len-i) {
        return 1;
    }
    for (iz k = 1; k < n; k++) {
        if ((s.s[i+k] & 0xc0) != 0x80) {
            return 1;
        }
    }
    return n;
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
// With ZIP_UTF8, ? matches a UTF-8 character, and * a run of them, as
// Info-ZIP's Windows port matches characters of its code page (or wide
// ones), while on Unix it matches bytes. Sets, unused there, match bytes.
//
// Info-ZIP's quirks are kept: an unclosed set or a trailing backslash
// matches nothing; a trailing ** needs at least one more byte; and once
// a * is followed by no more wildcards, the rest of the pattern is
// compared with the end of the name byte for byte, escapes included.
static b32 zip_match(s8 pat, s8 s, i32 flags)
{
    b32 wild = !(flags & ZIP_NOWILD);
    b32 sets = wild && (flags & ZIP_SETS);
    b32 utf8 = flags & ZIP_UTF8;
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
                i += utf8 ? zip_charlen(s, i) : 1;
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
        p    = star;
        mark += utf8 ? zip_charlen(s, mark) : 1;
        i    = mark;
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
