// Unit tests for the portable ZIP format layer (src/zip.c, src/wild.c)
// On success prints "all zip tests pass". A failure traps.
// $ cc -g3 -fsanitize=address,undefined -o tests-zip test/ziptests.c
#include "../src/base.c"
#include "../src/zip.c"
#include "../src/wild.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
            fflush(stderr); \
            __builtin_trap(); \
        } \
    } while (0)

struct os { int unused; };

static void os_oom(os *ctx)
{
    (void)ctx;
    fprintf(stderr, "out of memory\n");
    __builtin_trap();
}

static void os_extend(os *ctx, arena *a, iz need)
{
    (void)a;
    (void)need;
    os_oom(ctx);
}

static s8 str(char const *z)
{
    return (s8){(u8 *)z, (iz)strlen(z)};
}

static b32 equals(s8 a, char const *z)
{
    s8 b = str(z);
    return a.len==b.len && !memcmp(a.s, b.s, (uz)a.len);
}

// A copy of exactly len bytes, as parsers get from the zip program,
// rather than a view into a larger buffer, whose following bytes would
// hide reads past its end.
static u8 *exact(u8 const *p, iz len)
{
    u8 *r = malloc((uz)len);
    TEST(r || !len);
    return bytecopy(r, p, len);
}

static void test_dostime(void)
{
    i32 tm[6];
    zip_gmtime(0, tm);
    TEST(tm[0]==1970 && tm[1]==1 && tm[2]==1 && !tm[3] && !tm[4] && !tm[5]);
    zip_gmtime(1700000000, tm);  // 2023-11-14 22:13:20
    TEST(tm[0]==2023 && tm[1]==11 && tm[2]==14);
    TEST(tm[3]==22 && tm[4]==13 && tm[5]==20);
    zip_gmtime(951782400, tm);   // 2000-02-29, a leap day
    TEST(tm[0]==2000 && tm[1]==2 && tm[2]==29);
    zip_gmtime(-1, tm);          // 1969-12-31 23:59:59
    TEST(tm[0]==1969 && tm[1]==12 && tm[2]==31 && tm[5]==59);

    // Every day from 1970 through 2200 matches a straightforward count
    i32 y = 1970, m = 1, d = 1;
    for (i64 day = 0; day < 84000; day++) {
        zip_gmtime(day*86400 + 3661, tm);
        TEST(tm[0]==y && tm[1]==m && tm[2]==d);
        TEST(tm[3]==1 && tm[4]==1 && tm[5]==1);
        b32 leap = (y%4==0 && y%100!=0) || y%400==0;
        i32 mdays[] = {31, 28+leap, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (++d > mdays[m-1]) {
            d = 1;
            if (++m > 12) {
                m = 1;
                y++;
            }
        }
    }

    i32 t[6] = {2023, 11, 14, 22, 13, 20};
    u32 dt = zip_dostime(t);
    TEST(dt>>16 == (43u<<9 | 11<<5 | 14));
    TEST((dt & 0xffff) == (22u<<11 | 13<<5 | 10));
    i32 old[6] = {1975, 6, 1, 12, 0, 0};
    TEST(zip_dostime(old) == (1u<<5 | 1) << 16);       // 1980-01-01
    i32 far[6] = {2200, 1, 1, 0, 0, 0};
    TEST(zip_dostime(far)>>16 == (127u<<9 | 12<<5 | 31));

    // Years beyond i32 saturate rather than wrap (to 2000 here)
    zip_gmtime(-135536075854733232, tm);
    TEST(tm[0] == -0x7fffffff-1);
    TEST(zip_dostime(tm) == (1u<<5 | 1) << 16);
    zip_gmtime(135536077748150352, tm);
    TEST(tm[0] == 0x7fffffff);
    TEST(zip_dostime(tm)>>16 == (127u<<9 | 12<<5 | 31));
    zip_gmtime(-0x7fffffffffffffff-1, tm);
    TEST(tm[0] == -0x7fffffff-1);
    zip_gmtime(0x7fffffffffffffff, tm);
    TEST(tm[0]==0x7fffffff && tm[3]==15 && tm[4]==30 && tm[5]==7);
}

static void test_utf8(void)
{
    TEST(zip_utf8(str("plain/ascii.txt")) == 0);
    TEST(zip_utf8(str("")) == 0);
    TEST(zip_utf8(str("caf\xc3\xa9")) == 1);
    TEST(zip_utf8(str("\xe2\x82\xac")) == 1);          // U+20AC
    TEST(zip_utf8(str("\xf0\x9f\x98\x80")) == 1);      // U+1F600
    TEST(zip_utf8(str("\xf4\x8f\xbf\xbf")) == 1);      // U+10FFFF
    TEST(zip_utf8(str("\xff")) == -1);
    TEST(zip_utf8(str("\xc3")) == -1);                 // truncated
    TEST(zip_utf8(str("\xc0\xaf")) == -1);             // overlong
    TEST(zip_utf8(str("\xe0\x80\xaf")) == -1);         // overlong
    TEST(zip_utf8(str("\xed\xa0\x80")) == -1);         // surrogate (WTF-8)
    TEST(zip_utf8(str("\xf4\x90\x80\x80")) == -1);     // beyond U+10FFFF
    TEST(zip_utf8(str("a\x80")) == -1);                // stray continuation
}

static void test_match(void)
{
    i32 u = ZIP_SETS;
    TEST( zip_match(str("*.c"), str("a/b/x.c"), u));   // * crosses /
    TEST(!zip_match(str("*.c"), str("x.h"), u));
    TEST( zip_match(str("*"), str(""), u));
    TEST( zip_match(str("a?c"), str("abc"), u));
    TEST(!zip_match(str("a?c"), str("ac"), u));
    TEST( zip_match(str("d/sub/*"), str("d/sub/"), u));
    TEST( zip_match(str("*a*b*c*"), str("xxaxxbxxcxx"), u));
    TEST(!zip_match(str("*a*b*c*"), str("xxaxxcxxbxx"), u));
    TEST( zip_match(str("[ab].txt"), str("a.txt"), u));
    TEST(!zip_match(str("[ab].txt"), str("c.txt"), u));
    TEST( zip_match(str("[!ab].txt"), str("c.txt"), u));
    TEST( zip_match(str("[^ab].txt"), str("c.txt"), u));
    TEST( zip_match(str("[a-c]x"), str("bx"), u));
    TEST(!zip_match(str("[a-c]x"), str("dx"), u));
    TEST( zip_match(str("\\*"), str("*"), u));
    TEST(!zip_match(str("\\*"), str("x"), u));
    TEST(!zip_match(str("A.TXT"), str("a.txt"), u));

    // Sets as Info-ZIP's recmatch reads them: the first unescaped ]
    // closes, backslashes escape, and a byte before a - only starts a
    // range, so dangling and chained range starts match nothing
    TEST(!zip_match(str("[]]"), str("]"), u));         // empty set, then ]
    TEST( zip_match(str("[\\]]"), str("]"), u));
    TEST( zip_match(str("[!]"), str("x"), u));         // negated empty set
    TEST(!zip_match(str("[!]"), str(""), u));
    TEST(!zip_match(str("[a-]"), str("a"), u));
    TEST(!zip_match(str("[a-]"), str("-"), u));
    TEST( zip_match(str("[ab-]"), str("a"), u));
    TEST(!zip_match(str("[ab-]"), str("b"), u));
    TEST(!zip_match(str("[a-b-c]"), str("a"), u));
    TEST( zip_match(str("[a-b-c]"), str("b"), u));
    TEST( zip_match(str("[--0]"), str("."), u));       // leading - literal
    TEST( zip_match(str("[\\-x]"), str("-"), u));
    TEST(!zip_match(str("[\\-x]"), str("a"), u));
    TEST(!zip_match(str("[a\\]"), str("a"), u));       // unclosed
    TEST( zip_match(str("[\xff]"), str("q"), u));      // 0xff wraps around
    TEST(!zip_match(str("[!\xff]"), str("q"), u));

    // Malformed patterns match nothing
    TEST(!zip_match(str("a["), str("a["), u));
    TEST(!zip_match(str("*["), str("a["), u));
    TEST(!zip_match(str("x\\"), str("x\\"), u));
    TEST(!zip_match(str("\\"), str("\\"), u));

    // A trailing run of *s needs a byte, unlike a single *
    TEST( zip_match(str("d/*"), str("d/"), u));
    TEST(!zip_match(str("d/**"), str("d/"), u));
    TEST( zip_match(str("d/**"), str("d/x"), u));
    TEST(!zip_match(str("**"), str(""), u));
    TEST( zip_match(str("a**b"), str("ab"), u));

    // After a * with no wildcards following, the rest is compared
    // literally, escapes included
    TEST(!zip_match(str("*\\*"), str("a*"), u));
    TEST( zip_match(str("*\\*"), str("a\\*"), u));
    TEST( zip_match(str("*\\"), str("x\\"), u));
    TEST( zip_match(str("*[a]\\*"), str("a*"), u));    // not after a set

    // -nw: only ? is a wildcard
    i32 nw = ZIP_SETS | ZIP_NOWILD;
    TEST( zip_match(str("a?"), str("ab"), nw));
    TEST(!zip_match(str("a*"), str("ab"), nw));
    TEST( zip_match(str("a*"), str("a*"), nw));
    TEST( zip_match(str("[a]\\"), str("[a]\\"), nw));

    // Windows: brackets and backslashes literal; folding when asked
    TEST( zip_match(str("[ab].txt"), str("[ab].txt"), 0));
    TEST(!zip_match(str("[ab].txt"), str("a.txt"), 0));
    TEST( zip_match(str("A.*"), str("a.txt"), ZIP_FOLD));
    TEST(!zip_match(str("A.*"), str("a.txt"), 0));
    TEST( zip_match(str("*.TXT"), str("d/a.txt"), ZIP_FOLD));
    TEST( zip_match(str("a["), str("a["), 0));

    // DOS rules: a name without a period has one at its end
    i32 dos = ZIP_FOLD | ZIP_DOS;
    TEST( zip_match(str("*.*"), str("Makefile"), dos));
    TEST( zip_match(str("README.*"), str("readme"), dos));
    TEST( zip_match(str("*."), str("LICENSE"), dos));
    TEST(!zip_match(str("*."), str("a.txt"), dos));
    TEST( zip_match(str("*.*"), str("v1.0/Makefile"), dos));
    TEST(!zip_match(str("*.*"), str("Makefile"), ZIP_FOLD));

    // On Windows, ? is a UTF-8 character, as Info-ZIP's port matches
    // characters, and * moves by them too; on Unix it matches bytes
    i32 w = dos | ZIP_UTF8;
    TEST( zip_match(str("?.txt"), str("\xc3\xa9.txt"), w));          // U+E9
    TEST(!zip_match(str("??.txt"), str("\xc3\xa9.txt"), w));
    TEST(!zip_match(str("?.txt"), str("\xc3\xa9.txt"), dos));
    TEST( zip_match(str("??.txt"), str("\xc3\xa9.txt"), u));
    TEST( zip_match(str("?.txt"), str("\xe2\x82\xac.txt"), w));      // U+20AC
    TEST( zip_match(str("a?"), str("a\xf0\x9f\x98\x80"), w));        // U+1F600
    TEST( zip_match(str("?"), str("\xed\xa0\x80"), w));              // WTF-8
    TEST( zip_match(str("*?"), str("\xe2\x82\xac"), w));
    TEST(!zip_match(str("*??"), str("\xe2\x82\xac"), w));
    TEST( zip_match(str("*??"), str("\xe2\x82\xac"), u));
    TEST( zip_match(str("??"), str("\xc3\x41"), w));       // not a sequence
    TEST( zip_match(str("a??"), str("a\xe2\x82"), w));     // cut short
    TEST(!zip_match(str("a?"), str("a\xe2\x82"), w));
    TEST( zip_match(str("?"), str("\xc3\xa9"), ZIP_NOWILD|ZIP_UTF8));
    TEST( zip_match(str("?."), str("\xc3\xa9"), w));       // implicit period
    TEST(!zip_match(str("*.?"), str("\xc3\xa9"), w));

    TEST( zip_haswild(str("a*"), 0));
    TEST( zip_haswild(str("a?"), 0));
    TEST(!zip_haswild(str("a[b]"), 0));
    TEST( zip_haswild(str("a[b]"), ZIP_SETS));
}

static void test_names(arena a)
{
    TEST(equals(zip_name(&a, str("./a/b"), 0), "a/b"));
    TEST(equals(zip_name(&a, str("././a"), 0), "a"));
    TEST(equals(zip_name(&a, str("/abs/x"), 0), "abs/x"));
    TEST(equals(zip_name(&a, str("a//x//y"), 0), "a/x/y"));
    TEST(equals(zip_name(&a, str("///x//y"), 0), "x/y"));
    TEST(equals(zip_name(&a, str("../x"), 0), "../x"));
    TEST(equals(zip_name(&a, str("a/./b"), 0), "a/./b"));
    TEST(equals(zip_name(&a, str("."), 0), ""));
    TEST(equals(zip_name(&a, str("dir/"), 0), "dir/"));
    TEST(equals(zip_name(&a, str(".hidden"), 0), ".hidden"));
    TEST(equals(zip_name(&a, str("a\\b"), 0), "a\\b"));
    TEST(equals(zip_name(&a, str("a\\b"), 1), "a/b"));
    TEST(equals(zip_name(&a, str("C:\\x\\y"), 1), "x/y"));
    TEST(equals(zip_name(&a, str("C:x"), 1), "x"));
    TEST(equals(zip_name(&a, str("z:/x"), 1), "x"));
    TEST(equals(zip_name(&a, str(".\\x"), 1), "x"));
    TEST(equals(zip_name(&a, str("C:x"), 0), "C:x"));

    // Only a letter is a drive, as Windows has it: "1:x" opens stream x
    // of file 1, so the name keeps it
    TEST(equals(zip_name(&a, str("1:x"), 1), "1:x"));
    TEST(equals(zip_name(&a, str("@:x"), 1), "@:x"));
    TEST(equals(zip_name(&a, str("[:x"), 1), "[:x"));
    TEST(equals(zip_name(&a, str("{:x"), 1), "{:x"));
    TEST(equals(zip_name(&a, str(":"), 1), ":"));

    // UNC and device prefixes, dropped as Info-ZIP's ex2in does
    TEST(equals(zip_name(&a, str("\\\\server\\share\\f"), 1), "f"));
    TEST(equals(zip_name(&a, str("//server/share/d/f"), 1), "d/f"));
    TEST(equals(zip_name(&a, str("\\\\server\\share\\"), 1), ""));
    TEST(equals(zip_name(&a, str("\\\\server\\share"), 1), "server/share"));
    TEST(equals(zip_name(&a, str("\\\\server"), 1), "server"));
    TEST(equals(zip_name(&a, str("\\\\\\x"), 1), "x"));
    TEST(equals(zip_name(&a, str("\\\\?\\C:\\rel\\x.txt"), 1), "rel/x.txt"));
    TEST(equals(zip_name(&a, str("//?/C:/x"), 1), "x"));
    TEST(equals(zip_name(&a, str("\\\\.\\C:\\d\\f.txt"), 1), "d/f.txt"));
    TEST(equals(zip_name(&a, str("\\\\?\\UNC\\srv\\shr\\x"), 1), "x"));
    TEST(equals(zip_name(&a, str("\\\\?\\unc\\srv\\shr\\.\\x"), 1), "x"));
    TEST(equals(zip_name(&a, str("\\\\?\\UNC\\srv\\shr"), 1), "srv/shr"));
    TEST(equals(zip_name(&a, str("\\\\?\\Volume{1}\\d\\x"), 1), "d/x"));
    TEST(equals(zip_name(&a, str("\\\\?x\\UNC\\srv\\shr\\x"), 1),
                "srv/shr/x"));

    // Info-ZIP's Unix ex2in drops "//host/share/" too, but knows only
    // slashes and no device paths
    TEST(equals(zip_name(&a, str("//server/share/d/f"), 0), "d/f"));
    TEST(equals(zip_name(&a, str("//x//y"), 0), "y"));
    TEST(equals(zip_name(&a, str("//server/share"), 0), "server/share"));
    TEST(equals(zip_name(&a, str("//a\\b/c/d"), 0), "d"));
    TEST(equals(zip_name(&a, str("//?/UNC/srv/shr/x"), 0), "srv/shr/x"));
}

static void test_percent(void)
{
    TEST(zip_percent(24, 11) == 54);
    TEST(zip_percent(100000, 114) == 100);
    TEST(zip_percent(1, 1) == 0);
    TEST(zip_percent(0, 0) == 0);
    TEST(zip_percent(10, 20) == 0);
    TEST(zip_percent((i64)1<<40, (i64)1<<39) == 50);
    // Exact over 16 MiB too, as Info-ZIP computes it (not by its disabled
    // reduction, which gives 100, 67, 91, and 98)
    TEST(zip_percent(104857600, 524289) == 99);
    TEST(zip_percent(18918855, 6337836) == 66);
    TEST(zip_percent(491555167, 41782159) == 92);
    TEST(zip_percent(20000000, 299977) == 99);
}

// An archive of three small entries and a comment, written by the header
// encoders and read back by the end record and central directory
// parsers, which must also reject truncations, a prefix, and splits.
static void test_roundtrip_headers(arena a)
{
    // Small archive: three entries, an archive comment
    zentry e[3] = {0};
    char const *names[] = {"dir/", "dir/file.txt", "caf\xc3\xa9"};
    u8 data[] = "hello";
    u8 *buf = new(&a, 4096, u8);
    u8 *p = buf;
    for (i32 i = 0; i < 3; i++) {
        e[i].name    = str(names[i]);
        e[i].made    = 0x031e;
        e[i].method  = i==1 ? ZIP_DEFLATE : ZIP_STORE;
        e[i].flags   = i==2 ? ZIP_FLAG_UTF8 : 0;
        e[i].dostime = 0x57654321u + (u32)i;
        e[i].crc     = 0x12345678u * (u32)i;
        e[i].csize   = i ? 5 : 0;
        e[i].usize   = i==1 ? 9 : e[i].csize;
        e[i].extattr = 0x81a40000u;
        e[i].offset  = p - buf;
        if (i == 1) {
            e[i].lextra = S("UT\x05\x00\x03\x01\x02\x03\x04");
            e[i].cextra = e[i].lextra;
        }
        u8 *q = zip_local(p, e+i);
        TEST(q-p == zip_local_len(e+i));
        p = q;
        bytecopy(p, data, (iz)e[i].csize);
        p += e[i].csize;
    }
    i64 cdoff = p - buf;
    for (i32 i = 0; i < 3; i++) {
        u8 *q = zip_central(p, e+i);
        TEST(q-p == zip_central_len(e+i));
        p = q;
    }
    i64 cdsize = p - buf - cdoff;
    s8 comment = str("archive comment");
    u8 *q = zip_end(p, 3, cdsize, cdoff, comment, 0x031e);
    TEST(q-p == zip_end_len(3, cdsize, cdoff, comment));
    TEST(q-p == ZIP_END_LEN + comment.len);
    p = q;
    iz total = p - buf;

    zend end = {0};
    TEST(zip_find_end(buf, total, total, &end) == ZIP_OK);
    TEST(end.count==3 && end.cdoff==cdoff && end.cdsize==cdsize);
    TEST(end.end64 == -1);
    TEST(end.comment.len==comment.len);
    TEST(!memcmp(end.comment.s, comment.s, (uz)comment.len));

    // Only the tail need be supplied
    iz tail = ZIP_END_LEN + comment.len + 3;
    TEST(zip_find_end(buf+total-tail, tail, total, &end) == ZIP_OK);
    TEST(end.endpos == cdoff+cdsize);

    zentry *got = zip_parse_central(buf+cdoff, (iz)cdsize, 3, cdoff, &a);
    TEST(got);
    for (i32 i = 0; i < 3; i++) {
        TEST(got[i].name.len == e[i].name.len);
        TEST(!memcmp(got[i].name.s, e[i].name.s, (uz)e[i].name.len));
        TEST(got[i].method==e[i].method && got[i].flags==e[i].flags);
        TEST(got[i].dostime==e[i].dostime && got[i].crc==e[i].crc);
        TEST(got[i].csize==e[i].csize && got[i].usize==e[i].usize);
        TEST(got[i].offset==e[i].offset && got[i].extattr==e[i].extattr);
        TEST(got[i].made==0x031e);
        TEST(got[i].needed == (i==1 ? 20 : 10));
        TEST(got[i].cextra.len == e[i].cextra.len);

        iz v = zip_local_varlen(buf + got[i].offset);
        TEST(v == e[i].name.len + e[i].lextra.len);
    }

    // Corruptions are rejected, never read out of bounds: each in an
    // allocation of its exact size, so that AddressSanitizer would see.
    // Only whole headers, as many as expected, make a central directory.
    iz ends[4] = {0};  // of the first i headers
    for (i32 i = 0; i < 3; i++) {
        ends[i+1] = ends[i] + zip_central_len(e+i);
    }
    for (iz n = 0; n <= (iz)cdsize; n++) {
        u8 *cd = exact(buf+cdoff, n);
        for (i32 count = 0; count <= 4; count++) {
            zentry *r = zip_parse_central(cd, n, count, cdoff, &a);
            TEST(!r == (count==4 || n!=ends[count]));
        }
        TEST(!zip_parse_central(cd, n, 3, 20, &a));
        free(cd);
    }
    for (iz n = 0; n < total; n++) {
        u8 *t   = exact(buf+total-n, n);  // a tail, which must hold it all
        i32 got = zip_find_end(t, n, total, &end);
        TEST(got == (n<ZIP_END_LEN+comment.len ? ZIP_ENOEND : ZIP_OK));
        free(t);
        t = exact(buf, n);  // the archive cut short
        TEST(zip_find_end(t, n, n, &end) == ZIP_ENOEND);
        free(t);
    }
    TEST(zip_local_varlen(buf+1) == -1);

    // Data prepended to the archive (as by a self-extractor) is detected
    u8 *pre = new(&a, total+100, u8);
    bytefill(pre, 'x', 100);
    bytecopy(pre+100, buf, total);
    TEST(zip_find_end(pre, total+100, total+100, &end) == ZIP_EPREFIX);

    // Split archives are refused
    u8 *split = new(&a, total, u8);
    bytecopy(split, buf, total);
    put16(split + cdoff + cdsize + 4, 1);
    TEST(zip_find_end(split, total, total, &end) == ZIP_EMULTI);
}

static void test_zip64(arena a)
{
    // A central header with every field overflowing
    zentry e = {0};
    e.name   = str("big");
    e.made   = 0x031e;
    e.method = ZIP_DEFLATE;
    e.usize  = (i64)5 << 30;
    e.csize  = ((i64)4 << 30) + 7;
    e.offset = ((i64)6 << 30) + 3;
    e.zip64  = 1;
    TEST(zip_needed(&e) == 45);
    TEST(zip_central64_len(&e) == 28);

    u8 local[256];
    TEST(zip_local(local, &e) - local == zip_local_len(&e));
    TEST(get32(local+18)==0xffffffff && get32(local+22)==0xffffffff);
    TEST(get16(local+28) == 20);
    TEST(get64(local+30+3+4) == (u64)e.usize);
    TEST(get64(local+30+3+12) == (u64)e.csize);

    u8 *buf = new(&a, 1024, u8);
    u8 *p   = zip_central(buf, &e);
    i64 cdsize = p - buf;
    i64 cdoff  = ((i64)11 << 30);  // past the entry's data
    i64 count  = 1;
    u8 *q = zip_end(p, count, cdsize, cdoff, (s8){0}, 0x031e);
    TEST(q-p == ZIP_END64_LEN + ZIP_LOC64_LEN + ZIP_END_LEN);
    TEST(zip_end_len(count, cdsize, cdoff, (s8){0}) == q-p);

    // As if the central directory sat at cdoff in a huge file
    i64 size = cdoff + (q - buf);
    zend end = {0};
    iz tail = q - p;
    TEST(zip_find_end(p, tail, size, &end) == ZIP_OK);
    TEST(end.end64 == cdoff + cdsize);
    TEST(get32(q-ZIP_END_LEN+16) == 0xffffffff);  // offset overflowed
    TEST(get16(q-ZIP_END_LEN+10) == 1);           // count did not
    TEST(zip_parse_end64(p, &end) == ZIP_OK);
    TEST(end.count==count && end.cdsize==cdsize && end.cdoff==cdoff);

    zentry *got = zip_parse_central(buf, (iz)cdsize, 1, cdoff, &a);
    TEST(got);
    TEST(got->usize==e.usize && got->csize==e.csize && got->offset==e.offset);
    TEST(!got->cextra.len);  // the Zip64 extra is consumed

    // Both sizes always appear, saturated, even with a big offset alone,
    // which UnZip 6.0 needs after a size of exactly 0xffffffff
    zentry f = {0};
    f.name   = str("f");
    f.usize  = 10;
    f.csize  = 10;
    f.offset = (i64)5 << 30;
    TEST(zip_central64_len(&f) == 28);
    TEST(zip_needed(&f) == 45);
    p = zip_central(buf, &f);
    TEST(get32(buf+20)==0xffffffff && get32(buf+24)==0xffffffff);
    TEST(get64(buf+ZIP_CENTRAL_LEN+1+4) == 10);
    got = zip_parse_central(buf, p-buf, 1, (i64)6 << 30, &a);
    TEST(got && got->offset==f.offset && got->usize==10 && got->csize==10);

    // A size of exactly 0xffffffff needs Zip64, and the offset no field
    zentry ff = {0};
    ff.name   = str("ff");
    ff.method = ZIP_DEFLATE;
    ff.usize  = 0xffffffff;
    ff.csize  = 4200000;
    ff.offset = 100;
    TEST(zip_central64_len(&ff) == 20);
    TEST(zip_needed(&ff) == 45);
    p = zip_central(buf, &ff);
    TEST(get32(buf+42) == 100);
    got = zip_parse_central(buf, p-buf, 1, 5000000, &a);
    TEST(got && got->usize==0xffffffff && got->csize==4200000);
    TEST(got->offset==100 && !got->cextra.len);

    // Without a Zip64 extra, saturated fields are literal, as Info-ZIP
    // writes a size of exactly 0xffffffff, which bounds checks then judge
    zentry lit = ff;
    lit.usize = 10;
    p = zip_central(buf, &lit);
    put32(buf+24, 0xffffffff);
    got = zip_parse_central(buf, p-buf, 1, 5000000, &a);
    TEST(got && got->usize==0xffffffff && got->csize==4200000);
    lit.method = ZIP_STORE;
    lit.csize  = 10;
    p = zip_central(buf, &lit);
    put32(buf+20, 0xffffffff);
    put32(buf+24, 0xffffffff);
    TEST(!zip_parse_central(buf, p-buf, 1, 0xffffffff, &a));
    got = zip_parse_central(buf, p-buf, 1, (i64)0xffffffff + 100, &a);
    TEST(got && got->usize==0xffffffff && got->csize==0xffffffff);
    f.offset = 100;
    p = zip_central(buf, &f);
    TEST(zip_central64_len(&f) == 0);
    put32(buf+42, 0xffffffff);
    TEST(!zip_parse_central(buf, p-buf, 1, 1000, &a));

    // But a Zip64 extra too short for the saturated fields is malformed
    lit.cextra = S("\x01\x00\x08\x00" "\xff\xff\xff\xff\x00\x00\x00\x00");
    p = zip_central(buf, &lit);
    put32(buf+20, 0xffffffff);
    put32(buf+24, 0xffffffff);
    TEST(!zip_parse_central(buf, p-buf, 1, (i64)0xffffffff + 100, &a));
    put32(buf+20, 10);
    got = zip_parse_central(buf, p-buf, 1, (i64)0xffffffff + 100, &a);
    TEST(got && got->usize==0xffffffff && got->csize==10);

    // Except one holding just a saturated offset, which Info-ZIP writes
    // past 4 GiB beside literal sizes of exactly 0xffffffff
    lit.cextra = S("\x01\x00\x08\x00" "\x00\x00\x00\x40\x01\x00\x00\x00");
    p = zip_central(buf, &lit);
    put32(buf+24, 0xffffffff);
    put32(buf+42, 0xffffffff);
    got = zip_parse_central(buf, p-buf, 1, (i64)6 << 30, &a);
    TEST(got && got->usize==0xffffffff && got->csize==10);
    TEST(got->offset==(i64)5<<30 && !got->cextra.len);
    put32(buf+20, 0xffffffff);
    TEST(!zip_parse_central(buf, p-buf, 1, (i64)6 << 30, &a));  // bounds
    got = zip_parse_central(buf, p-buf, 1, (i64)10 << 30, &a);
    TEST(got && got->usize==0xffffffff && got->csize==0xffffffff);
    TEST(got->offset == (i64)5<<30);

    // Data descriptors: 32-bit sizes, or 64-bit for Zip64 entries
    u8 desc[24];
    TEST(zip_desc(desc, &f) - desc == 16);
    TEST(get32(desc)==ZIP_DESC_SIG && get32(desc+8)==10);
    TEST(zip_desc(desc, &e) - desc == 24);
    TEST(get64(desc+8)==(u64)e.csize && get64(desc+16)==(u64)e.usize);

    // Many entries alone call for Zip64 records
    u8 many[128];
    u8 *m = zip_end(many, 70000, 70000*46, 1000, (s8){0}, 0x001e);
    TEST(m-many == ZIP_END64_LEN + ZIP_LOC64_LEN + ZIP_END_LEN);
    TEST(get16(many+12)==0x001e && get16(many+14)==45);  // made by, needed
    TEST(get64(many+24) == 70000);
    TEST(get16(m-ZIP_END_LEN+10) == 0xffff);
    TEST(get32(m-ZIP_END_LEN+16) == 1000);

    // Below the thresholds, no Zip64 records
    TEST(!zip_end_needs64(65534, 100, 100));
    TEST( zip_end_needs64(65535, 100, 100));
    TEST( zip_end_needs64(1, 100, 0xffffffff));
}

static void test_extras(arena a)
{
    // Only Zip64 is dropped from a kept entry: AES (0x9901), whose
    // method depends on it, and unknown fields stay, as in Info-ZIP
    s8 x = S("UT\x05\x00\x03\x01\x02\x03\x04"
             "\x01\x00\x08\x00\x01\x02\x03\x04\x05\x06\x07\x08"
             "\x01\x99\x07\x00\x02\x00" "AE\x03\x08\x00"
             "\xfe\xca\x00\x00"
             "up\x01\x00\x01");
    s8 r = zip_filter_extra(&a, x);
    TEST(r.len == 9 + 11 + 4 + 5);
    TEST(!memcmp(r.s, x.s, 9));
    TEST(!memcmp(r.s+9, x.s+21, 20));

    s8 bad = S("UT\x05\x00\x03\x01\x02\x03\x04" "ux\x09\x00\x01");
    r = zip_filter_extra(&a, bad);
    TEST(r.len == 9);

    // The UT modification time, after other fields, unsigned
    i64 t = 0;
    s8 ut = S("ux\x00\x00" "UT\x05\x00\x03\x80\x5d\x8b\xfe");
    TEST(zip_extra_mtime(ut, &t) && t==0xfe8b5d80);
    s8 noflag = S("UT\x05\x00\x02\x80\x5d\x8b\x65");
    TEST(!zip_extra_mtime(noflag, &t));
    s8 shortut = S("UT\x04\x00\x01\x80\x5d\x8b");
    TEST(!zip_extra_mtime(shortut, &t));
    s8 cut = S("UT\x09\x00\x03\x80\x5d\x8b\x65");
    TEST(!zip_extra_mtime(cut, &t));

    // As in Info-ZIP, the last UT field decides, even without the time
    s8 two = S("UT\x05\x00\x01\x01\x00\x00\x00"
               "UT\x05\x00\x01\x02\x00\x00\x00");
    TEST(zip_extra_mtime(two, &t) && t==2);
    s8 cleared = S("UT\x05\x00\x01\x01\x00\x00\x00" "UT\x01\x00\x02");
    TEST(!zip_extra_mtime(cleared, &t));

    // Failing any, an old UX field (access, then modification time), but
    // not after a UT field, or a newer Ux field, which holds no times
    s8 ux1 = S("UX\x08\x00\x01\x00\x00\x00\x03\x00\x00\x00");
    TEST(zip_extra_mtime(ux1, &t) && t==3);
    s8 shortux = S("UX\x07\x00\x01\x00\x00\x00\x03\x00\x00");
    TEST(!zip_extra_mtime(shortux, &t));
    s8 utfirst = S("UT\x05\x00\x01\x01\x00\x00\x00"
                   "UX\x08\x00\x01\x00\x00\x00\x03\x00\x00\x00");
    TEST(zip_extra_mtime(utfirst, &t) && t==1);
    s8 uxfirst = S("UX\x08\x00\x01\x00\x00\x00\x03\x00\x00\x00"
                   "UT\x05\x00\x01\x01\x00\x00\x00");
    TEST(zip_extra_mtime(uxfirst, &t) && t==1);
    s8 uxthenut = S("UX\x08\x00\x01\x00\x00\x00\x03\x00\x00\x00"
                    "UT\x01\x00\x00");
    TEST(!zip_extra_mtime(uxthenut, &t));
    s8 ux2first = S("Ux\x04\x00\xe8\x03\xe8\x03"
                    "UX\x08\x00\x01\x00\x00\x00\x03\x00\x00\x00");
    TEST(!zip_extra_mtime(ux2first, &t));
    s8 ux2after = S("UX\x08\x00\x01\x00\x00\x00\x03\x00\x00\x00"
                    "Ux\x04\x00\xe8\x03\xe8\x03");
    TEST(!zip_extra_mtime(ux2after, &t));
    s8 newux = S("ux\x0b\x00\x01\x04\xe8\x03\x00\x00\x04\xe8\x03\x00\x00"
                 "UX\x08\x00\x01\x00\x00\x00\x03\x00\x00\x00");
    TEST(zip_extra_mtime(newux, &t) && t==3);  // ux holds IDs, ignored

    // The Unicode path, after other fields, if its CRC is the stored
    // name's (here "caf\x82.txt") and its version at most 1
    u32 crc   = 0xa0976e8f;
    u8  buf[] = "UT\x05\x00\x03\x80\x5d\x8b\xfe"
                "up\x0e\x00\x01\x8f\x6e\x97\xa0" "caf\xc3\xa9.txt";
    s8  up    = {buf, countof(buf)-1};
    TEST(equals(zip_extra_upath(up, crc), "caf\xc3\xa9.txt"));
    TEST(!zip_extra_upath(up, crc^1).s);
    up.s[13] = 0;
    TEST(equals(zip_extra_upath(up, crc), "caf\xc3\xa9.txt"));
    up.s[13] = 2;
    TEST(!zip_extra_upath(up, crc).s);
    s8 same = S("up\x05\x00\x01\x8f\x6e\x97\xa0");  // the stored name is UTF-8
    s8 r2   = zip_extra_upath(same, crc);
    TEST(r2.s && !r2.len);
    TEST(!zip_extra_upath(S("up\x04\x00\x01\x8f\x6e\x97"), crc).s);
    TEST(!zip_extra_upath(S("up\x0e\x00\x01\x8f\x6e\x97\xa0"), crc).s);
    TEST(!zip_extra_upath(S("UT\x05\x00\x03\x80\x5d\x8b\xfe"), crc).s);

    // Info-ZIP warns of one it reads, of version at most 1, but finds
    // stale: for another name, or too short to tell
    up.s[13] = 1;
    TEST(!zip_extra_upath_stale(up, crc));
    TEST( zip_extra_upath_stale(up, crc^1));
    up.s[13] = 2;
    TEST(!zip_extra_upath_stale(up, crc^1));
    TEST( zip_extra_upath_stale(S("up\x04\x00\x01\x8f\x6e\x97"), crc));
    TEST( zip_extra_upath_stale(S("up\x00\x00"), crc));
    TEST(!zip_extra_upath_stale(S("UT\x05\x00\x03\x80\x5d\x8b\xfe"), crc));
}

// Names not flagged UTF-8 are in an OEM code page if made on MS-DOS
// (but for PKZIP for Windows), OS/2, or by WinZip on NTFS, as Info-ZIP's
// Windows port judges.
static void test_oem(void)
{
    zentry e = {0};
    e.made = 0x0014;
    TEST(zip_oem_name(&e));
    e.flags = ZIP_FLAG_UTF8;
    TEST(!zip_oem_name(&e));
    e.flags = 0;
    e.made  = 0x0019;  // PKZIP 2.5, but no Unix attributes
    TEST(zip_oem_name(&e));
    e.extattr = 0x81a40020;
    TEST(!zip_oem_name(&e));
    e.made = 0x003f;   // 7-Zip
    TEST(zip_oem_name(&e));
    e.made = 0x0614;   // OS/2
    TEST(zip_oem_name(&e));
    e.made = 0x0b32;   // WinZip on NTFS
    TEST(zip_oem_name(&e));
    e.made = 0x0b14;
    TEST(!zip_oem_name(&e));
    e.made = 0x031e;   // Unix
    TEST(!zip_oem_name(&e));
}

// Zip64 end records are relied upon only when they check out, or when
// the end record defers to them or leaves room for them; otherwise the
// end record stands alone.
static void test_end_records(arena a)
{
    // An empty archive, its end record at the very start of the tail,
    // where no locator could precede it
    u8 *empty = new(&a, ZIP_END_LEN, u8);
    TEST(zip_end(empty, 0, 0, 0, (s8){0}, 0x031e) == empty+ZIP_END_LEN);
    zend z = {0};
    TEST(zip_find_end(empty, ZIP_END_LEN, ZIP_END_LEN, &z) == ZIP_OK);
    TEST(z.count==0 && z.endpos==0 && z.end64==-1);

    // Emptied after a stub, which precedes its directory: as Info-ZIP
    // writes it, offset 0, which UnZip needs to find it empty, or as
    // tugz once wrote it, at the stub's end, but not elsewhere
    u8 sfx[28+ZIP_END_LEN] = "#!/bin/sh\necho stub; exit 0\n";
    i64 offsets[] = {0, 28, 5};
    i32 want[]    = {ZIP_OK, ZIP_OK, ZIP_EPREFIX};
    for (i32 i = 0; i < countof(offsets); i++) {
        zip_end(sfx+28, 0, 0, offsets[i], (s8){0}, 0x031e);
        z = (zend){0};
        TEST(zip_find_end(sfx, countof(sfx), countof(sfx), &z) == want[i]);
        TEST(want[i] || (z.count==0 && z.endpos==28 && z.cdoff==28));
    }

    // One entry whose comment, just before the end record, ends with
    // bytes resembling a Zip64 locator that points at offset 0
    u8 fake[4+ZIP_LOC64_LEN] = "note";
    put32(fake+4, ZIP_LOC64_SIG);
    put32(fake+8, 0);
    put64(fake+12, 0);
    put32(fake+20, 1);
    zentry e = {0};
    e.name    = str("a.txt");
    e.csize   = 5;
    e.usize   = 5;
    e.comment = (s8){fake, countof(fake)};
    u8 *buf = new(&a, 256, u8);
    u8 *p   = zip_local(buf, &e);
    p = putbytes(p, str("hello"));
    i64 cdoff = p - buf;
    p = zip_central(p, &e);
    i64 cdsize = p - buf - cdoff;
    u8 *end = p;
    u8 *loc = end - ZIP_LOC64_LEN;
    p = zip_end(p, 1, cdsize, cdoff, (s8){0}, 0x031e);
    iz total = p - buf;

    // The locator checks out, so its record is read, but no Zip64 record
    // is there: the end record suffices, and is used alone
    z = (zend){0};
    TEST(zip_find_end(buf, total, total, &z) == ZIP_OK);
    TEST(z.end64 == 0);
    TEST(zip_parse_end64(buf+z.end64, &z) == ZIP_OK);
    TEST(z.end64 == -1);
    TEST(z.count==1 && z.cdoff==cdoff && z.cdsize==cdsize);

    // A count of entries on this disk that differs from the total, on a
    // single disk, as some writers make, as Info-ZIP reads it
    put16(end+8, 0);
    z = (zend){0};
    TEST(zip_find_end(buf, total, total, &z) == ZIP_OK);
    TEST(zip_parse_end64(buf+z.end64, &z) == ZIP_OK);
    TEST(z.count == 1);
    put16(end+8, 1);

    // Locators that cannot be right are ignored outright
    put64(loc+8, (u64)1 << 40);
    TEST(zip_find_end(buf, total, total, &z) == ZIP_OK);
    TEST(z.end64==-1 && z.count==1 && z.cdoff==cdoff);
    put64(loc+8, 0);
    put32(loc+16, 2);  // total disks
    TEST(zip_find_end(buf, total, total, &z) == ZIP_OK);
    TEST(z.end64==-1 && z.count==1 && z.cdoff==cdoff);

    // Unless the end record defers to Zip64 records
    put16(end+8,  ZIP_MAX16);  // entries on this disk
    put16(end+10, ZIP_MAX16);  // entries
    TEST(zip_find_end(buf, total, total, &z) == ZIP_EMULTI);
    put32(loc+16, 1);
    put64(loc+8, (u64)1 << 40);
    TEST(zip_find_end(buf, total, total, &z) == ZIP_EFORMAT);
    put64(loc+8, 0);
    TEST(zip_find_end(buf, total, total, &z) == ZIP_OK);
    TEST(zip_parse_end64(buf+z.end64, &z) == ZIP_EFORMAT);  // no record

    // Zip64 records after a central directory of 70,000 entries at offset
    // 1000, as at the end of a large file
    u8  tail[ZIP_END64_LEN + ZIP_LOC64_LEN + ZIP_END_LEN];
    u8 *rec   = tail;
    u8 *loc64 = tail + ZIP_END64_LEN;
    i64 n     = 70000;
    i64 big   = n * ZIP_CENTRAL_LEN;
    i64 size  = 1000 + big + countof(tail);
    TEST(zip_end(tail, n, big, 1000, (s8){0}, 0x031e) == tail+countof(tail));
    TEST(zip_find_end(tail, countof(tail), size, &z) == ZIP_OK);
    TEST(z.end64 == 1000+big);
    TEST(zip_parse_end64(rec, &z) == ZIP_OK);
    TEST(z.count==n && z.cdsize==big && z.cdoff==1000);

    // A bad Zip64 record is an error when the end record defers to it
    rec[0] ^= 1;
    TEST(zip_find_end(tail, countof(tail), size, &z) == ZIP_OK);
    TEST(zip_parse_end64(rec, &z) == ZIP_EFORMAT);
    rec[0] ^= 1;
    put32(rec+20, 1);  // disk with the central directory
    TEST(zip_find_end(tail, countof(tail), size, &z) == ZIP_OK);
    TEST(zip_parse_end64(rec, &z) == ZIP_EMULTI);
    put32(rec+20, 0);

    // More entries than the central directory could hold
    put64(rec+24, (u64)n+1);
    put64(rec+32, (u64)n+1);
    TEST(zip_find_end(tail, countof(tail), size, &z) == ZIP_OK);
    TEST(zip_parse_end64(rec, &z) == ZIP_EFORMAT);
    put64(rec+24, (u64)n);
    put64(rec+32, (u64)n);

    // Nor do counts of entries on this disk, nor this disk's number, tell
    // of a split archive, as in Info-ZIP: only disks with records do
    put64(rec+24, 1);  // entries on this disk
    put32(rec+16, 1);  // this disk
    TEST(zip_find_end(tail, countof(tail), size, &z) == ZIP_OK);
    TEST(zip_parse_end64(rec, &z) == ZIP_OK);
    TEST(z.count == n);
    put64(rec+24, (u64)n);
    put32(rec+16, 0);

    // Data prepended without adjusting offsets: the locator points short
    // of the record, at whatever is there, such as central headers
    u8 cd[ZIP_END64_LEN] = {0};
    put32(cd, ZIP_CENTRAL_SIG);
    TEST(zip_find_end(tail, countof(tail), size+100, &z) == ZIP_OK);
    TEST(z.end64 == 1000+big);
    TEST(zip_parse_end64(cd, &z) == ZIP_EFORMAT);

    // Only the locator adjusted: the central directory ends short of the
    // record
    put64(loc64+8, (u64)(1000 + big + 100));
    TEST(zip_find_end(tail, countof(tail), size+100, &z) == ZIP_OK);
    TEST(z.end64 == 1000+big+100);
    TEST(zip_parse_end64(rec, &z) == ZIP_EPREFIX);
    put64(loc64+8, (u64)(1000 + big));

    // Bytes between the record and its locator: by its size, the record
    // falls short of the locator, which is no prepended data
    u8 gap[countof(tail) + 8];
    bytecopy(gap, rec, ZIP_END64_LEN);
    bytecopy(gap+ZIP_END64_LEN, "JUNKJUNK", 8);
    bytecopy(gap+ZIP_END64_LEN+8, loc64, countof(tail)-ZIP_END64_LEN);
    TEST(zip_find_end(gap, countof(gap), size+8, &z) == ZIP_OK);
    TEST(z.end64 == 1000+big);
    TEST(zip_parse_end64(gap, &z) == ZIP_EFORMAT);

    // Zip64 records beside an end record of real values, as some writers
    // make for small archives: their faults are theirs, not taken for
    // data before the archive, as the end record alone would be, its
    // central directory ending before them
    i64 small = 1000 + 2*ZIP_CENTRAL_LEN + countof(tail);
    u8 *end32 = loc64 + ZIP_LOC64_LEN;
    zip_end(tail, ZIP_MAX16, 2*ZIP_CENTRAL_LEN, 1000, (s8){0}, 0x031e);
    put64(rec+24, 2);
    put64(rec+32, 2);
    put16(end32+8, 2);
    put16(end32+10, 2);
    TEST(zip_find_end(tail, countof(tail), small, &z) == ZIP_OK);
    TEST(zip_parse_end64(rec, &z) == ZIP_OK);
    TEST(z.count==2 && z.cdoff==1000 && z.end64==1000+2*ZIP_CENTRAL_LEN);
    put32(loc64+4, 1);  // the record on disk 1
    TEST(zip_find_end(tail, countof(tail), small, &z) == ZIP_EMULTI);
    put32(loc64+4, 0);
    put32(loc64+16, 2);  // total disks
    TEST(zip_find_end(tail, countof(tail), small, &z) == ZIP_EMULTI);
    put32(loc64+16, 0);
    TEST(zip_find_end(tail, countof(tail), small, &z) == ZIP_EMULTI);
    put32(loc64+16, 1);
    put64(loc64+8, (u64)small);  // past the end
    TEST(zip_find_end(tail, countof(tail), small, &z) == ZIP_EFORMAT);
    put64(loc64+8, 1000 + 2*ZIP_CENTRAL_LEN);
    put64(rec+48, 1001);  // central directory offset
    TEST(zip_find_end(tail, countof(tail), small, &z) == ZIP_OK);
    TEST(zip_parse_end64(rec, &z) == ZIP_EFORMAT);
    put64(rec+48, 999);
    TEST(zip_find_end(tail, countof(tail), small, &z) == ZIP_OK);
    TEST(zip_parse_end64(rec, &z) == ZIP_EPREFIX);  // by the record
    put64(rec+48, 1000);
    put32(rec+20, 1);  // disk with the central directory
    TEST(zip_find_end(tail, countof(tail), small, &z) == ZIP_OK);
    TEST(zip_parse_end64(rec, &z) == ZIP_EMULTI);
}

// Zip64 extra fields in central headers: one 8-byte value for each
// saturated field, in order, then a 4-byte disk number if the disk is
// saturated, which must be zero.
static void test_central64(arena a)
{
    u8 *buf = new(&a, 256, u8);
    u8 *ext = buf + ZIP_CENTRAL_LEN + 1;  // after the one-byte name
    zentry e = {0};
    e.name   = str("x");
    e.csize  = 10;
    e.usize  = 10;
    e.offset = 100;

    // A Zip64 extra holding only a disk number, needed only when the
    // disk is saturated
    e.cextra = S("\x01\x00\x04\x00" "\x00\x00\x00\x00");
    iz len = zip_central(buf, &e) - buf;
    zentry *got = zip_parse_central(buf, len, 1, 1000, &a);
    TEST(got && got->offset==100 && !got->cextra.len);
    put16(buf+34, ZIP_MAX16);
    got = zip_parse_central(buf, len, 1, 1000, &a);
    TEST(got && got->offset==100 && !got->cextra.len);
    put32(ext+4, 1);
    TEST(!zip_parse_central(buf, len, 1, 1000, &a));
    put32(ext+4, 0);
    put16(buf+34, 1);  // a real disk number: a split archive
    TEST(!zip_parse_central(buf, len, 1, 1000, &a));

    // Without the disk number, a saturated disk is malformed
    e.cextra = S("\x01\x00\x00\x00");
    len = zip_central(buf, &e) - buf;
    TEST(zip_parse_central(buf, len, 1, 1000, &a));
    put16(buf+34, ZIP_MAX16);
    TEST(!zip_parse_central(buf, len, 1, 1000, &a));
    e.cextra = (s8){0};
    len = zip_central(buf, &e) - buf;
    put16(buf+34, ZIP_MAX16);
    TEST(!zip_parse_central(buf, len, 1, 1000, &a));

    // The disk number follows a saturated offset
    e.cextra = S("\x01\x00\x0c\x00" "\x64\x00\x00\x00\x00\x00\x00\x00"
                 "\x00\x00\x00\x00");
    len = zip_central(buf, &e) - buf;
    put32(buf+42, 0xffffffff);
    put16(buf+34, ZIP_MAX16);
    got = zip_parse_central(buf, len, 1, 1000, &a);
    TEST(got && got->offset==100 && !got->cextra.len);
    e.cextra = S("\x01\x00\x08\x00" "\x64\x00\x00\x00\x00\x00\x00\x00");
    len = zip_central(buf, &e) - buf;
    put32(buf+42, 0xffffffff);
    got = zip_parse_central(buf, len, 1, 1000, &a);
    TEST(got && got->offset==100);
    put16(buf+34, ZIP_MAX16);
    TEST(!zip_parse_central(buf, len, 1, 1000, &a));

    // Values must fit in i64
    put16(buf+34, 0);
    put64(ext+4, (u64)1 << 63);
    TEST(!zip_parse_central(buf, len, 1, 1000, &a));
    put32(buf+42, 100);
    put32(buf+24, 0xffffffff);  // uncompressed size
    TEST(!zip_parse_central(buf, len, 1, 1000, &a));
    put64(ext+4, 0x7fffffffffffffff);
    got = zip_parse_central(buf, len, 1, 1000, &a);
    TEST(got && got->usize==0x7fffffffffffffff && got->offset==100);
}

// Name, extra, and comment lengths are 16 bits in both headers, where
// Zip64 extra fields add 20 bytes (local) or 20 or 28 (central).
static void test_fits(void)
{
    static u8 big[0x10000];
    zentry e = {0};
    e.name   = (s8){big, 0xffff};
    e.lextra = (s8){big, 0xffff};
    e.cextra = (s8){big, 0xffff};
    e.comment = (s8){big, 0xffff};
    TEST(zip_fits(&e));
    e.name.len++;
    TEST(!zip_fits(&e));
    e.name.len = 1;
    e.comment.len++;
    TEST(!zip_fits(&e));
    e.comment.len = 0;

    // A copied entry that needs Zip64 fields must have room for them
    e.zip64  = 1;
    e.usize  = (i64)5 << 30;
    TEST(!zip_fits(&e));
    e.lextra.len = 0xffff - 20;
    TEST(!zip_fits(&e));  // central: 20 more bytes
    e.cextra.len = 0xffff - 20;
    TEST(zip_fits(&e));
    e.offset = (i64)6 << 30;
    TEST(!zip_fits(&e));  // and 8 more for the offset
    e.cextra.len = 0xffff - 28;
    TEST(zip_fits(&e));
}

// The version needed is in the low byte, which a copied entry may need
// raised; some writers put a host system in the high byte, kept as is.
static void test_needed(void)
{
    zentry e = {0};
    e.method = ZIP_DEFLATE;
    e.needed = 0x0214;
    TEST(zip_needed(&e) == 0x0214);
    e.offset = (i64)5 << 30;
    TEST(zip_needed(&e) == 0x022d);
    e.needed = 0x0b3f;
    TEST(zip_needed(&e) == 0x0b3f);
    e.needed = 0;
    e.offset = 0;
    TEST(zip_needed(&e) == 20);
}

int main(void)
{
    (void)bytemove;
    iz cap = (iz)1 << 24;
    arena a = {0};
    a.beg = malloc((uz)cap);
    a.end = a.beg + cap;

    test_dostime();
    test_utf8();
    test_match();
    test_names(a);
    test_percent();
    test_roundtrip_headers(a);
    test_zip64(a);
    test_extras(a);
    test_oem();
    test_end_records(a);
    test_central64(a);
    test_fits();
    test_needed();

    free(a.beg);
    puts("all zip tests pass");
    return 0;
}
