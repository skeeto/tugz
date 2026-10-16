// Unit tests for unzip's rules (src/unzip.c, and ZipInfo mode's in
// src/zipinfo.c)
// On success prints "all unzip tests pass". A failure traps.
// $ cc -g3 -fsanitize=address,undefined -o tests-unzip test/unziptests.c
//
// The recorded tables come from Info-ZIP UnZip 6.0 itself: single-entry
// archives, crafted for each row, were extracted (-o, and -j, -V, -K, or
// -X as noted) and listed (-l) with TZ=UTC and a umask of 027 by macOS's
// /usr/bin/unzip and Debian's 6.0-29 (Ubuntu's 6.0-29ubuntu1 on aarch64),
// and the names, modes, times, and messages of the results noted. They
// agree but where noted, and then the table has Debian's: the owner IDs,
// which macOS's does not restore, show in Debian's warning for failing
// to set them as another user's. Names that macOS's file system refuses
// (not UTF-8) are Debian's. Windows names, from win32/win32.c's rules,
// are not recorded. Extraction times leave out what UnZip gets wrong,
// in dos_to_unix_time's own arithmetic (fileio.c): DOS months 0, 14,
// and 15, read from outside its table of days, and dates past 2100, a
// day late, as it counts 2100 a leap year for the years after it. Nor
// does tugz read, as Debian's does, a "ux" owner ID past the field's end.
// Displayed names leave out those not UTF-8, which both show as they
// are, the Mac's in its Latin-1 C locale.
//
// ZipInfo's tables (recorded_options through recorded_ratios) come from
// its zipinfo, Info-ZIP's ZipInfo 3.00 in UnZip 6.0, the same two builds,
// which agree on every row: archives of entries crafted for each table
// (central headers alone matter, so no data) were listed with -s, -T,
// or -m under TZ=UTC, and the field in question read off each line; for
// the options, an archive with a comment and one entry was listed after
// each sequence of words, with and without that entry named as a
// member, and its lines told which of the header, entries, totals, and
// comment showed, and in what format; for the program's names, links of
// each name to zipinfo listed an archive (-1), or showed the usage.
// Left out are what the builds abort on (versions made by from 10.0 but
// with Unix's attributes, and methods from 1000, which Apple's build
// overruns its buffer with), and compression factors whose arithmetic
// overflows; tugz's own rows for those follow each table's test.
#include "../src/base.c"
#pragma GCC diagnostic push  // unzip.c needs only a little of zip.c
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../src/zip.c"
#pragma GCC diagnostic pop
#include "../src/unzip.c"
#include "../src/zipinfo.c"

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

// As TEST, naming the table row that failed
#define TESTROW(c, i) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "%s:%d: FAIL (row %d): %s\n", __FILE__, \
                    __LINE__, (int)(i), #c); \
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
    return a.len==b.len && (!a.len || !memcmp(a.s, b.s, (uz)a.len));
}

// Names: (name, made, options) -> (path, flags), recorded
static struct {
    char const *name;
    u32         made;
    i32         opts;
    char const *path;
    i32         flags;
} recorded_names[] = {
    {"a.txt", 0x031e, 0, "a.txt", 0},
    {"a.txt", 0x031e, UZ_JUNK, "a.txt", 0},
    {"dir/a.txt", 0x031e, 0, "dir/a.txt", 0},
    {"dir/a.txt", 0x031e, UZ_JUNK, "a.txt", 0},
    {"/abs/a.txt", 0x031e, 0, "abs/a.txt", UZ_ABSOLUTE},
    {"/abs/a.txt", 0x031e, UZ_JUNK, "a.txt", UZ_ABSOLUTE},
    {"//abs2/a.txt", 0x031e, 0, "abs2/a.txt", UZ_ABSOLUTE},
    {"//abs2/a.txt", 0x031e, UZ_JUNK, "a.txt", UZ_ABSOLUTE},
    {"../up.txt", 0x031e, 0, "up.txt", UZ_DOTDOT},
    {"../up.txt", 0x031e, UZ_JUNK, "up.txt", 0},
    {"a/../b.txt", 0x031e, 0, "a/b.txt", UZ_DOTDOT},
    {"a/../b.txt", 0x031e, UZ_JUNK, "b.txt", 0},
    {"a/./b.txt", 0x031e, 0, "a/b.txt", 0},
    {"a/./b.txt", 0x031e, UZ_JUNK, "b.txt", 0},
    {"./c.txt", 0x031e, 0, "c.txt", 0},
    {"./c.txt", 0x031e, UZ_JUNK, "c.txt", 0},
    {"a//d.txt", 0x031e, 0, "a/d.txt", 0},
    {"a//d.txt", 0x031e, UZ_JUNK, "d.txt", 0},
    {"..", 0x031e, 0, "__", 0},
    {"..", 0x031e, UZ_JUNK, "__", 0},
    {".", 0x031e, 0, "_", 0},
    {".", 0x031e, UZ_JUNK, "_", 0},
    {"x/..", 0x031e, 0, "x/__", 0},
    {"x/..", 0x031e, UZ_JUNK, "__", 0},
    {"x/.", 0x031e, 0, "x/_", 0},
    {"x/.", 0x031e, UZ_JUNK, "_", 0},
    {"dots/...", 0x031e, 0, "dots/...", 0},
    {"dots/...", 0x031e, UZ_JUNK, "...", 0},
    {"f;1", 0x031e, 0, "f", 0},
    {"f;1", 0x031e, UZ_JUNK, "f", 0},
    {"f;", 0x031e, 0, "f", 0},
    {"f;", 0x031e, UZ_JUNK, "f", 0},
    {"f;12a", 0x031e, 0, "f;12a", 0},
    {"f;12a", 0x031e, UZ_JUNK, "f;12a", 0},
    {"d;1/f;2", 0x031e, 0, "d;1/f", 0},
    {"d;1/f;2", 0x031e, UZ_JUNK, "f", 0},
    {"a;1;2", 0x031e, 0, "a;1", 0},
    {"a;1;2", 0x031e, UZ_JUNK, "a;1", 0},
    {"ctl\x01x", 0x031e, 0, "ctlx", 0},
    {"ctl\x01x", 0x031e, UZ_JUNK, "ctlx", 0},
    {"tab\x09x", 0x031e, 0, "tabx", 0},
    {"tab\x09x", 0x031e, UZ_JUNK, "tabx", 0},
    {"del\x7fx", 0x031e, 0, "delx", 0},
    {"del\x7fx", 0x031e, UZ_JUNK, "delx", 0},
    {"esc\x1b[1mx", 0x031e, 0, "esc[1mx", 0},
    {"esc\x1b[1mx", 0x031e, UZ_JUNK, "esc[1mx", 0},
    {"sp ace", 0x031e, 0, "sp ace", 0},
    {"sp ace", 0x031e, UZ_JUNK, "sp ace", 0},
    {"back\\slash", 0x031e, 0, "back\\slash", 0},
    {"back\\slash", 0x031e, UZ_JUNK, "back\\slash", 0},
    {"C:/x.txt", 0x031e, 0, "C:/x.txt", 0},
    {"C:/x.txt", 0x031e, UZ_JUNK, "x.txt", 0},
    {"con.txt", 0x031e, 0, "con.txt", 0},
    {"con.txt", 0x031e, UZ_JUNK, "con.txt", 0},
    {"a:b", 0x031e, 0, "a:b", 0},
    {"a:b", 0x031e, UZ_JUNK, "a:b", 0},
    {"q?*<>|\".txt", 0x031e, 0, "q?*<>|\".txt", 0},
    {"q?*<>|\".txt", 0x031e, UZ_JUNK, "q?*<>|\".txt", 0},
    {"trail.", 0x031e, 0, "trail.", 0},
    {"trail.", 0x031e, UZ_JUNK, "trail.", 0},
    {"trail ", 0x031e, 0, "trail ", 0},
    {"trail ", 0x031e, UZ_JUNK, "trail ", 0},
    {"../../../etc/z", 0x031e, 0, "etc/z", UZ_DOTDOT},
    {"../../../etc/z", 0x031e, UZ_JUNK, "z", 0},
    {"/../q", 0x031e, 0, "q", UZ_ABSOLUTE|UZ_DOTDOT},
    {"/../q", 0x031e, UZ_JUNK, "q", UZ_ABSOLUTE},
    {"a/b/c/d.txt", 0x031e, 0, "a/b/c/d.txt", 0},
    {"a/b/c/d.txt", 0x031e, UZ_JUNK, "d.txt", 0},
    {"x/;1", 0x031e, 0, "", UZ_FAILED},
    {"x/;1", 0x031e, UZ_JUNK, "", UZ_FAILED},
    {"..;1", 0x031e, 0, "__", 0},
    {"..;1", 0x031e, UZ_JUNK, "__", 0},
    {".;2", 0x031e, 0, "_", 0},
    {".;2", 0x031e, UZ_JUNK, "_", 0},
    {"...", 0x031e, 0, "...", 0},
    {"...", 0x031e, UZ_JUNK, "...", 0},
    {"a/.../b", 0x031e, 0, "a/.../b", 0},
    {"a/.../b", 0x031e, UZ_JUNK, "b", 0},
    {"hi\xc3\xa9", 0x031e, 0, "hi\xc3\xa9", 0},
    {"\xff\xfe.bin", 0x031e, 0, "\xfe.bin", 0},
    {"c1\xc2\x85x", 0x031e, 0, "c1\xc2\x85x", 0},
    {"lat\xe9", 0x031e, 0, "lat\xe9", 0},
    {"z\xc0\x80", 0x031e, 0, "z\xc0\x80", 0},
    {"dir/", 0x031e, 0, "dir", UZ_DIR},
    {"dir/", 0x031e, UZ_JUNK, "", UZ_DIR},
    {"../dir2/", 0x031e, 0, "dir2", UZ_DOTDOT|UZ_DIR},
    {"../dir2/", 0x031e, UZ_JUNK, "", UZ_DIR},
    {"/absdir/", 0x031e, 0, "absdir", UZ_ABSOLUTE|UZ_DIR},
    {"/absdir/", 0x031e, UZ_JUNK, "", UZ_ABSOLUTE|UZ_DIR},
    {"a;1/", 0x031e, 0, "a;1", UZ_DIR},
    {"a;1/", 0x031e, UZ_JUNK, "", UZ_DIR},
    {"./", 0x031e, 0, "", UZ_DIR},
    {"./", 0x031e, UZ_JUNK, "", UZ_DIR},
    {"/", 0x031e, 0, "", UZ_ABSOLUTE|UZ_FAILED},
    {"/", 0x031e, UZ_JUNK, "", UZ_ABSOLUTE|UZ_FAILED},
    {"x/../y/", 0x031e, 0, "x/y", UZ_DOTDOT|UZ_DIR},
    {"x/../y/", 0x031e, UZ_JUNK, "", UZ_DIR},
    {"deep/er/", 0x031e, 0, "deep/er", UZ_DIR},
    {"deep/er/", 0x031e, UZ_JUNK, "", UZ_DIR},
    {"f;1", 0x031e, UZ_KEEPVER, "f;1", 0},
    {"d;1/f;2", 0x031e, UZ_KEEPVER, "d;1/f;2", 0},
    {"f;", 0x031e, UZ_KEEPVER, "f;", 0},
    {"x/;1", 0x031e, UZ_KEEPVER, "x/;1", 0},
    {"win\\dir\\f.txt", 0x001e, 0, "win/dir/f.txt", UZ_BACKSLASH},
    {"win\\dir\\f.txt", 0x001e, UZ_JUNK, "f.txt", UZ_BACKSLASH},
    {"mixed/dir\\f.txt", 0x001e, 0, "mixed/dir\\f.txt", 0},
    {"mixed/dir\\f.txt", 0x001e, UZ_JUNK, "dir\\f.txt", 0},
    {"\\lead\\f.txt", 0x001e, 0, "lead/f.txt", UZ_BACKSLASH|UZ_ABSOLUTE},
    {"\\lead\\f.txt", 0x001e, UZ_JUNK, "f.txt", UZ_BACKSLASH|UZ_ABSOLUTE},
    {"C:\\x\\y.txt", 0x001e, 0, "C:/x/y.txt", UZ_BACKSLASH},
    {"C:\\x\\y.txt", 0x001e, UZ_JUNK, "y.txt", UZ_BACKSLASH},
    {"..\\up\\f.txt", 0x001e, 0, "up/f.txt", UZ_BACKSLASH|UZ_DOTDOT},
    {"..\\up\\f.txt", 0x001e, UZ_JUNK, "f.txt", UZ_BACKSLASH},
    {"dirb\\", 0x001e, 0, "dirb", UZ_BACKSLASH|UZ_DIR},
    {"dirb\\", 0x001e, UZ_JUNK, "", UZ_BACKSLASH|UZ_DIR},
    {"ntfs\\dir\\f.txt", 0x0b1e, 0, "ntfs\\dir\\f.txt", 0},
    {"hpfs\\dir\\f.txt", 0x061e, 0, "hpfs\\dir\\f.txt", 0},
};

// Modes: (made, external attributes, name, central extra, -K) ->
// permissions with a umask of 027, and whether a link, recorded
static struct {
    u32         made;
    u32         ext;
    char const *name;
    char const *cextra;
    iz          cxlen;
    b32         keep;
    u32         perm;
    b32         link;
} recorded_modes[] = {
    {0x031e, 0x81ed0000, "m", "", 0, 0, 00755, 0},  // unix 755
    {0x031e, 0x81ed0000, "m", "", 0, 1, 00755, 0},  // unix 755
    {0x031e, 0x81a40000, "m", "", 0, 0, 00644, 0},  // unix 644
    {0x031e, 0x81a40000, "m", "", 0, 1, 00644, 0},  // unix 644
    {0x031e, 0x81800000, "m", "", 0, 0, 00600, 0},  // unix 600
    {0x031e, 0x81800000, "m", "", 0, 1, 00600, 0},  // unix 600
    {0x031e, 0x89ed0000, "m", "", 0, 0, 00755, 0},  // unix suid
    {0x031e, 0x89ed0000, "m", "", 0, 1, 04755, 0},  // unix suid
    {0x031e, 0x85ed0000, "m", "", 0, 0, 00755, 0},  // unix sgid
    {0x031e, 0x85ed0000, "m", "", 0, 1, 02755, 0},  // unix sgid
    {0x031e, 0x83ff0000, "m", "", 0, 0, 00777, 0},  // unix sticky
    {0x031e, 0x83ff0000, "m", "", 0, 1, 01777, 0},  // unix sticky
    {0x031e, 0x80000000, "m", "", 0, 0, 00000, 0},  // unix 000
    {0x031e, 0x80000000, "m", "", 0, 1, 00000, 0},  // unix 000
    {0x031e, 0x00000000, "m", "", 0, 0, 00000, 0},  // unix no attrs
    {0x031e, 0x00000000, "m", "", 0, 1, 00000, 0},  // unix no attrs
    {0x031e, 0x00000001, "m", "", 0, 0, 00000, 0},  // unix no attrs ro
    {0x031e, 0x00000001, "m", "", 0, 1, 00000, 0},  // unix no attrs ro
    {0x031e, 0x0fff0000, "m", "", 0, 0, 00777, 0},  // unix no type 7777
    {0x031e, 0x0fff0000, "m", "", 0, 1, 07777, 0},  // unix no type 7777
    {0x001e, 0x00000000, "m", "", 0, 0, 00640, 0},  // fat 0
    {0x001e, 0x00000000, "m", "", 0, 1, 00640, 0},  // fat 0
    {0x001e, 0x00000001, "m", "", 0, 0, 00440, 0},  // fat ro
    {0x001e, 0x00000001, "m", "", 0, 1, 00440, 0},  // fat ro
    {0x001e, 0x00000020, "m", "", 0, 0, 00640, 0},  // fat archive
    {0x001e, 0x00000020, "m", "", 0, 1, 00640, 0},  // fat archive
    {0x001e, 0x00000027, "m", "", 0, 0, 00440, 0},  // fat rhs+a
    {0x001e, 0x00000027, "m", "", 0, 1, 00440, 0},  // fat rhs+a
    {0x001e, 0x81ed0000, "m", "", 0, 0, 00640, 0},  // fat unix 755 consistent
    {0x001e, 0x81ed0000, "m", "", 0, 1, 00640, 0},  // fat unix 755 consistent
    {0x001e, 0x81ed0001, "m", "", 0, 0, 00440, 0},  // fat unix 755 ro inconsistent
    {0x001e, 0x81ed0001, "m", "", 0, 1, 00440, 0},  // fat unix 755 ro inconsistent
    {0x001e, 0x816d0001, "m", "", 0, 0, 00440, 0},  // fat unix 555 ro consistent
    {0x001e, 0x816d0001, "m", "", 0, 1, 00440, 0},  // fat unix 555 ro consistent
    {0x001e, 0x81a40000, "m", "", 0, 0, 00644, 0},  // fat unix 644 (owner rw-: consistent)
    {0x001e, 0x81a40000, "m", "", 0, 1, 00644, 0},  // fat unix 644 (owner rw-: consistent)
    {0x001e, 0x81e40000, "m", "", 0, 0, 00640, 0},  // fat unix 744 (x: inconsistent)
    {0x001e, 0x81e40000, "m", "", 0, 1, 00640, 0},  // fat unix 744 (x: inconsistent)
    {0x0b1e, 0x00000020, "m", "", 0, 0, 00640, 0},  // ntfs
    {0x0b1e, 0x00000020, "m", "", 0, 1, 00640, 0},  // ntfs
    {0x0b1e, 0x00000021, "m", "", 0, 0, 00440, 0},  // ntfs ro
    {0x0b1e, 0x00000021, "m", "", 0, 1, 00440, 0},  // ntfs ro
    {0x0b1e, 0x81ed0000, "m", "", 0, 0, 00640, 0},  // ntfs unix 755
    {0x0b1e, 0x81ed0000, "m", "", 0, 1, 00640, 0},  // ntfs unix 755
    {0x061e, 0x00000000, "m", "", 0, 0, 00640, 0},  // hpfs
    {0x061e, 0x00000000, "m", "", 0, 1, 00640, 0},  // hpfs
    {0x071e, 0x81ed0000, "m", "", 0, 0, 00640, 0},  // mac 755
    {0x071e, 0x81ed0000, "m", "", 0, 1, 00640, 0},  // mac 755
    {0x131e, 0x81ed0000, "m", "", 0, 0, 00640, 0},  // osx 755
    {0x131e, 0x81ed0000, "m", "", 0, 1, 00640, 0},  // osx 755
    {0x131e, 0x81c00001, "m", "", 0, 0, 00440, 0},  // osx 700 ro
    {0x131e, 0x81c00001, "m", "", 0, 1, 00440, 0},  // osx 700 ro
    {0x0a1e, 0x00000000, "m", "", 0, 0, 00640, 0},  // tops20
    {0x0a1e, 0x00000000, "m", "", 0, 1, 00640, 0},  // tops20
    {0x011e, 0x000e0000, "m", "", 0, 0, 00750, 0},  // amiga rwe
    {0x011e, 0x000e0000, "m", "", 0, 1, 00750, 0},  // amiga rwe
    {0x011e, 0x000a0000, "m", "", 0, 0, 00550, 0},  // amiga r-e
    {0x011e, 0x000a0000, "m", "", 0, 1, 00550, 0},  // amiga r-e
    {0x011e, 0x00000000, "m", "", 0, 0, 00000, 0},  // amiga none
    {0x011e, 0x00000000, "m", "", 0, 1, 00000, 0},  // amiga none
    {0x121e, 0x81ed0000, "m", "", 0, 0, 00755, 0},  // theos 755
    {0x121e, 0x81ed0000, "m", "", 0, 1, 00755, 0},  // theos 755
    {0x121e, 0x41ed0000, "m", "", 0, 0, 00755, 0},  // theos dir bits on file
    {0x121e, 0x41ed0000, "m", "", 0, 1, 00755, 0},  // theos dir bits on file
    {0x021e, 0x81e80000, "m", "", 0, 0, 00750, 0},  // vms 750
    {0x021e, 0x81e80000, "m", "", 0, 1, 00750, 0},  // vms 750
    {0x051e, 0x81c90000, "m", "", 0, 0, 00711, 0},  // atari 711
    {0x051e, 0x81c90000, "m", "", 0, 1, 00711, 0},  // atari 711
    {0x101e, 0x81a00000, "m", "", 0, 0, 00640, 0},  // beos 640
    {0x101e, 0x81a00000, "m", "", 0, 1, 00640, 0},  // beos 640
    {0x1e1e, 0x81840000, "m", "", 0, 0, 00604, 0},  // atheos 604
    {0x1e1e, 0x81840000, "m", "", 0, 1, 00604, 0},  // atheos 604
    {0x0c1e, 0x81e90000, "m", "", 0, 0, 00751, 0},  // qdos 751
    {0x0c1e, 0x81e90000, "m", "", 0, 1, 00751, 0},  // qdos 751
    {0x0d1e, 0x81c10000, "m", "", 0, 0, 00701, 0},  // acorn 701
    {0x0d1e, 0x81c10000, "m", "", 0, 1, 00701, 0},  // acorn 701
    {0x111e, 0x81c80000, "m", "", 0, 0, 00710, 0},  // tandem 710
    {0x111e, 0x81c80000, "m", "", 0, 1, 00710, 0},  // tandem 710
    {0x041e, 0x81ed0000, "m", "", 0, 0, 00640, 0},  // vm/cms 755
    {0x041e, 0x81ed0000, "m", "", 0, 1, 00640, 0},  // vm/cms 755
    {0x1f1e, 0x81ed0000, "m", "", 0, 0, 00640, 0},  // host 31
    {0x1f1e, 0x81ed0000, "m", "", 0, 1, 00640, 0},  // host 31
    {0xc81e, 0x81ed0000, "m", "", 0, 0, 00640, 0},  // host 200
    {0xc81e, 0x81ed0000, "m", "", 0, 1, 00640, 0},  // host 200
    {0x0e1e, 0x81ed0000, "m", "", 0, 0, 00640, 0},  // vfat 755
    {0x0e1e, 0x81ed0000, "m", "", 0, 1, 00640, 0},  // vfat 755
    {0x031e, 0x00000000, "m", "nu\x0c\x00\x00\x00\x00\x00\xe9\x81\x00\x00\x00\x00\x00\x00", 16, 0, 00751, 0},  // asi nu 751
    {0x031e, 0x00000000, "m", "\x0c\x00\x08\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12, 0, 00640, 0},  // pkvms
    {0x031e, 0x00000001, "m", "\x0c\x00\x08\x00\x00\x00\x00\x00\x00\x00\x00\x00", 12, 0, 00440, 0},  // pkvms ro
    {0x031e, 0x00000000, "m", "nu\x02\x00\x00\x00", 6, 0, 00640, 0},  // asi short
    {0x031e, 0x41ed0010, "dd/", "", 0, 0, 00755, 0},  // unix dir 755
    {0x031e, 0x41ed0010, "dd/", "", 0, 1, 00755, 0},  // unix dir 755
    {0x031e, 0x41c00010, "dd/", "", 0, 0, 00700, 0},  // unix dir 700
    {0x031e, 0x41c00010, "dd/", "", 0, 1, 00700, 0},  // unix dir 700
    {0x031e, 0x45fd0010, "dd/", "", 0, 0, 00775, 0},  // unix dir sgid
    {0x031e, 0x45fd0010, "dd/", "", 0, 1, 02775, 0},  // unix dir sgid
    {0x031e, 0x43ff0010, "dd/", "", 0, 0, 00777, 0},  // unix dir sticky
    {0x031e, 0x43ff0010, "dd/", "", 0, 1, 01777, 0},  // unix dir sticky
    {0x001e, 0x00000010, "dd/", "", 0, 0, 00750, 0},  // fat dir
    {0x001e, 0x00000010, "dd/", "", 0, 1, 00750, 0},  // fat dir
    {0x001e, 0x00000011, "dd/", "", 0, 0, 00550, 0},  // fat dir ro
    {0x001e, 0x00000011, "dd/", "", 0, 1, 00550, 0},  // fat dir ro
    {0x001e, 0x00000000, "dd/", "", 0, 0, 00750, 0},  // fat dir no bit
    {0x001e, 0x00000000, "dd/", "", 0, 1, 00750, 0},  // fat dir no bit
    {0x0b1e, 0x00000010, "dd/", "", 0, 0, 00750, 0},  // ntfs dir
    {0x0b1e, 0x00000010, "dd/", "", 0, 1, 00750, 0},  // ntfs dir
    {0x031e, 0xa1ff0000, "lnk", "", 0, 0, 00000, 1},  // unix link
    {0x031e, 0xa1ed0000, "lnk", "", 0, 0, 00000, 1},  // unix link 755
    {0x001e, 0xa1ff0000, "lnk", "", 0, 0, 00640, 0},  // fat link 777 (inconsistent)
    {0x001e, 0xa1ed0000, "lnk", "", 0, 0, 00640, 0},  // fat link 755 (consistent)
    {0x001e, 0xa16d0001, "lnk", "", 0, 0, 00440, 0},  // fat link 555 ro
    {0x0b1e, 0xa1ff0000, "lnk", "", 0, 0, 00640, 0},  // ntfs link
    {0x131e, 0xa1ff0000, "lnk", "", 0, 0, 00640, 0},  // osx link
    {0x021e, 0xa1ff0000, "lnk", "", 0, 0, 00000, 1},  // vms link
    {0x051e, 0xa1ff0000, "lnk", "", 0, 0, 00000, 1},  // atari link
    {0x101e, 0xa1ff0000, "lnk", "", 0, 0, 00000, 1},  // beos link
    {0x1e1e, 0xa1ff0000, "lnk", "", 0, 0, 00000, 1},  // atheos link
    {0x0c1e, 0xa1ff0000, "lnk", "", 0, 0, 00777, 0},  // qdos link
    {0x0d1e, 0xa1ff0000, "lnk", "", 0, 0, 00777, 0},  // acorn link
    {0x121e, 0xa1ff0000, "lnk", "", 0, 0, 00777, 0},  // theos link
    {0x001e, 0xa1a40000, "lnk", "", 0, 0, 00000, 1},  // fat link 644 consistent
    {0x001e, 0xa1a40000, "lnk", "", 0, 1, 00000, 1},  // fat link 644 consistent
    {0x001e, 0xa1240001, "lnk", "", 0, 0, 00000, 1},  // fat link 444 ro consistent
    {0x001e, 0xa1240001, "lnk", "", 0, 1, 00000, 1},  // fat link 444 ro consistent
    {0x001e, 0x81ed0010, "lnk", "", 0, 0, 00755, 0},  // fat dir bit 755 file
    {0x001e, 0x81ed0010, "lnk", "", 0, 1, 00755, 0},  // fat dir bit 755 file
    {0x001e, 0x89a40000, "lnk", "", 0, 0, 00644, 0},  // fat suid 644 consistent
    {0x001e, 0x89a40000, "lnk", "", 0, 1, 04644, 0},  // fat suid 644 consistent
    {0x031e, 0xa1ff0010, "lnk", "", 0, 0, 00000, 1},  // unix link with dir bit
    {0x031e, 0xa1ff0010, "lnk", "", 0, 1, 00000, 1},  // unix link with dir bit
    {0x0e1e, 0xa1a40000, "lnk", "", 0, 0, 00640, 0},  // vfat link 644
    {0x0e1e, 0xa1a40000, "lnk", "", 0, 1, 00640, 0},  // vfat link 644
    {0x001e, 0x41ed0010, "dz/", "", 0, 0, 00755, 0},  // fat dir 755 consistent
    {0x001e, 0x41c00010, "dz/", "", 0, 0, 00700, 0},  // fat dir 700 consistent
    {0x001e, 0x00000010, "dz\\", "", 0, 0, 00750, 0},  // fat dir backslash
};

// Times and owners: (DOS time, local extra) -> (mtime, atime, owner),
// recorded with TZ=UTC
static struct {
    u32         dostime;
    char const *lextra;
    iz          lxlen;
    i64         mtime;
    i64         atime;
    i32         owner;  // -1: not recorded (without -X)
    u32         uid;
    u32         gid;
} recorded_times[] = {
    {0x50cf645c, "", 0, 1592224496, 1592224496, -1, 0, 0},  // dos plain
    {0x50cf645d, "", 0, 1592224498, 1592224498, -1, 0, 0},  // dos odd
    {0x00210000, "", 0, 315532800, 315532800, -1, 0, 0},  // dos min
    {0x51af6000, "", 0, 1610712000, 1610712000, -1, 0, 0},  // dos month 13
    {0x50406000, "", 0, 1580472000, 1580472000, -1, 0, 0},  // dos day 0
    {0x505f6000, "", 0, 1583150400, 1583150400, -1, 0, 0},  // dos feb 31
    {0x525d6000, "", 0, 1614600000, 1614600000, -1, 0, 0},  // dos feb 29 nonleap
    {0x50cfc000, "", 0, 1592265600, 1592265600, -1, 0, 0},  // dos hour 24
    {0x50cfffff, "", 0, 1592294642, 1592294642, -1, 0, 0},  // dos all max
    {0x50cf679e, "", 0, 1592226060, 1592226060, -1, 0, 0},  // dos min 60 sec 60
    {0x743319c4, "", 0, 2147483648, 2147483648, -1, 0, 0},  // dos 2038
    {0xf0610000, "", 0, 4107542400, 4107542400, -1, 0, 0},  // dos 2100
    {0x285d0000, "", 0, 951782400, 951782400, -1, 0, 0},  // dos 2000 leap
    {0x50cf645c, "UT\x05\x00\x01\x01\x10^_", 9, 1600000001, 1600000001, -1, 0, 0},  // UT m
    {0x50cf645c, "UT\x09\x00\x03\x01\x10^_\x19\x0c^_", 13, 1600000001, 1599999001, -1, 0, 0},  // UT ma
    {0x50cf645c, "UT\x0d\x00\x07\x01\x10^_\x19\x0c^_1\x08^_", 17, 1600000001, 1599999001, -1, 0, 0},  // UT mac
    {0x50cf645c, "UT\x05\x00\x02\x19\x0c^_", 9, 1592224496, 1599999001, -1, 0, 0},  // UT a only
    {0x50cf645c, "UT\x05\x00\x03\x01\x10^_", 9, 1600000001, 1600000001, -1, 0, 0},  // UT ma truncated
    {0x50cf645c, "UT\x01\x00\x01", 5, 1592224496, 1592224496, -1, 0, 0},  // UT m no data
    {0x50cf645c, "UT\x01\x00\x00", 5, 1592224496, 1592224496, -1, 0, 0},  // UT none
    {0x50cf645c, "", 0, 1592224496, 1592224496, -1, 0, 0},  // UT central only
    {0x50cf645c, "UT\x05\x00\x01\x01\x10^_UT\x05\x00\x01\x0b\x10^_", 18, 1600000011, 1600000011, -1, 0, 0},  // UT twice
    {0x50cf645c, "UT\x09\x00\x03\x01\x00\x00\x80\x05\x00\x00\x80", 13, 1592224496, 1592224496, -1, 0, 0},  // UT high, dos 2020
    {0x74320000, "UT\x09\x00\x03\x01\x00\x00\x80\x05\x00\x00\x80", 13, 2147483649, 2147483653, -1, 0, 0},  // UT high, dos 2038-01-18
    {0x7431bf7d, "UT\x09\x00\x03\x01\x00\x00\x80\x05\x00\x00\x80", 13, 2147385598, 2147385598, -1, 0, 0},  // UT high, dos 2038-01-17
    {0x74320000, "UT\x09\x00\x03\x01\x00\x00\x80\x05\x00\x00\x00", 13, 2147483649, 5, -1, 0, 0},  // UT m high a low, 2038
    {0x50cf645c, "UT\x09\x00\x03\x05\x00\x00\x00\x05\x00\x00\x80", 13, 5, 5, -1, 0, 0},  // UT m low a high, dos 2020
    {0x50cf645c, "UT\x05\x00\x01\x01\x10^_ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00", 24, 1600000001, 1600000001, -1, 0, 0},  // UT ux
    {0x50cf645c, "ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00UT\x05\x00\x01\x01\x10^_", 24, 1600000001, 1600000001, -1, 0, 0},  // ux UT
    {0x50cf645c, "UX\x08\x00\x19\x0c^_\x01\x10^_", 12, 1600000001, 1599999001, -1, 0, 0},  // UX
    {0x50cf645c, "UX\x0c\x00\x19\x0c^_\x01\x10^_90\xa0[", 16, 1600000001, 1599999001, -1, 0, 0},  // UX full
    {0x50cf645c, "UX\x08\x00\x19\x0c^_\x01\x10^_UT\x05\x00\x01\x0b\x10^_", 21, 1600000011, 1600000011, -1, 0, 0},  // UX UT
    {0x50cf645c, "UT\x05\x00\x01\x0b\x10^_UX\x08\x00\x19\x0c^_\x01\x10^_", 21, 1600000011, 1600000011, -1, 0, 0},  // UT UX
    {0x50cf645c, "Ux\x04\x00" "90\xa0[UX\x08\x00\x19\x0c^_\x01\x10^_", 20, 1592224496, 1592224496, -1, 0, 0},  // Ux UX
    {0x50cf645c, "UX\x08\x00\x19\x0c^_\x01\x10^_Ux\x04\x00" "90\xa0[", 20, 1600000001, 1599999001, -1, 0, 0},  // UX Ux
    {0x50cf645c, "ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00UX\x08\x00\x19\x0c^_\x01\x10^_", 27, 1592224496, 1592224496, -1, 0, 0},  // ux UX
    {0x50cf645c, "UX\x08\x00\x19\x0c^_\x01\x10^_ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00", 27, 1600000001, 1599999001, -1, 0, 0},  // UX ux
    {0x50cf645c, "UX\x08\x00\x01\x00\x00\x80\x01\x10^_", 12, 1600000001, 1600000001, -1, 0, 0},  // UX a high
    {0x50cf645c, "UX\x08\x00\x01\x10^_\x01\x00\x00\x80", 12, 1592224496, 1592224496, -1, 0, 0},  // UX m high
    {0x50cf645c, "UX\x04\x00\x01\x10^_", 8, 1592224496, 1592224496, -1, 0, 0},  // UX short
    {0x50cf645c, "UT\x05\x00\x01\x01\x10^", 8, 1592224496, 1592224496, -1, 0, 0},  // UT cut (bad length)
    {0x50cf645c, "UT\x05\x00\x01\x01\x10^_\x01\x02", 11, 1600000001, 1600000001, -1, 0, 0},  // UT then junk
    {0x50cf645c, "UT\x07\x00\x01\x01\x10^_zz", 11, 1600000001, 1600000001, -1, 0, 0},  // UT m long
    {0x50cf645c, "ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00", 15, 1592224496, 1592224496, 1, 12345, 23456},  // owner ux 4
    {0x50cf645c, "ux\x07\x00\x01\x02" "90\x02\xa0[", 11, 1592224496, 1592224496, 1, 12345, 23456},  // owner ux 2
    {0x50cf645c, "ux\x13\x00\x01\x08" "90\x00\x00\x00\x00\x00\x00\x08\xa0[\x00\x00\x00\x00\x00\x00", 23, 1592224496, 1592224496, 1, 12345, 23456},  // owner ux 8
    {0x50cf645c, "Ux\x04\x00" "90\xa0[", 8, 1592224496, 1592224496, 1, 12345, 23456},  // owner Ux
    {0x50cf645c, "UX\x0c\x00\x01\x10^_\x01\x10^_90\xa0[", 16, 1600000001, 1600000001, 1, 12345, 23456},  // owner UX full
    {0x50cf645c, "UX\x08\x00\x01\x10^_\x01\x10^_", 12, 1600000001, 1600000001, 0, 0, 0},  // owner UX short
    {0x50cf645c, "Ux\x04\x00" "90\xa0[UX\x0c\x00\x01\x10^_\x01\x10^_g+\xceV", 24, 1592224496, 1592224496, 1, 12345, 23456},  // owner Ux UX
    {0x50cf645c, "UX\x0c\x00\x01\x10^_\x01\x10^_g+\xceVUx\x04\x00" "90\xa0[", 24, 1600000001, 1600000001, 1, 12345, 23456},  // owner UX Ux
    {0x50cf645c, "ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00Ux\x04\x00g+\xceV", 23, 1592224496, 1592224496, 1, 12345, 23456},  // owner ux Ux
    {0x50cf645c, "Ux\x04\x00g+\xceVux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00", 23, 1592224496, 1592224496, 1, 12345, 23456},  // owner Ux ux
    {0x50cf645c, "UT\x05\x00\x01\x01\x10^_UX\x0c\x00\x01\x10^_\x01\x10^_g+\xceV", 25, 1600000001, 1600000001, 0, 0, 0},  // owner UT UX
    {0x50cf645c, "ux\x0b\x00\x02\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00", 15, 1592224496, 1592224496, 0, 0, 0},  // owner ux version 2
    {0x50cf645c, "Ux\x06\x00\x01\x00\x02\x00\x03\x00", 10, 1592224496, 1592224496, 0, 0, 0},  // owner Ux long
    {0x50cf645c, "UT\x05\x00\x01\x01\x10^_Ux\x04\x00" "90\xa0[", 17, 1600000001, 1600000001, 1, 12345, 23456},  // UT Ux
    {0x50cf645c, "UT\x09\x00\x03\x01\x10^_\xfc\x0f^_Ux\x04\x00" "90\xa0[", 21, 1600000001, 1599999996, 1, 12345, 23456},  // UTma Ux
    {0x50cf645c, "UX\x0c\x00\x19\x0c^_\x01\x10^_g+\xceVux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00", 31, 1600000001, 1599999001, 1, 12345, 23456},  // UXfull ux
    {0x50cf645c, "ux\x0b\x00\x02\x04" "90\x00\x00\x04\xa0[\x00\x00UX\x0c\x00\x19\x0c^_\x01\x10^_g+\xceV", 31, 1592224496, 1592224496, 0, 0, 0},  // ux v2 UXfull
    {0x50cf645c, "UX\x0c\x00\x19\x0c^_\x01\x10^_g+\xceVux\x0b\x00\x02\x04" "90\x00\x00\x04\xa0[\x00\x00", 31, 1600000001, 1599999001, 0, 0, 0},  // UXfull ux v2
    {0x50cf645c, "ux\x05\x00\x01\x01" "9\x01\xa0", 9, 1592224496, 1592224496, 0, 0, 0},  // ux size 1 b
    {0x50cf645c, "ux\x05\x00\x01\x01{\x01\xea", 9, 1592224496, 1592224496, 0, 0, 0},  // ux size 1 small
    {0x50cf645c, "ux\x09\x00\x01\x03" "90\x00\x03\xa0[\x00", 13, 1592224496, 1592224496, 0, 0, 0},  // ux size 3
    {0x50cf645c, "ux\x09\x00\x01\x04" "90\x00\x00\x02\xa0[", 13, 1592224496, 1592224496, 1, 12345, 23456},  // ux 4/2
    {0x50cf645c, "ux\x13\x00\x01\x08\x00\x00\x00\x00\x02\x00\x00\x00\x08\xa0[\x00\x00\x00\x00\x00\x00", 23, 1592224496, 1592224496, 0, 0, 0},  // ux 8 big uid
    {0x50cf645c, "ux\x13\x00\x01\x08" "90\x00\x00\x00\x00\x00\x00\x08\x00\x00\x00\x00\x00\x01\x00\x00", 23, 1592224496, 1592224496, 0, 0, 0},  // ux 8 big gid
    {0x50cf645c, "ux\x05\x00\x01\x02" "90\x02", 9, 1592224496, 1592224496, 0, 0, 0},  // ux short gid
    {0x50cf645c, "ux\x07\x00\x01\x02" "90\x02\xa0[", 11, 1592224496, 1592224496, 1, 12345, 23456},  // ux exact 7
    {0x50cf645c, "ux\x09\x00\x01\x02" "90\x02\xa0[\x00\x00", 13, 1592224496, 1592224496, 1, 12345, 23456},  // ux 7 plus 2
    {0x50cf645c, "UX\x0c\x00\x19\x0c^_\x01\x10^_g+\xceVUT\x05\x00\x01\x0b\x10^_", 25, 1600000011, 1600000011, 1, 11111, 22222},  // UXfull UT
    {0x50cf645c, "UX\x0c\x00\x19\x0c^_\x01\x10^_g+\xceVUx\x04\x00" "90\xa0[UX\x0c\x00\x01\x00\x00\x00\x02\x00\x00\x00\x03\x00\x04\x00", 40, 1600000001, 1599999001, 1, 12345, 23456},  // UXfull Ux UXfull
    {0x50cf645c, "Ux\x04\x00" "90\xa0[Ux\x04\x00g+\xceV", 16, 1592224496, 1592224496, 1, 11111, 22222},  // Ux Ux
    {0x50cf645c, "ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00ux\x0b\x00\x01\x04g+\x00\x00\x04\xceV\x00\x00", 30, 1592224496, 1592224496, 1, 11111, 22222},  // ux ux
    {0x50cf645c, "ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00ux\x0b\x00\x02\x04\x00\x00\x00\x00\x00\x00\x00\x00\x00", 30, 1592224496, 1592224496, 0, 0, 0},  // ux ux-v2
    {0x50cf645c, "ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00ux\x03\x00\x01\x04\x00", 22, 1592224496, 1592224496, 0, 0, 0},  // ux ux-short
    {0x50cf645c, "UX\x0c\x00\x19\x0c^_\x01\x10^_g+\xceV", 16, 1600000001, 1599999001, -1, 0, 0},  // UXfull plain no -X
    {0x50cf645c, "UT\x05\x00\x01\x01\x10^_", 9, 1600000001, 1600000001, 0, 0, 0},  // UT central with uid
    {0x50cf645c, "Ux\x04\x00" "90\xa0[UT\x09\x00\x03\x01\x10^_\xfc\x0f^_", 21, 1600000001, 1599999996, 1, 12345, 23456},  // Ux UTma
    {0x50cf645c, "ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00UT\x09\x00\x03\x01\x10^_\xfc\x0f^_", 28, 1600000001, 1599999996, 1, 12345, 23456},  // ux UTma
    {0x50cf645c, "UT\x09\x00\x03\x01\x10^_\xfc\x0f^_ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00UX\x08\x00\x01\x00\x00\x00\x02\x00\x00\x00", 40, 1600000001, 1599999996, 1, 12345, 23456},  // UTma ux UX
    {0x50cf645c, "UX\x08\x00\x19\x0c^_\x01\x10^_ux\x0b\x00\x01\x04" "90\x00\x00\x04\xa0[\x00\x00UT\x05\x00\x01\x08\x10^_", 36, 1600000008, 1600000008, 1, 12345, 23456},  // UX ux UT
    {0x50cf645c, "UX\x0a\x00\x19\x0c^_\x01\x10^_90", 14, 1600000001, 1599999001, 0, 0, 0},  // UX 10 bytes
    {0x50cf645c, "UX\x0e\x00\x19\x0c^_\x01\x10^_90\xa0[\x07\x00", 18, 1600000001, 1599999001, 1, 12345, 23456},  // UX 14 bytes
};

// Listed dates: (DOS time, central extra) -> date, recorded with TZ=UTC
static struct {
    u32         dostime;
    char const *cextra;
    iz          cxlen;
    char const *date;
} recorded_dates[] = {
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // dos plain
    {0x50cf645d, "", 0, "2020-06-15 12:34"},  // dos odd
    {0x00210000, "", 0, "1980-01-01 00:00"},  // dos min
    {0xff9fbf7d, "", 0, "2107-12-31 23:59"},  // dos max
    {0x500f6000, "", 0, "2020-00-15 12:00"},  // dos month 0
    {0x51af6000, "", 0, "2020-13-15 12:00"},  // dos month 13
    {0x51ff6000, "", 0, "2020-15-31 12:00"},  // dos month 15
    {0x50406000, "", 0, "2020-02-00 12:00"},  // dos day 0
    {0x505f6000, "", 0, "2020-02-31 12:00"},  // dos feb 31
    {0x525d6000, "", 0, "2021-02-29 12:00"},  // dos feb 29 nonleap
    {0x50cfc000, "", 0, "2020-06-15 24:00"},  // dos hour 24
    {0x50cfffff, "", 0, "2020-06-15 31:63"},  // dos all max
    {0x50cf679e, "", 0, "2020-06-15 12:60"},  // dos min 60 sec 60
    {0x743319c4, "", 0, "2038-01-19 03:14"},  // dos 2038
    {0xf0610000, "", 0, "2100-03-01 00:00"},  // dos 2100
    {0x285d0000, "", 0, "2000-02-29 00:00"},  // dos 2000 leap
    {0x00000000, "", 0, "1980-00-00 00:00"},  // dos zero
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT m
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT ma
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT mac
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT a only
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT ma truncated
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT m no data
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT none
    {0x50cf645c, "UT\x05\x00\x01\x01\x10^_", 9, "2020-09-13 12:26"},  // UT central only
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT twice
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT high, dos 2020
    {0x74320000, "", 0, "2038-01-18 00:00"},  // UT high, dos 2038-01-18
    {0x7431bf7d, "", 0, "2038-01-17 23:59"},  // UT high, dos 2038-01-17
    {0x74320000, "", 0, "2038-01-18 00:00"},  // UT m high a low, 2038
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT m low a high, dos 2020
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT ux
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // ux UT
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UX
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UX full
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UX UT
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT UX
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // Ux UX
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UX Ux
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // ux UX
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UX ux
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UX a high
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UX m high
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UX short
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT cut (bad length)
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT then junk
    {0x50cf645c, "", 0, "2020-06-15 12:34"},  // UT m long
};

// Displayed names, as both list them
static struct {
    char const *name;
    char const *shown;
} recorded_shown[] = {
    {"a.txt", "a.txt"},
    {"dir/a.txt", "dir/a.txt"},
    {"/abs/a.txt", "/abs/a.txt"},
    {"//abs2/a.txt", "//abs2/a.txt"},
    {"../up.txt", "../up.txt"},
    {"a/../b.txt", "a/../b.txt"},
    {"a/./b.txt", "a/./b.txt"},
    {"./c.txt", "./c.txt"},
    {"a//d.txt", "a//d.txt"},
    {"..", ".."},
    {".", "."},
    {"x/..", "x/.."},
    {"x/.", "x/."},
    {"dots/...", "dots/..."},
    {"f;1", "f;1"},
    {"f;", "f;"},
    {"f;12a", "f;12a"},
    {"d;1/f;2", "d;1/f;2"},
    {"a;1;2", "a;1;2"},
    {"ctl\x01x", "ctl^Ax"},
    {"tab\x09x", "tab^Ix"},
    {"esc\x1b[1mx", "esc^[[1mx"},
    {"sp ace", "sp ace"},
    {"back\\slash", "back\\slash"},
    {"C:/x.txt", "C:/x.txt"},
    {"con.txt", "con.txt"},
    {"a:b", "a:b"},
    {"q?*<>|\".txt", "q?*<>|\".txt"},
    {"trail.", "trail."},
    {"trail ", "trail "},
    {"../../../etc/z", "../../../etc/z"},
    {"/../q", "/../q"},
    {"a/b/c/d.txt", "a/b/c/d.txt"},
    {"x/;1", "x/;1"},
    {"..;1", "..;1"},
    {".;2", ".;2"},
    {"...", "..."},
    {"a/.../b", "a/.../b"},
    {"hi\xc3\xa9", "hi\xc3\xa9"},
    {"dir/", "dir/"},
    {"../dir2/", "../dir2/"},
    {"/absdir/", "/absdir/"},
    {"a;1/", "a;1/"},
    {"./", "./"},
    {"/", "/"},
    {"x/../y/", "x/../y/"},
    {"deep/er/", "deep/er/"},
    {"win\\dir\\f.txt", "win\\dir\\f.txt"},
    {"mixed/dir\\f.txt", "mixed/dir\\f.txt"},
    {"\\lead\\f.txt", "\\lead\\f.txt"},
    {"C:\\x\\y.txt", "C:\\x\\y.txt"},
    {"..\\up\\f.txt", "..\\up\\f.txt"},
    {"dirb\\", "dirb\\"},
    {"ntfs\\dir\\f.txt", "ntfs\\dir\\f.txt"},
    {"hpfs\\dir\\f.txt", "hpfs\\dir\\f.txt"},
};

static void test_recorded_names(arena scratch)
{
    for (i32 i = 0; i < countof(recorded_names); i++) {
        arena a = scratch;
        uzpath r = uz_mapname(str(recorded_names[i].name),
                              recorded_names[i].made,
                              recorded_names[i].opts, &a);
        TESTROW(equals(r.path, recorded_names[i].path), i);
        TESTROW(r.flags == recorded_names[i].flags, i);
    }
}

// The names in UnZip's messages, as recorded for these
static void test_message_names(arena a)
{
    uzpath r = uz_mapname(str("/../q"), 0x031e, 0, &a);
    TEST(equals(r.full, "/../q"));  // stripped absolute path spec from
    TEST(equals(r.name, "../q"));   // skipped "../" path component(s) in
    r = uz_mapname(str("\\lead\\f.txt"), 0x001e, 0, &a);
    TEST(equals(r.full, "/lead/f.txt"));
    TEST(equals(r.name, "lead/f.txt"));
    r = uz_mapname(str("x/;1"), 0x031e, UZ_JUNK, &a);
    TEST(equals(r.name, "x/;1"));   // conversion of x/;1 failed
    r = uz_mapname(str("/"), 0x031e, 0, &a);
    TEST(equals(r.full, "/") && equals(r.name, ""));
    TEST(r.flags == (UZ_ABSOLUTE | UZ_FAILED));
}

// Windows names, by win32/win32.c's mapname and tugz's trailing rule
static void test_windows_names(arena scratch)
{
    static struct {
        char const *name;
        u32         made;
        i32         opts;
        char const *path;
        i32         flags;
    } t[] = {
        {"a.txt", 0x031e, 0, "a.txt", 0},
        {"a:b", 0x031e, 0, "a_b", 0},
        {"f.txt:stream:$DATA", 0x031e, 0, "f.txt_stream_$DATA", 0},
        {"q?*<>|\".txt", 0x031e, 0, "q______.txt", 0},
        {"back\\slash", 0x031e, 0, "back_slash", 0},
        {"ntfs\\dir\\f", 0x0b1e, 0, "ntfs_dir_f", 0},
        {"C:/x.txt", 0x031e, 0, "C_/x.txt", 0},
        {"C:\\x\\y.txt", 0x001e, 0, "C_/x/y.txt", UZ_BACKSLASH},
        {"\\\\srv\\share\\f", 0x001e, 0, "srv/share/f", UZ_BACKSLASH|UZ_ABSOLUTE},
        {"/abs/a", 0x031e, 0, "abs/a", UZ_ABSOLUTE},
        {"../up/f", 0x031e, 0, "up/f", UZ_DOTDOT},
        {"ctl\x01x", 0x031e, 0, "ctlx", 0},
        {"del\x7fx", 0x031e, 0, "del\x7fx", 0},
        {"hi\xc3\xa9\xff", 0x031e, 0, "hi\xc3\xa9\xff", 0},
        {"sp ace", 0x031e, 0, "sp ace", 0},
        {"f;1", 0x031e, 0, "f", 0},
        {"f;1", 0x031e, UZ_KEEPVER, "f;1", 0},
        {"d/f", 0x031e, UZ_JUNK, "f", 0},
        // Devices, with any extension, in any case
        {"con", 0x031e, 0, "_con", 0},
        {"CON.txt", 0x031e, 0, "_CON.txt", 0},
        {"nul.tar.gz", 0x031e, 0, "_nul.tar.gz", 0},
        {"Aux", 0x031e, 0, "_Aux", 0},
        {"prn.", 0x031e, 0, "_prn", 0},
        {"CON .txt", 0x031e, 0, "_CON .txt", 0},
        {"CON ", 0x031e, 0, "_CON", 0},
        {"COM0", 0x031e, 0, "_COM0", 0},
        {"com1.log", 0x031e, 0, "_com1.log", 0},
        {"LPT9", 0x031e, 0, "_LPT9", 0},
        {"COM\xc2\xb9", 0x031e, 0, "_COM\xc2\xb9", 0},
        {"lpt\xc2\xb3.x", 0x031e, 0, "_lpt\xc2\xb3.x", 0},
        {"COM\xc2\xb4", 0x031e, 0, "COM\xc2\xb4", 0},
        {"CONIN$", 0x031e, 0, "_CONIN$", 0},
        {"conout$.txt", 0x031e, 0, "_conout$.txt", 0},
        {"CLOCK$", 0x031e, 0, "_CLOCK$", 0},
        {"COM10", 0x031e, 0, "COM10", 0},
        {"COM", 0x031e, 0, "COM", 0},
        {"CONSOLE", 0x031e, 0, "CONSOLE", 0},
        {"xcon", 0x031e, 0, "xcon", 0},
        {"con:x", 0x031e, 0, "con_x", 0},
        {"dir/aux/f", 0x031e, 0, "dir/_aux/f", 0},
        {"nul/", 0x031e, 0, "_nul", UZ_DIR},
        // Trailing dots and spaces
        {"trail.", 0x031e, 0, "trail", 0},
        {"trail ", 0x031e, 0, "trail", 0},
        {"a. . /b", 0x031e, 0, "a/b", 0},
        {"a/.../b", 0x031e, 0, "a/b", 0},
        {"d./", 0x031e, 0, "d", UZ_DIR},
        {"...", 0x031e, 0, "", UZ_FAILED},
        {".", 0x031e, 0, "", UZ_FAILED},
        {"..", 0x031e, 0, "", UZ_FAILED},
        {"x/. .", 0x031e, 0, "", UZ_FAILED},
        {".hidden", 0x031e, 0, ".hidden", 0},
    };
    for (i32 i = 0; i < countof(t); i++) {
        arena a = scratch;
        uzpath r = uz_mapname(str(t[i].name), t[i].made,
                              t[i].opts|UZ_WINDOWS, &a);
        TESTROW(equals(r.path, t[i].path), i);
        TESTROW(r.flags == t[i].flags, i);
    }
}

// Every name maps to a path within the destination: of nonempty
// components, not "." or "..", without control characters, and on
// Windows without characters it reserves, trailing dots or spaces, or
// device names.
static void check_path(uzpath r, i32 opts)
{
    b32 win = opts & UZ_WINDOWS;
    s8  p   = r.path;
    TEST(!(r.flags & UZ_FAILED) || !p.len);
    TEST(r.flags & (UZ_FAILED|UZ_DIR) || p.len);
    for (iz beg = 0, i = 0; i <= p.len; i++) {
        if (i<p.len && p.s[i]!='/') {
            u8 c = p.s[i];
            TEST(c >= 0x20);
            TEST(win || (c!=0x7f && c!=0xff));
            TEST(!win || !strchr(":\\<>|\"?*", c));
            continue;
        }
        s8 c = {p.s+beg, i-beg};
        TEST(!p.len || c.len);
        TEST(!(c.len==1 && c.s[0]=='.'));
        TEST(!(c.len==2 && c.s[0]=='.' && c.s[1]=='.'));
        if (win && c.len) {
            TEST(c.s[c.len-1]!='.' && c.s[c.len-1]!=' ');
            TEST(!uz_device(c.s, c.len));
        }
        TEST(!(opts & UZ_JUNK) || i==p.len);
        beg = i + 1;
    }
}

static void check_name(u8 *name, iz len, arena scratch)
{
    for (i32 opts = 0; opts < 8; opts++) {
        for (u32 made = 0x001e; made <= 0x031e; made += 0x0300) {
            arena a = scratch;
            check_path(uz_mapname((s8){name, len}, made, opts, &a), opts);
        }
    }
}

static void test_name_safety(arena scratch)
{
    static u8 const alpha[] = "a./\\;1 :CON\x01\x7f\xff?";
    i32 nalpha = countof(alpha) - 1;
    u8  buf[16];

    // Every name of up to four characters
    for (i32 len = 1; len <= 4; len++) {
        i32 count = 1;
        for (i32 i = 0; i < len; i++) {
            count *= nalpha;
        }
        for (i32 n = 0; n < count; n++) {
            for (i32 i = 0, k = n; i < len; i++, k /= nalpha) {
                buf[i] = alpha[k % nalpha];
            }
            check_name(buf, len, scratch);
        }
    }

    // Then longer ones at random
    u64 rng = 1;
    for (i32 n = 0; n < 100000; n++) {
        rng = rng*0x3243f6a8885a308d + 1;
        i32 len = 5 + (i32)(rng >> 61);
        for (i32 i = 0; i < len; i++) {
            rng = rng*0x3243f6a8885a308d + 1;
            buf[i] = alpha[(rng >> 33) % (u64)nalpha];
        }
        check_name(buf, len, scratch);
    }
}

static void test_dosdate(void)
{
    i32 tm[6];
    uz_dosdate(0x50cf645c, tm);
    TEST(tm[0]==2020 && tm[1]==6 && tm[2]==15);
    TEST(tm[3]==12 && tm[4]==34 && tm[5]==56);
    uz_dosdate(0xffffffff, tm);
    TEST(tm[0]==2107 && tm[1]==15 && tm[2]==31);
    TEST(tm[3]==31 && tm[4]==63 && tm[5]==62);
    uz_dosdate(0, tm);
    TEST(tm[0]==1980 && !tm[1] && !tm[2] && !tm[3] && !tm[4] && !tm[5]);
}

static void test_timegm(void)
{
    // The inverse of zip_gmtime, across eras and leap days
    i32 tm[6];
    for (i64 t = -62167219200; t < 253402300800; t += 7777777) {
        zip_gmtime(t, tm);
        TEST(uz_timegm(tm) == t);
    }
    for (i64 t = 951782400-86400; t < 951782400+2*86400; t += 3599) {
        zip_gmtime(t, tm);
        TEST(uz_timegm(tm) == t);
    }

    // Fields out of range carry over
    i32 dec[6] = {2020, 0, 15, 12, 0, 0};  // month 0: December before
    TEST(uz_timegm(dec) == 1576411200);
    i32 feb[6] = {2020, 14, 1, 0, 0, 0};   // month 14: next February
    TEST(uz_timegm(feb) == 1612137600);
    i32 neg[6] = {2020, -11, 1, 0, 0, 0};  // a year before
    TEST(uz_timegm(neg) == 1546300800);
    i32 big[6] = {2020, 1, 1, 0, 0, 3600}; // seconds into the hour
    TEST(uz_timegm(big) == 1577840400);
}

// Extraction times and owners from local fields, else the DOS time
static void test_recorded_times(void)
{
    for (i32 i = 0; i < countof(recorded_times); i++) {
        s8  x = {(u8 *)recorded_times[i].lextra, recorded_times[i].lxlen};
        u32 d = recorded_times[i].dostime;
        uzizux z = uz_extra_izux(x, 0, d);
        i32 tm[6];
        uz_dosdate(d, tm);
        i64 m = z.flags&UZ_MTIME ? z.mtime : uz_timegm(tm);
        i64 a = z.flags&UZ_ATIME ? z.atime : m;
        TESTROW(m == recorded_times[i].mtime, i);
        TESTROW(a == recorded_times[i].atime, i);
        b32 owner = !!(z.flags & UZ_OWNER);
        TESTROW(recorded_times[i].owner<0 || owner==recorded_times[i].owner, i);
        TESTROW(recorded_times[i].owner<1 || z.uid==recorded_times[i].uid, i);
        TESTROW(recorded_times[i].owner<1 || z.gid==recorded_times[i].gid, i);
    }
}

// Listed dates, from a central "UT" field if it has a modification
// time, as UTC here, else the DOS fields as they are
static void test_recorded_dates(void)
{
    for (i32 i = 0; i < countof(recorded_dates); i++) {
        s8  x = {(u8 *)recorded_dates[i].cextra, recorded_dates[i].cxlen};
        u32 d = recorded_dates[i].dostime;
        uzizux z = uz_extra_izux(x, 1, d);
        i32 tm[6];
        if (z.flags & UZ_MTIME) {
            zip_gmtime(z.mtime, tm);
        } else {
            uz_dosdate(d, tm);
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                 tm[0], tm[1], tm[2], tm[3], tm[4]);
        TESTROW(!strcmp(buf, recorded_dates[i].date), i);
    }
}

static void test_extra_izux(void)
{
    // A central "UT" holds only the modification time, whatever its
    // flags announce
    s8 ut = S("UT\x05\x00\x07\x01\x10^_");
    uzizux z = uz_extra_izux(ut, 1, 0);
    TEST(z.flags==UZ_MTIME && z.mtime==1600000001);
    z = uz_extra_izux(ut, 0, 0);  // locally, the others are missing
    TEST(z.flags==UZ_MTIME && z.mtime==1600000001);
    s8 ma = S("UT\x09\x00\x03\x01\x10^_\x19\x0c^_");
    z = uz_extra_izux(ma, 1, 0);
    TEST(z.flags == UZ_MTIME);

    // A "ux" whose GID would run past it has none (Debian's reads on)
    s8 past = S("ux\x07\x00\x01\x04\x39\x30\x00\x00\x02" "ab");
    z = uz_extra_izux(past, 0, 0);
    TEST(!(z.flags & UZ_OWNER));
    s8 fits = S("ux\x09\x00\x01\x04\x39\x30\x00\x00\x02\xa0\x5b");
    z = uz_extra_izux(fits, 0, 0);
    TEST(z.flags==UZ_OWNER && z.uid==12345 && z.gid==23456);
    s8 big = S("ux\x0b\x00\x01\x04\xff\xff\xff\xff\x04\xfe\xff\xff\xff");
    z = uz_extra_izux(big, 0, 0);
    TEST(z.flags==UZ_OWNER && z.uid==0xffffffff && z.gid==0xfffffffe);

    // PKWARE's Unix field reads as "UX"
    s8 pk = S("\x0d\x00\x0c\x00\x19\x0c^_\x01\x10^_\x39\x30\xa0\x5b");
    z = uz_extra_izux(pk, 0, 0);
    TEST(z.flags == (UZ_MTIME|UZ_ATIME|UZ_OWNER));
    TEST(z.mtime==1600000001 && z.atime==1599999001);
    TEST(z.uid==12345 && z.gid==23456);

    // Truncated or empty fields
    TEST(!uz_extra_izux((s8){0}, 0, 0).flags);
    TEST(!uz_extra_izux(S("UT"), 0, 0).flags);
    TEST(!uz_extra_izux(S("UT\x09\x00\x01"), 0, 0).flags);
}

static void test_recorded_modes(void)
{
    for (i32 i = 0; i < countof(recorded_modes); i++) {
        s8 x = {(u8 *)recorded_modes[i].cextra, recorded_modes[i].cxlen};
        uzmode m = uz_mode(recorded_modes[i].ext, recorded_modes[i].made,
                           str(recorded_modes[i].name), x,
                           recorded_modes[i].keep);
        TESTROW(m.symlink == recorded_modes[i].link, i);
        if (!m.symlink) {
            u32 perm = m.mode & 07777 & (m.umask ? ~027u : ~0u);
            TESTROW(perm == recorded_modes[i].perm, i);
        }
    }
}

static void test_modes(void)
{
    // File types are kept; set-ID and sticky bits only with -K
    uzmode m = uz_mode(0x41ed0010, 0x031e, S("d/"), (s8){0}, 0);
    TEST(m.mode==040755 && !m.umask && !m.symlink);
    m = uz_mode(0xa1ff0000, 0x031e, S("l"), (s8){0}, 0);
    TEST(m.mode==0120777 && m.symlink);
    m = uz_mode(0x8fff0000, 0x031e, S("f"), (s8){0}, 0);
    TEST(m.mode == 0100777);
    m = uz_mode(0x8fff0000, 0x031e, S("f"), (s8){0}, 1);
    TEST(m.mode == 0107777);

    // A name ending in '/' is a directory without the DOS bit
    m = uz_mode(0, 0x001e, S("d/"), (s8){0}, 0);
    TEST(m.mode==0777 && m.umask);
    m = uz_mode(0, 0x001e, S("d\\"), (s8){0}, 0);  // raw, as in UnZip
    TEST(m.mode==0666 && m.umask);

    // Hosts past UnZip's own count as its last
    m = uz_mode(0x81ed0000, 0xff1e, S("f"), (s8){0}, 0);
    TEST(m.mode==0666 && m.umask);

    // An extra field whose length overruns is left alone
    m = uz_mode(0, 0x031e, S("f"), S("nu\x10\x00\x00"), 0);
    TEST(m.mode==0 && !m.umask);
}

static void test_symlink_host(void)
{
    u32 yes[] = {2, 3, 5, 16, 30};
    u32 no[]  = {0, 1, 4, 6, 7, 10, 11, 12, 13, 14, 17, 18, 19, 31, 255};
    for (i32 i = 0; i < countof(yes); i++) {
        TEST(uz_symlink_host(yes[i]<<8 | 30));
    }
    for (i32 i = 0; i < countof(no); i++) {
        TEST(!uz_symlink_host(no[i]<<8 | 30));
    }
}

static void test_dosattr(void)
{
    TEST(uz_dosattr(0) == 0x20);
    TEST(uz_dosattr(0x01) == 0x21);
    TEST(uz_dosattr(0x27) == 0x27);
    TEST(uz_dosattr(0x10) == 0x10);
    TEST(uz_dosattr(0x11) == 0x11);
    TEST(uz_dosattr(0x81ed0010) == 0x10);
    TEST(uz_dosattr(0x81a40000) == 0x20);
    TEST(uz_dosattr(0xff) == 0x7f);
    TEST(uz_dosattr(0x80) == 0x20);
}

static void test_filter(arena scratch)
{
    for (i32 i = 0; i < countof(recorded_shown); i++) {
        arena a = scratch;
        s8 got = uz_filter(str(recorded_shown[i].name), &a);
        TESTROW(equals(got, recorded_shown[i].shown), i);
    }

    // Where the two differ, or show bytes as they are: DEL as macOS's,
    // and the rest as UTF-8
    static struct {
        char const *name;
        char const *shown;
    } t[] = {
        {"del\x7fx", "del?x"},
        {"us\x1fx", "us^_x"},
        {"c1\xc2\x85x", "c1?x"},
        {"c1\xc2\x9f\xc2\xa0", "c1?\xc2\xa0"},
        {"\xff\xfe.bin", "??.bin"},
        {"lat\xe9", "lat?"},
        {"z\xc0\x80", "z??"},
        {"sur\xed\xa0\x80", "sur???"},
        {"cut\xe2\x82", "cut??"},
        {"\xe2\x82\xac \xf0\x9f\x98\x80", "\xe2\x82\xac \xf0\x9f\x98\x80"},
    };
    for (i32 i = 0; i < countof(t); i++) {
        arena a = scratch;
        s8 got = uz_filter(str(t[i].name), &a);
        TESTROW(equals(got, t[i].shown), i);
    }
    s8 nul = uz_filter(S("nul\x00x"), &scratch);
    TEST(equals(nul, "nul^@x"));
}

// Options: (words before the archive) -> (status, format, header,
// totals, comment) without members, then with one, recorded
static struct {
    char const *words;
    i32         got[2][5];
} recorded_options[] = {
    {"", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-1", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-2", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-s", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-m", {{0, 4, 1, 1, 0}, {0, 4, 0, 0, 0}}},
    {"-l", {{0, 5, 1, 1, 0}, {0, 5, 0, 0, 0}}},
    {"-v", {{0, 10, 1, 0, 1}, {0, 10, 1, 0, 1}}},
    {"-h", {{0, 0, 1, 0, 0}, {0, 3, 1, 0, 0}}},
    {"-t", {{0, 0, 0, 1, 0}, {0, 3, 0, 1, 0}}},
    {"-z", {{0, 3, 1, 1, 1}, {0, 3, 0, 0, 1}}},
    {"-T", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-C", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-Z", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"--1", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"--2", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"--s", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"--m", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"--l", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"--v", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"--h", {{0, 3, 0, 1, 0}, {0, 3, 0, 0, 0}}},
    {"--t", {{0, 3, 1, 0, 0}, {0, 3, 0, 0, 0}}},
    {"--z", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-1h", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-1t", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-1z", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-1htz", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-2h", {{0, 1, 1, 0, 0}, {0, 1, 1, 0, 0}}},
    {"-2t", {{0, 1, 0, 1, 0}, {0, 1, 0, 1, 0}}},
    {"-2z", {{0, 1, 0, 0, 1}, {0, 1, 0, 0, 1}}},
    {"-2ht", {{0, 1, 1, 1, 0}, {0, 1, 1, 1, 0}}},
    {"-2htz", {{0, 1, 1, 1, 1}, {0, 1, 1, 1, 1}}},
    {"-sh", {{0, 3, 1, 1, 0}, {0, 3, 1, 0, 0}}},
    {"-st", {{0, 3, 1, 1, 0}, {0, 3, 0, 1, 0}}},
    {"-sz", {{0, 3, 1, 1, 1}, {0, 3, 0, 0, 1}}},
    {"-s--h", {{0, 3, 0, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-s--t", {{0, 3, 1, 0, 0}, {0, 3, 0, 0, 0}}},
    {"-m--h", {{0, 4, 0, 1, 0}, {0, 4, 0, 0, 0}}},
    {"-l--t", {{0, 5, 1, 0, 0}, {0, 5, 0, 0, 0}}},
    {"-m-h", {{0, 4, 0, 1, 0}, {0, 4, 0, 0, 0}}},
    {"-h-t", {{0, 0, 1, 0, 0}, {0, 3, 1, 0, 0}}},
    {"-ht", {{0, 0, 1, 1, 0}, {0, 3, 1, 1, 0}}},
    {"-hz", {{0, 0, 1, 0, 1}, {0, 3, 1, 0, 1}}},
    {"-tz", {{0, 0, 0, 1, 1}, {0, 3, 0, 1, 1}}},
    {"-htz", {{0, 0, 1, 1, 1}, {0, 3, 1, 1, 1}}},
    {"-h--t", {{0, 0, 1, 0, 0}, {0, 3, 1, 0, 0}}},
    {"--h-t", {{0, 3, 0, 0, 0}, {0, 3, 0, 0, 0}}},
    {"-l-t", {{0, 5, 1, 0, 0}, {0, 5, 0, 0, 0}}},
    {"--tl", {{0, 5, 1, 0, 0}, {0, 5, 0, 0, 0}}},
    {"--hl", {{0, 5, 0, 1, 0}, {0, 5, 0, 0, 0}}},
    {"-Z1", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-Zl", {{0, 5, 1, 1, 0}, {0, 5, 0, 0, 0}}},
    {"-1Z", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-z1", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-1-z", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-l -", {{0, 5, 1, 1, 0}, {0, 5, 0, 0, 0}}},
    {"- -h", {{0, 0, 1, 0, 0}, {0, 3, 1, 0, 0}}},
    {"-- -h", {{0, 3, 0, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-h --", {{0, 0, 1, 0, 0}, {0, 3, 1, 0, 0}}},
    {"-h- -t", {{0, 0, 1, 0, 0}, {0, 3, 1, 0, 0}}},
    {"-t- -h", {{0, 0, 0, 1, 0}, {0, 3, 0, 1, 0}}},
    {"-l- -t", {{0, 5, 1, 0, 0}, {0, 5, 0, 0, 0}}},
    {"-l- -h", {{0, 5, 0, 1, 0}, {0, 5, 0, 0, 0}}},
    {"-v -t", {{0, 10, 1, 1, 1}, {0, 10, 1, 1, 1}}},
    {"-v --h", {{0, 10, 0, 0, 1}, {0, 10, 0, 0, 1}}},
    {"-vt", {{0, 10, 1, 1, 1}, {0, 10, 1, 1, 1}}},
    {"-vh", {{0, 10, 1, 0, 1}, {0, 10, 1, 0, 1}}},
    {"-v--t", {{0, 10, 1, 0, 1}, {0, 10, 1, 0, 1}}},
    {"-1 -h", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-h -1", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-h -s", {{0, 3, 1, 1, 0}, {0, 3, 1, 0, 0}}},
    {"-t -2", {{0, 1, 0, 1, 0}, {0, 1, 0, 1, 0}}},
    {"-2 -h -t", {{0, 1, 1, 1, 0}, {0, 1, 1, 1, 0}}},
    {"-l -1", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"-1 -l", {{0, 5, 1, 1, 0}, {0, 5, 0, 0, 0}}},
    {"-s -1-", {{0, 1, 0, 0, 0}, {0, 1, 0, 0, 0}}},
    {"--- -h", {{0, 3, 0, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-hZ-t", {{0, 0, 1, 0, 0}, {0, 3, 1, 0, 0}}},
    {"-Z-h", {{0, 3, 0, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-zZ", {{0, 3, 1, 1, 1}, {0, 3, 0, 0, 1}}},
    {"-T--", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
    {"-q", {{10, -1, 0, 0, 0}, {10, -1, 0, 0, 0}}},
    {"-x", {{10, -1, 0, 0, 0}, {10, -1, 0, 0, 0}}},
    {"-hq", {{10, -1, 0, 0, 0}, {10, -1, 0, 0, 0}}},
    {"-a", {{10, -1, 0, 0, 0}, {10, -1, 0, 0, 0}}},
    {"-1q", {{10, -1, 0, 0, 0}, {10, -1, 0, 0, 0}}},
    {"-? -l", {{10, -1, 0, 0, 0}, {10, -1, 0, 0, 0}}},
    {"-d", {{10, -1, 0, 0, 0}, {10, -1, 0, 0, 0}}},
    {"-hh", {{0, 0, 1, 0, 0}, {0, 3, 1, 0, 0}}},
    {"-tt", {{0, 0, 0, 1, 0}, {0, 3, 0, 1, 0}}},
    {"-1-1", {{0, 3, 1, 1, 0}, {0, 3, 0, 0, 0}}},
};

// Program names (POSIX): whether ZipInfo mode, recorded
static struct {
    char const *name;
    b32         zipinfo;
} recorded_programs[] = {
    {"zipinfo", 1},
    {"ZIPINFO", 1},
    {"ZipInfo.exe", 1},
    {"zipinfo-old", 1},
    {"zipinfo2", 1},
    {"ii", 1},
    {"II", 1},
    {"iI", 1},
    {"iii", 1},
    {"ii.exe", 1},
    {"i", 0},
    {"zipinf", 0},
    {"xzipinfo", 0},
    {"unzip", 0},
    {"zip", 0},
    {"info", 0},
    {"dir/zipinfo", 1},
    {"zipinfo.d/unzip", 0},
    {"a\\zipinfo", 0},
    {"a\\ii", 0},
    {"C:zipinfo", 0},
    {"ZIP\\ii.exe", 0},
};

// Attributes: (made, external attributes, name) -> as -s shows them,
// recorded (names were prefixed with a directory of their own)
static struct {
    u32         made;
    u32         extattr;
    char const *name;
    char const *attr;
} recorded_attribs[] = {
    {0x031e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x031e, 0x81ed0000, "f", "-rwxr-xr-x  3.0"},
    {0x031e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x031e, 0x89a40000, "f", "-rwSr--r--  3.0"},
    {0x031e, 0x85ed0000, "f", "-rwxr-sr-x  3.0"},
    {0x031e, 0x85a40000, "f", "-rw-r-Sr--  3.0"},
    {0x031e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x031e, 0x83b60000, "f", "-rw-rw-rwT  3.0"},
    {0x031e, 0x8fff0000, "f", "-rwsrwsrwt  3.0"},
    {0x031e, 0x8e000000, "f", "---S--S--T  3.0"},
    {0x031e, 0x80000000, "f", "----------  3.0"},
    {0x031e, 0x81ff0000, "f", "-rwxrwxrwx  3.0"},
    {0x031e, 0x81110000, "f", "-r---w---x  3.0"},
    {0x031e, 0x80540000, "f", "---x-w-r--  3.0"},
    {0x031e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x031e, 0x41ed0000, "f", "drwxr-xr-x  3.0"},
    {0x031e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x031e, 0x49a40000, "f", "drwSr--r--  3.0"},
    {0x031e, 0x45ed0000, "f", "drwxr-sr-x  3.0"},
    {0x031e, 0x45a40000, "f", "drw-r-Sr--  3.0"},
    {0x031e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x031e, 0x43b60000, "f", "drw-rw-rwT  3.0"},
    {0x031e, 0x4fff0000, "f", "drwsrwsrwt  3.0"},
    {0x031e, 0x4e000000, "f", "d--S--S--T  3.0"},
    {0x031e, 0x40000000, "f", "d---------  3.0"},
    {0x031e, 0x41ff0000, "f", "drwxrwxrwx  3.0"},
    {0x031e, 0x41110000, "f", "dr---w---x  3.0"},
    {0x031e, 0x40540000, "f", "d--x-w-r--  3.0"},
    {0x031e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x031e, 0xa1ed0000, "f", "lrwxr-xr-x  3.0"},
    {0x031e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x031e, 0xa9a40000, "f", "lrwSr--r--  3.0"},
    {0x031e, 0xa5ed0000, "f", "lrwxr-sr-x  3.0"},
    {0x031e, 0xa5a40000, "f", "lrw-r-Sr--  3.0"},
    {0x031e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x031e, 0xa3b60000, "f", "lrw-rw-rwT  3.0"},
    {0x031e, 0xafff0000, "f", "lrwsrwsrwt  3.0"},
    {0x031e, 0xae000000, "f", "l--S--S--T  3.0"},
    {0x031e, 0xa0000000, "f", "l---------  3.0"},
    {0x031e, 0xa1ff0000, "f", "lrwxrwxrwx  3.0"},
    {0x031e, 0xa1110000, "f", "lr---w---x  3.0"},
    {0x031e, 0xa0540000, "f", "l--x-w-r--  3.0"},
    {0x031e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x031e, 0x61ed0000, "f", "brwxr-xr-x  3.0"},
    {0x031e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x031e, 0x69a40000, "f", "brwSr--r--  3.0"},
    {0x031e, 0x65ed0000, "f", "brwxr-sr-x  3.0"},
    {0x031e, 0x65a40000, "f", "brw-r-Sr--  3.0"},
    {0x031e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x031e, 0x63b60000, "f", "brw-rw-rwT  3.0"},
    {0x031e, 0x6fff0000, "f", "brwsrwsrwt  3.0"},
    {0x031e, 0x6e000000, "f", "b--S--S--T  3.0"},
    {0x031e, 0x60000000, "f", "b---------  3.0"},
    {0x031e, 0x61ff0000, "f", "brwxrwxrwx  3.0"},
    {0x031e, 0x61110000, "f", "br---w---x  3.0"},
    {0x031e, 0x60540000, "f", "b--x-w-r--  3.0"},
    {0x031e, 0x21a40000, "f", "crw-r--r--  3.0"},
    {0x031e, 0x21ed0000, "f", "crwxr-xr-x  3.0"},
    {0x031e, 0x29ed0000, "f", "crwsr-xr-x  3.0"},
    {0x031e, 0x29a40000, "f", "crwSr--r--  3.0"},
    {0x031e, 0x25ed0000, "f", "crwxr-sr-x  3.0"},
    {0x031e, 0x25a40000, "f", "crw-r-Sr--  3.0"},
    {0x031e, 0x23ff0000, "f", "crwxrwxrwt  3.0"},
    {0x031e, 0x23b60000, "f", "crw-rw-rwT  3.0"},
    {0x031e, 0x2fff0000, "f", "crwsrwsrwt  3.0"},
    {0x031e, 0x2e000000, "f", "c--S--S--T  3.0"},
    {0x031e, 0x20000000, "f", "c---------  3.0"},
    {0x031e, 0x21ff0000, "f", "crwxrwxrwx  3.0"},
    {0x031e, 0x21110000, "f", "cr---w---x  3.0"},
    {0x031e, 0x20540000, "f", "c--x-w-r--  3.0"},
    {0x031e, 0x11a40000, "f", "prw-r--r--  3.0"},
    {0x031e, 0x11ed0000, "f", "prwxr-xr-x  3.0"},
    {0x031e, 0x19ed0000, "f", "prwsr-xr-x  3.0"},
    {0x031e, 0x19a40000, "f", "prwSr--r--  3.0"},
    {0x031e, 0x15ed0000, "f", "prwxr-sr-x  3.0"},
    {0x031e, 0x15a40000, "f", "prw-r-Sr--  3.0"},
    {0x031e, 0x13ff0000, "f", "prwxrwxrwt  3.0"},
    {0x031e, 0x13b60000, "f", "prw-rw-rwT  3.0"},
    {0x031e, 0x1fff0000, "f", "prwsrwsrwt  3.0"},
    {0x031e, 0x1e000000, "f", "p--S--S--T  3.0"},
    {0x031e, 0x10000000, "f", "p---------  3.0"},
    {0x031e, 0x11ff0000, "f", "prwxrwxrwx  3.0"},
    {0x031e, 0x11110000, "f", "pr---w---x  3.0"},
    {0x031e, 0x10540000, "f", "p--x-w-r--  3.0"},
    {0x031e, 0xc1a40000, "f", "srw-r--r--  3.0"},
    {0x031e, 0xc1ed0000, "f", "srwxr-xr-x  3.0"},
    {0x031e, 0xc9ed0000, "f", "srwsr-xr-x  3.0"},
    {0x031e, 0xc9a40000, "f", "srwSr--r--  3.0"},
    {0x031e, 0xc5ed0000, "f", "srwxr-sr-x  3.0"},
    {0x031e, 0xc5a40000, "f", "srw-r-Sr--  3.0"},
    {0x031e, 0xc3ff0000, "f", "srwxrwxrwt  3.0"},
    {0x031e, 0xc3b60000, "f", "srw-rw-rwT  3.0"},
    {0x031e, 0xcfff0000, "f", "srwsrwsrwt  3.0"},
    {0x031e, 0xce000000, "f", "s--S--S--T  3.0"},
    {0x031e, 0xc0000000, "f", "s---------  3.0"},
    {0x031e, 0xc1ff0000, "f", "srwxrwxrwx  3.0"},
    {0x031e, 0xc1110000, "f", "sr---w---x  3.0"},
    {0x031e, 0xc0540000, "f", "s--x-w-r--  3.0"},
    {0x031e, 0x01a40000, "f", "?rw-r--r--  3.0"},
    {0x031e, 0x01ed0000, "f", "?rwxr-xr-x  3.0"},
    {0x031e, 0x09ed0000, "f", "?rwsr-xr-x  3.0"},
    {0x031e, 0x09a40000, "f", "?rwSr--r--  3.0"},
    {0x031e, 0x05ed0000, "f", "?rwxr-sr-x  3.0"},
    {0x031e, 0x05a40000, "f", "?rw-r-Sr--  3.0"},
    {0x031e, 0x03ff0000, "f", "?rwxrwxrwt  3.0"},
    {0x031e, 0x03b60000, "f", "?rw-rw-rwT  3.0"},
    {0x031e, 0x0fff0000, "f", "?rwsrwsrwt  3.0"},
    {0x031e, 0x0e000000, "f", "?--S--S--T  3.0"},
    {0x031e, 0x00000000, "f", "?---------  3.0"},
    {0x031e, 0x01ff0000, "f", "?rwxrwxrwx  3.0"},
    {0x031e, 0x01110000, "f", "?r---w---x  3.0"},
    {0x031e, 0x00540000, "f", "?--x-w-r--  3.0"},
    {0x031e, 0x31a40000, "f", "?rw-r--r--  3.0"},
    {0x031e, 0x31ed0000, "f", "?rwxr-xr-x  3.0"},
    {0x031e, 0x39ed0000, "f", "?rwsr-xr-x  3.0"},
    {0x031e, 0x39a40000, "f", "?rwSr--r--  3.0"},
    {0x031e, 0x35ed0000, "f", "?rwxr-sr-x  3.0"},
    {0x031e, 0x35a40000, "f", "?rw-r-Sr--  3.0"},
    {0x031e, 0x33ff0000, "f", "?rwxrwxrwt  3.0"},
    {0x031e, 0x33b60000, "f", "?rw-rw-rwT  3.0"},
    {0x031e, 0x3fff0000, "f", "?rwsrwsrwt  3.0"},
    {0x031e, 0x3e000000, "f", "?--S--S--T  3.0"},
    {0x031e, 0x30000000, "f", "?---------  3.0"},
    {0x031e, 0x31ff0000, "f", "?rwxrwxrwx  3.0"},
    {0x031e, 0x31110000, "f", "?r---w---x  3.0"},
    {0x031e, 0x30540000, "f", "?--x-w-r--  3.0"},
    {0x031e, 0xf1a40000, "f", "?rw-r--r--  3.0"},
    {0x031e, 0xf1ed0000, "f", "?rwxr-xr-x  3.0"},
    {0x031e, 0xf9ed0000, "f", "?rwsr-xr-x  3.0"},
    {0x031e, 0xf9a40000, "f", "?rwSr--r--  3.0"},
    {0x031e, 0xf5ed0000, "f", "?rwxr-sr-x  3.0"},
    {0x031e, 0xf5a40000, "f", "?rw-r-Sr--  3.0"},
    {0x031e, 0xf3ff0000, "f", "?rwxrwxrwt  3.0"},
    {0x031e, 0xf3b60000, "f", "?rw-rw-rwT  3.0"},
    {0x031e, 0xffff0000, "f", "?rwsrwsrwt  3.0"},
    {0x031e, 0xfe000000, "f", "?--S--S--T  3.0"},
    {0x031e, 0xf0000000, "f", "?---------  3.0"},
    {0x031e, 0xf1ff0000, "f", "?rwxrwxrwx  3.0"},
    {0x031e, 0xf1110000, "f", "?r---w---x  3.0"},
    {0x031e, 0xf0540000, "f", "?--x-w-r--  3.0"},
    {0x051e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x051e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x051e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x051e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x051e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x051e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x051e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x051e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x051e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x051e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x051e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x051e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x071e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x071e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x071e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x071e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x071e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x071e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x071e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x071e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x071e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x071e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x071e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x071e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x081e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x081e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x081e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x081e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x081e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x081e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x081e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x081e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x081e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x081e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x081e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x081e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x091e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x091e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x091e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x091e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x091e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x091e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x091e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x091e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x091e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x091e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x091e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x091e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x0a1e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x0a1e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x0a1e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x0a1e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x0a1e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x0a1e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x0a1e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x0a1e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x0a1e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x0a1e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x0a1e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x0a1e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x0c1e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x0c1e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x0c1e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x0c1e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x0c1e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x0c1e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x0c1e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x0c1e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x0c1e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x0c1e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x0c1e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x0c1e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x101e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x101e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x101e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x101e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x101e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x101e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x101e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x101e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x101e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x101e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x101e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x101e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x111e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x111e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x111e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x111e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x111e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x111e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x111e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x111e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x111e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x111e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x111e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x111e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x131e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x131e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x131e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x131e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x131e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x131e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x131e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x131e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x131e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x131e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x131e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x131e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x141e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x141e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x141e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x141e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x141e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x141e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x141e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x141e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x141e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x141e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x141e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x141e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x1d1e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x1d1e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x1d1e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x1d1e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x1d1e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x1d1e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x1d1e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x1d1e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x1d1e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x1d1e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x1d1e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x1d1e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x1e1e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x1e1e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x1e1e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x1e1e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x1e1e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x1e1e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x1e1e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x1e1e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x1e1e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x1e1e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x1e1e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x1e1e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x1f1e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0x1f1e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0x1f1e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0x1f1e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0x1f1e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0x1f1e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0x1f1e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0x1f1e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0x1f1e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0x1f1e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0x1f1e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0x1f1e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0xc81e, 0x81a40000, "f", "-rw-r--r--  3.0"},
    {0xc81e, 0x89ed0000, "f", "-rwsr-xr-x  3.0"},
    {0xc81e, 0x83ff0000, "f", "-rwxrwxrwt  3.0"},
    {0xc81e, 0x41a40000, "f", "drw-r--r--  3.0"},
    {0xc81e, 0x49ed0000, "f", "drwsr-xr-x  3.0"},
    {0xc81e, 0x43ff0000, "f", "drwxrwxrwt  3.0"},
    {0xc81e, 0xa1a40000, "f", "lrw-r--r--  3.0"},
    {0xc81e, 0xa9ed0000, "f", "lrwsr-xr-x  3.0"},
    {0xc81e, 0xa3ff0000, "f", "lrwxrwxrwt  3.0"},
    {0xc81e, 0x61a40000, "f", "brw-r--r--  3.0"},
    {0xc81e, 0x69ed0000, "f", "brwsr-xr-x  3.0"},
    {0xc81e, 0x63ff0000, "f", "brwxrwxrwt  3.0"},
    {0x0014, 0x00000000, "f", "-rw----     2.0"},
    {0x0014, 0x00000001, "f", "-r-----     2.0"},
    {0x0014, 0x00000002, "f", "-rw--h-     2.0"},
    {0x0014, 0x00000004, "f", "-rw---s     2.0"},
    {0x0014, 0x00000008, "f", "Vrw----     2.0"},
    {0x0014, 0x00000010, "f", "drwx---     2.0"},
    {0x0014, 0x00000020, "f", "-rw-a--     2.0"},
    {0x0014, 0x00000021, "f", "-r--a--     2.0"},
    {0x0014, 0x0000003f, "f", "Vr-xahs     2.0"},
    {0x0014, 0x00000011, "f", "dr-x---     2.0"},
    {0x0014, 0x00000018, "f", "Vrwx---     2.0"},
    {0x0014, 0x000000c0, "f", "-rw----     2.0"},
    {0x0014, 0x00000020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0x00000008, "a.exe", "Vrw----     2.0"},
    {0x0014, 0x00000020, "A.EXE", "-rwxa--     2.0"},
    {0x0014, 0x00000008, "A.EXE", "Vrw----     2.0"},
    {0x0014, 0x00000020, "b.com", "-rwxa--     2.0"},
    {0x0014, 0x00000008, "b.com", "Vrw----     2.0"},
    {0x0014, 0x00000020, "c.Btm", "-rwxa--     2.0"},
    {0x0014, 0x00000008, "c.Btm", "Vrw----     2.0"},
    {0x0014, 0x00000020, "d.cmd", "-rwxa--     2.0"},
    {0x0014, 0x00000008, "d.cmd", "Vrw----     2.0"},
    {0x0014, 0x00000020, "e.bat", "-rwxa--     2.0"},
    {0x0014, 0x00000008, "e.bat", "Vrw----     2.0"},
    {0x0014, 0x00000020, "f.batch", "-rwxa--     2.0"},
    {0x0014, 0x00000008, "f.batch", "Vrw----     2.0"},
    {0x0014, 0x00000020, "g.exe.txt", "-rw-a--     2.0"},
    {0x0014, 0x00000008, "g.exe.txt", "Vrw----     2.0"},
    {0x0014, 0x00000020, "h.ex", "-rw-a--     2.0"},
    {0x0014, 0x00000008, "h.ex", "Vrw----     2.0"},
    {0x0014, 0x00000020, "dir.exe/f", "-rwxa--     2.0"},
    {0x0014, 0x00000008, "dir.exe/f", "Vrw----     2.0"},
    {0x0014, 0x00000020, "noext", "-rw-a--     2.0"},
    {0x0014, 0x00000008, "noext", "Vrw----     2.0"},
    {0x0014, 0x00000020, "x.", "-rw-a--     2.0"},
    {0x0014, 0x00000008, "x.", "Vrw----     2.0"},
    {0x0414, 0x00000000, "f", "-rw----     2.0"},
    {0x0414, 0x00000001, "f", "-r-----     2.0"},
    {0x0414, 0x00000002, "f", "-rw--h-     2.0"},
    {0x0414, 0x00000004, "f", "-rw---s     2.0"},
    {0x0414, 0x00000008, "f", "Vrw----     2.0"},
    {0x0414, 0x00000010, "f", "drwx---     2.0"},
    {0x0414, 0x00000020, "f", "-rw-a--     2.0"},
    {0x0414, 0x00000021, "f", "-r--a--     2.0"},
    {0x0414, 0x0000003f, "f", "Vr-xahs     2.0"},
    {0x0414, 0x00000011, "f", "dr-x---     2.0"},
    {0x0414, 0x00000018, "f", "Vrwx---     2.0"},
    {0x0414, 0x000000c0, "f", "-rw----     2.0"},
    {0x0414, 0x00000020, "a.exe", "-rwxa--     2.0"},
    {0x0414, 0x00000008, "a.exe", "Vrw----     2.0"},
    {0x0414, 0x00000020, "A.EXE", "-rwxa--     2.0"},
    {0x0414, 0x00000008, "A.EXE", "Vrw----     2.0"},
    {0x0414, 0x00000020, "b.com", "-rwxa--     2.0"},
    {0x0414, 0x00000008, "b.com", "Vrw----     2.0"},
    {0x0414, 0x00000020, "c.Btm", "-rwxa--     2.0"},
    {0x0414, 0x00000008, "c.Btm", "Vrw----     2.0"},
    {0x0414, 0x00000020, "d.cmd", "-rwxa--     2.0"},
    {0x0414, 0x00000008, "d.cmd", "Vrw----     2.0"},
    {0x0414, 0x00000020, "e.bat", "-rwxa--     2.0"},
    {0x0414, 0x00000008, "e.bat", "Vrw----     2.0"},
    {0x0414, 0x00000020, "f.batch", "-rwxa--     2.0"},
    {0x0414, 0x00000008, "f.batch", "Vrw----     2.0"},
    {0x0414, 0x00000020, "g.exe.txt", "-rw-a--     2.0"},
    {0x0414, 0x00000008, "g.exe.txt", "Vrw----     2.0"},
    {0x0414, 0x00000020, "h.ex", "-rw-a--     2.0"},
    {0x0414, 0x00000008, "h.ex", "Vrw----     2.0"},
    {0x0414, 0x00000020, "dir.exe/f", "-rwxa--     2.0"},
    {0x0414, 0x00000008, "dir.exe/f", "Vrw----     2.0"},
    {0x0414, 0x00000020, "noext", "-rw-a--     2.0"},
    {0x0414, 0x00000008, "noext", "Vrw----     2.0"},
    {0x0414, 0x00000020, "x.", "-rw-a--     2.0"},
    {0x0414, 0x00000008, "x.", "Vrw----     2.0"},
    {0x0614, 0x00000000, "f", "-rw----     2.0"},
    {0x0614, 0x00000001, "f", "-r-----     2.0"},
    {0x0614, 0x00000002, "f", "-rw--h-     2.0"},
    {0x0614, 0x00000004, "f", "-rw---s     2.0"},
    {0x0614, 0x00000008, "f", "Vrw----     2.0"},
    {0x0614, 0x00000010, "f", "drwx---     2.0"},
    {0x0614, 0x00000020, "f", "-rw-a--     2.0"},
    {0x0614, 0x00000021, "f", "-r--a--     2.0"},
    {0x0614, 0x0000003f, "f", "Vr-xahs     2.0"},
    {0x0614, 0x00000011, "f", "dr-x---     2.0"},
    {0x0614, 0x00000018, "f", "Vrwx---     2.0"},
    {0x0614, 0x000000c0, "f", "-rw----     2.0"},
    {0x0614, 0x00000020, "a.exe", "-rwxa--     2.0"},
    {0x0614, 0x00000008, "a.exe", "Vrw----     2.0"},
    {0x0614, 0x00000020, "A.EXE", "-rwxa--     2.0"},
    {0x0614, 0x00000008, "A.EXE", "Vrw----     2.0"},
    {0x0614, 0x00000020, "b.com", "-rwxa--     2.0"},
    {0x0614, 0x00000008, "b.com", "Vrw----     2.0"},
    {0x0614, 0x00000020, "c.Btm", "-rwxa--     2.0"},
    {0x0614, 0x00000008, "c.Btm", "Vrw----     2.0"},
    {0x0614, 0x00000020, "d.cmd", "-rwxa--     2.0"},
    {0x0614, 0x00000008, "d.cmd", "Vrw----     2.0"},
    {0x0614, 0x00000020, "e.bat", "-rwxa--     2.0"},
    {0x0614, 0x00000008, "e.bat", "Vrw----     2.0"},
    {0x0614, 0x00000020, "f.batch", "-rwxa--     2.0"},
    {0x0614, 0x00000008, "f.batch", "Vrw----     2.0"},
    {0x0614, 0x00000020, "g.exe.txt", "-rw-a--     2.0"},
    {0x0614, 0x00000008, "g.exe.txt", "Vrw----     2.0"},
    {0x0614, 0x00000020, "h.ex", "-rw-a--     2.0"},
    {0x0614, 0x00000008, "h.ex", "Vrw----     2.0"},
    {0x0614, 0x00000020, "dir.exe/f", "-rwxa--     2.0"},
    {0x0614, 0x00000008, "dir.exe/f", "Vrw----     2.0"},
    {0x0614, 0x00000020, "noext", "-rw-a--     2.0"},
    {0x0614, 0x00000008, "noext", "Vrw----     2.0"},
    {0x0614, 0x00000020, "x.", "-rw-a--     2.0"},
    {0x0614, 0x00000008, "x.", "Vrw----     2.0"},
    {0x0b14, 0x00000000, "f", "-rw----     2.0"},
    {0x0b14, 0x00000001, "f", "-r-----     2.0"},
    {0x0b14, 0x00000002, "f", "-rw--h-     2.0"},
    {0x0b14, 0x00000004, "f", "-rw---s     2.0"},
    {0x0b14, 0x00000008, "f", "Vrw----     2.0"},
    {0x0b14, 0x00000010, "f", "drwx---     2.0"},
    {0x0b14, 0x00000020, "f", "-rw-a--     2.0"},
    {0x0b14, 0x00000021, "f", "-r--a--     2.0"},
    {0x0b14, 0x0000003f, "f", "Vr-xahs     2.0"},
    {0x0b14, 0x00000011, "f", "dr-x---     2.0"},
    {0x0b14, 0x00000018, "f", "Vrwx---     2.0"},
    {0x0b14, 0x000000c0, "f", "-rw----     2.0"},
    {0x0b14, 0x00000020, "a.exe", "-rwxa--     2.0"},
    {0x0b14, 0x00000008, "a.exe", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "A.EXE", "-rwxa--     2.0"},
    {0x0b14, 0x00000008, "A.EXE", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "b.com", "-rwxa--     2.0"},
    {0x0b14, 0x00000008, "b.com", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "c.Btm", "-rwxa--     2.0"},
    {0x0b14, 0x00000008, "c.Btm", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "d.cmd", "-rwxa--     2.0"},
    {0x0b14, 0x00000008, "d.cmd", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "e.bat", "-rwxa--     2.0"},
    {0x0b14, 0x00000008, "e.bat", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "f.batch", "-rwxa--     2.0"},
    {0x0b14, 0x00000008, "f.batch", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "g.exe.txt", "-rw-a--     2.0"},
    {0x0b14, 0x00000008, "g.exe.txt", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "h.ex", "-rw-a--     2.0"},
    {0x0b14, 0x00000008, "h.ex", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "dir.exe/f", "-rwxa--     2.0"},
    {0x0b14, 0x00000008, "dir.exe/f", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "noext", "-rw-a--     2.0"},
    {0x0b14, 0x00000008, "noext", "Vrw----     2.0"},
    {0x0b14, 0x00000020, "x.", "-rw-a--     2.0"},
    {0x0b14, 0x00000008, "x.", "Vrw----     2.0"},
    {0x0d14, 0x00000000, "f", "-rw----     2.0"},
    {0x0d14, 0x00000001, "f", "-r-----     2.0"},
    {0x0d14, 0x00000002, "f", "-rw--h-     2.0"},
    {0x0d14, 0x00000004, "f", "-rw---s     2.0"},
    {0x0d14, 0x00000008, "f", "Vrw----     2.0"},
    {0x0d14, 0x00000010, "f", "drwx---     2.0"},
    {0x0d14, 0x00000020, "f", "-rw-a--     2.0"},
    {0x0d14, 0x00000021, "f", "-r--a--     2.0"},
    {0x0d14, 0x0000003f, "f", "Vr-xahs     2.0"},
    {0x0d14, 0x00000011, "f", "dr-x---     2.0"},
    {0x0d14, 0x00000018, "f", "Vrwx---     2.0"},
    {0x0d14, 0x000000c0, "f", "-rw----     2.0"},
    {0x0d14, 0x00000020, "a.exe", "-rwxa--     2.0"},
    {0x0d14, 0x00000008, "a.exe", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "A.EXE", "-rwxa--     2.0"},
    {0x0d14, 0x00000008, "A.EXE", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "b.com", "-rwxa--     2.0"},
    {0x0d14, 0x00000008, "b.com", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "c.Btm", "-rwxa--     2.0"},
    {0x0d14, 0x00000008, "c.Btm", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "d.cmd", "-rwxa--     2.0"},
    {0x0d14, 0x00000008, "d.cmd", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "e.bat", "-rwxa--     2.0"},
    {0x0d14, 0x00000008, "e.bat", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "f.batch", "-rwxa--     2.0"},
    {0x0d14, 0x00000008, "f.batch", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "g.exe.txt", "-rw-a--     2.0"},
    {0x0d14, 0x00000008, "g.exe.txt", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "h.ex", "-rw-a--     2.0"},
    {0x0d14, 0x00000008, "h.ex", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "dir.exe/f", "-rwxa--     2.0"},
    {0x0d14, 0x00000008, "dir.exe/f", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "noext", "-rw-a--     2.0"},
    {0x0d14, 0x00000008, "noext", "Vrw----     2.0"},
    {0x0d14, 0x00000020, "x.", "-rw-a--     2.0"},
    {0x0d14, 0x00000008, "x.", "Vrw----     2.0"},
    {0x0e14, 0x00000000, "f", "-rw----     2.0"},
    {0x0e14, 0x00000001, "f", "-r-----     2.0"},
    {0x0e14, 0x00000002, "f", "-rw--h-     2.0"},
    {0x0e14, 0x00000004, "f", "-rw---s     2.0"},
    {0x0e14, 0x00000008, "f", "Vrw----     2.0"},
    {0x0e14, 0x00000010, "f", "drwx---     2.0"},
    {0x0e14, 0x00000020, "f", "-rw-a--     2.0"},
    {0x0e14, 0x00000021, "f", "-r--a--     2.0"},
    {0x0e14, 0x0000003f, "f", "Vr-xahs     2.0"},
    {0x0e14, 0x00000011, "f", "dr-x---     2.0"},
    {0x0e14, 0x00000018, "f", "Vrwx---     2.0"},
    {0x0e14, 0x000000c0, "f", "-rw----     2.0"},
    {0x0e14, 0x00000020, "a.exe", "-rwxa--     2.0"},
    {0x0e14, 0x00000008, "a.exe", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "A.EXE", "-rwxa--     2.0"},
    {0x0e14, 0x00000008, "A.EXE", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "b.com", "-rwxa--     2.0"},
    {0x0e14, 0x00000008, "b.com", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "c.Btm", "-rwxa--     2.0"},
    {0x0e14, 0x00000008, "c.Btm", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "d.cmd", "-rwxa--     2.0"},
    {0x0e14, 0x00000008, "d.cmd", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "e.bat", "-rwxa--     2.0"},
    {0x0e14, 0x00000008, "e.bat", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "f.batch", "-rwxa--     2.0"},
    {0x0e14, 0x00000008, "f.batch", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "g.exe.txt", "-rw-a--     2.0"},
    {0x0e14, 0x00000008, "g.exe.txt", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "h.ex", "-rw-a--     2.0"},
    {0x0e14, 0x00000008, "h.ex", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "dir.exe/f", "-rwxa--     2.0"},
    {0x0e14, 0x00000008, "dir.exe/f", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "noext", "-rw-a--     2.0"},
    {0x0e14, 0x00000008, "noext", "Vrw----     2.0"},
    {0x0e14, 0x00000020, "x.", "-rw-a--     2.0"},
    {0x0e14, 0x00000008, "x.", "Vrw----     2.0"},
    {0x0f14, 0x00000000, "f", "-rw----     2.0"},
    {0x0f14, 0x00000001, "f", "-r-----     2.0"},
    {0x0f14, 0x00000002, "f", "-rw--h-     2.0"},
    {0x0f14, 0x00000004, "f", "-rw---s     2.0"},
    {0x0f14, 0x00000008, "f", "Vrw----     2.0"},
    {0x0f14, 0x00000010, "f", "drwx---     2.0"},
    {0x0f14, 0x00000020, "f", "-rw-a--     2.0"},
    {0x0f14, 0x00000021, "f", "-r--a--     2.0"},
    {0x0f14, 0x0000003f, "f", "Vr-xahs     2.0"},
    {0x0f14, 0x00000011, "f", "dr-x---     2.0"},
    {0x0f14, 0x00000018, "f", "Vrwx---     2.0"},
    {0x0f14, 0x000000c0, "f", "-rw----     2.0"},
    {0x0f14, 0x00000020, "a.exe", "-rwxa--     2.0"},
    {0x0f14, 0x00000008, "a.exe", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "A.EXE", "-rwxa--     2.0"},
    {0x0f14, 0x00000008, "A.EXE", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "b.com", "-rwxa--     2.0"},
    {0x0f14, 0x00000008, "b.com", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "c.Btm", "-rwxa--     2.0"},
    {0x0f14, 0x00000008, "c.Btm", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "d.cmd", "-rwxa--     2.0"},
    {0x0f14, 0x00000008, "d.cmd", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "e.bat", "-rwxa--     2.0"},
    {0x0f14, 0x00000008, "e.bat", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "f.batch", "-rwxa--     2.0"},
    {0x0f14, 0x00000008, "f.batch", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "g.exe.txt", "-rw-a--     2.0"},
    {0x0f14, 0x00000008, "g.exe.txt", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "h.ex", "-rw-a--     2.0"},
    {0x0f14, 0x00000008, "h.ex", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "dir.exe/f", "-rwxa--     2.0"},
    {0x0f14, 0x00000008, "dir.exe/f", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "noext", "-rw-a--     2.0"},
    {0x0f14, 0x00000008, "noext", "Vrw----     2.0"},
    {0x0f14, 0x00000020, "x.", "-rw-a--     2.0"},
    {0x0f14, 0x00000008, "x.", "Vrw----     2.0"},
    {0x0014, 0x81a40000, "a.exe", "-rw-r--r--  2.0"},
    {0x0b14, 0x81a40000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0x81240000, "a.exe", "-rwx---     2.0"},
    {0x0b14, 0x81240000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0x41ed0000, "a.exe", "-rwx---     2.0"},
    {0x0b14, 0x41ed0000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0x416d0000, "a.exe", "-rwx---     2.0"},
    {0x0b14, 0x416d0000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0x81800000, "a.exe", "-rw-------  2.0"},
    {0x0b14, 0x81800000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0x81000000, "a.exe", "-rwx---     2.0"},
    {0x0b14, 0x81000000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0x41c00000, "a.exe", "-rwx---     2.0"},
    {0x0b14, 0x41c00000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0xa1ff0000, "a.exe", "-rwx---     2.0"},
    {0x0b14, 0xa1ff0000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0x80000000, "a.exe", "-rwx---     2.0"},
    {0x0b14, 0x80000000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0x80800000, "a.exe", "-rwx---     2.0"},
    {0x0b14, 0x80800000, "a.exe", "-rwx---     2.0"},
    {0x0014, 0x81a40001, "a.exe", "-r-x---     2.0"},
    {0x0b14, 0x81a40001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0x81240001, "a.exe", "-r--r--r--  2.0"},
    {0x0b14, 0x81240001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0x41ed0001, "a.exe", "-r-x---     2.0"},
    {0x0b14, 0x41ed0001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0x416d0001, "a.exe", "-r-x---     2.0"},
    {0x0b14, 0x416d0001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0x81800001, "a.exe", "-r-x---     2.0"},
    {0x0b14, 0x81800001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0x81000001, "a.exe", "-r--------  2.0"},
    {0x0b14, 0x81000001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0x41c00001, "a.exe", "-r-x---     2.0"},
    {0x0b14, 0x41c00001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0xa1ff0001, "a.exe", "-r-x---     2.0"},
    {0x0b14, 0xa1ff0001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0x80000001, "a.exe", "-r-x---     2.0"},
    {0x0b14, 0x80000001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0x80800001, "a.exe", "-r-x---     2.0"},
    {0x0b14, 0x80800001, "a.exe", "-r-x---     2.0"},
    {0x0014, 0x81a40010, "a.exe", "drwx---     2.0"},
    {0x0b14, 0x81a40010, "a.exe", "drwx---     2.0"},
    {0x0014, 0x81240010, "a.exe", "drwx---     2.0"},
    {0x0b14, 0x81240010, "a.exe", "drwx---     2.0"},
    {0x0014, 0x41ed0010, "a.exe", "drwxr-xr-x  2.0"},
    {0x0b14, 0x41ed0010, "a.exe", "drwx---     2.0"},
    {0x0014, 0x416d0010, "a.exe", "drwx---     2.0"},
    {0x0b14, 0x416d0010, "a.exe", "drwx---     2.0"},
    {0x0014, 0x81800010, "a.exe", "drwx---     2.0"},
    {0x0b14, 0x81800010, "a.exe", "drwx---     2.0"},
    {0x0014, 0x81000010, "a.exe", "drwx---     2.0"},
    {0x0b14, 0x81000010, "a.exe", "drwx---     2.0"},
    {0x0014, 0x41c00010, "a.exe", "drwx------  2.0"},
    {0x0b14, 0x41c00010, "a.exe", "drwx---     2.0"},
    {0x0014, 0xa1ff0010, "a.exe", "lrwxrwxrwx  2.0"},
    {0x0b14, 0xa1ff0010, "a.exe", "drwx---     2.0"},
    {0x0014, 0x80000010, "a.exe", "drwx---     2.0"},
    {0x0b14, 0x80000010, "a.exe", "drwx---     2.0"},
    {0x0014, 0x80800010, "a.exe", "drwx---     2.0"},
    {0x0b14, 0x80800010, "a.exe", "drwx---     2.0"},
    {0x0014, 0x81a40011, "a.exe", "dr-x---     2.0"},
    {0x0b14, 0x81a40011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0x81240011, "a.exe", "dr-x---     2.0"},
    {0x0b14, 0x81240011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0x41ed0011, "a.exe", "dr-x---     2.0"},
    {0x0b14, 0x41ed0011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0x416d0011, "a.exe", "dr-xr-xr-x  2.0"},
    {0x0b14, 0x416d0011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0x81800011, "a.exe", "dr-x---     2.0"},
    {0x0b14, 0x81800011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0x81000011, "a.exe", "dr-x---     2.0"},
    {0x0b14, 0x81000011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0x41c00011, "a.exe", "dr-x---     2.0"},
    {0x0b14, 0x41c00011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0xa1ff0011, "a.exe", "dr-x---     2.0"},
    {0x0b14, 0xa1ff0011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0x80000011, "a.exe", "dr-x---     2.0"},
    {0x0b14, 0x80000011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0x80800011, "a.exe", "dr-x---     2.0"},
    {0x0b14, 0x80800011, "a.exe", "dr-x---     2.0"},
    {0x0014, 0x81a40020, "a.exe", "-rw-r--r--  2.0"},
    {0x0b14, 0x81a40020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0x81240020, "a.exe", "-rwxa--     2.0"},
    {0x0b14, 0x81240020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0x41ed0020, "a.exe", "-rwxa--     2.0"},
    {0x0b14, 0x41ed0020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0x416d0020, "a.exe", "-rwxa--     2.0"},
    {0x0b14, 0x416d0020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0x81800020, "a.exe", "-rw-------  2.0"},
    {0x0b14, 0x81800020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0x81000020, "a.exe", "-rwxa--     2.0"},
    {0x0b14, 0x81000020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0x41c00020, "a.exe", "-rwxa--     2.0"},
    {0x0b14, 0x41c00020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0xa1ff0020, "a.exe", "-rwxa--     2.0"},
    {0x0b14, 0xa1ff0020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0x80000020, "a.exe", "-rwxa--     2.0"},
    {0x0b14, 0x80000020, "a.exe", "-rwxa--     2.0"},
    {0x0014, 0x80800020, "a.exe", "-rwxa--     2.0"},
    {0x0b14, 0x80800020, "a.exe", "-rwxa--     2.0"},
    {0x0214, 0x80000000, "f", ",,          2.0"},
    {0x0214, 0x81ff0000, "f", "RWED,RWED,RWED "},
    {0x0214, 0x81c00000, "f", "RWED,,      2.0"},
    {0x0214, 0x80380000, "f", ",RWED,      2.0"},
    {0x0214, 0x80070000, "f", ",,RWED      2.0"},
    {0x0214, 0x81240000, "f", "R,R,R       2.0"},
    {0x0214, 0x80920000, "f", "WD,WD,WD    2.0"},
    {0x0214, 0x80490000, "f", "E,E,E       2.0"},
    {0x0214, 0x81a00000, "f", "RWD,R,      2.0"},
    {0x0214, 0x81e90000, "f", "RWED,RE,E   2.0"},
    {0x0214, 0x89ed0000, "f", "RWED,RE,RE  2.0"},
    {0x0214, 0x81a20000, "f", "RWD,R,WD    2.0"},
    {0x0214, 0x80800000, "f", "WD,,        2.0"},
    {0x0214, 0x80100000, "f", ",WD,        2.0"},
    {0x0114, 0x00000000, "f", "?--------   2.0"},
    {0x0114, 0x08000000, "f", "d--------   2.0"},
    {0x0114, 0x04000000, "f", "---------   2.0"},
    {0x0114, 0x0c000000, "f", "?--------   2.0"},
    {0x0114, 0x04ff0000, "f", "-hsparwed   2.0"},
    {0x0114, 0x087f0000, "f", "d-sparwed   2.0"},
    {0x0114, 0x04800000, "f", "-h-------   2.0"},
    {0x0114, 0x04400000, "f", "--s------   2.0"},
    {0x0114, 0x04200000, "f", "---p-----   2.0"},
    {0x0114, 0x04100000, "f", "----a----   2.0"},
    {0x0114, 0x04080000, "f", "-----r---   2.0"},
    {0x0114, 0x04040000, "f", "------w--   2.0"},
    {0x0114, 0x04020000, "f", "-------e-   2.0"},
    {0x0114, 0x04010000, "f", "--------d   2.0"},
    {0x0114, 0x040f0000, "f", "-----rwed   2.0"},
    {0x1214, 0x0fff0000, "f", "?........   2.0"},
    {0x1214, 0x1fff0000, "f", "?........   2.0"},
    {0x1214, 0x2fff0000, "f", "C........   2.0"},
    {0x1214, 0x3fff0000, "f", "?........   2.0"},
    {0x1214, 0x4fff0000, "f", "D........   2.0"},
    {0x1214, 0x5fff0000, "f", "L........   2.0"},
    {0x1214, 0x6fff0000, "f", "?........   2.0"},
    {0x1214, 0x7fff0000, "f", "?........   2.0"},
    {0x1214, 0x80000000, "f", "SHMWREXWR   2.0"},
    {0x1214, 0x8fff0000, "f", "S........   2.0"},
    {0x1214, 0x84000000, "f", "S.MWREXWR   2.0"},
    {0x1214, 0x88000000, "f", "SH.WREXWR   2.0"},
    {0x1214, 0x80020000, "f", "SHM.REXWR   2.0"},
    {0x1214, 0x80040000, "f", "SHMW.EXWR   2.0"},
    {0x1214, 0x82000000, "f", "SHMWR.XWR   2.0"},
    {0x1214, 0x80400000, "f", "SHMWRE.WR   2.0"},
    {0x1214, 0x80800000, "f", "SHMWREX.R   2.0"},
    {0x1214, 0x81000000, "f", "SHMWREXW.   2.0"},
    {0x1214, 0x9fff0000, "f", "R........   2.0"},
    {0x1214, 0xafff0000, "f", "K........   2.0"},
    {0x1214, 0xbfff0000, "f", "I........   2.0"},
    {0x1214, 0xcfff0000, "f", "?........   2.0"},
    {0x1214, 0xdfff0000, "f", "P........   2.0"},
    {0x1214, 0xefff0000, "f", "2........   2.0"},
    {0x1214, 0xffff0000, "f", "3........   2.0"},
    {0x0300, 0x81a40000, "f", "-rw-r--r--  0.0"},
    {0x1300, 0x41ed0000, "f", "drwxr-xr-x  0.0"},
    {0x0000, 0x00000020, "f", "-rw-a--     0.0"},
    {0x0100, 0x04000000, "f", "---------   0.0"},
    {0x0200, 0x81ff0000, "f", "RWED,RWED,RWED "},
    {0x0200, 0x81240000, "f", "R,R,R       0.0"},
    {0x1200, 0x80000000, "f", "SHMWREXWR   0.0"},
    {0x0301, 0x81a40000, "f", "-rw-r--r--  0.1"},
    {0x1301, 0x41ed0000, "f", "drwxr-xr-x  0.1"},
    {0x0001, 0x00000020, "f", "-rw-a--     0.1"},
    {0x0101, 0x04000000, "f", "---------   0.1"},
    {0x0201, 0x81ff0000, "f", "RWED,RWED,RWED "},
    {0x0201, 0x81240000, "f", "R,R,R       0.1"},
    {0x1201, 0x80000000, "f", "SHMWREXWR   0.1"},
    {0x0309, 0x81a40000, "f", "-rw-r--r--  0.9"},
    {0x1309, 0x41ed0000, "f", "drwxr-xr-x  0.9"},
    {0x0009, 0x00000020, "f", "-rw-a--     0.9"},
    {0x0109, 0x04000000, "f", "---------   0.9"},
    {0x0209, 0x81ff0000, "f", "RWED,RWED,RWED "},
    {0x0209, 0x81240000, "f", "R,R,R       0.9"},
    {0x1209, 0x80000000, "f", "SHMWREXWR   0.9"},
    {0x030a, 0x81a40000, "f", "-rw-r--r--  1.0"},
    {0x130a, 0x41ed0000, "f", "drwxr-xr-x  1.0"},
    {0x000a, 0x00000020, "f", "-rw-a--     1.0"},
    {0x010a, 0x04000000, "f", "---------   1.0"},
    {0x020a, 0x81ff0000, "f", "RWED,RWED,RWED "},
    {0x020a, 0x81240000, "f", "R,R,R       1.0"},
    {0x120a, 0x80000000, "f", "SHMWREXWR   1.0"},
    {0x0314, 0x81a40000, "f", "-rw-r--r--  2.0"},
    {0x1314, 0x41ed0000, "f", "drwxr-xr-x  2.0"},
    {0x0014, 0x00000020, "f", "-rw-a--     2.0"},
    {0x0114, 0x04000000, "f", "---------   2.0"},
    {0x0214, 0x81ff0000, "f", "RWED,RWED,RWED "},
    {0x0214, 0x81240000, "f", "R,R,R       2.0"},
    {0x1214, 0x80000000, "f", "SHMWREXWR   2.0"},
    {0x032d, 0x81a40000, "f", "-rw-r--r--  4.5"},
    {0x132d, 0x41ed0000, "f", "drwxr-xr-x  4.5"},
    {0x002d, 0x00000020, "f", "-rw-a--     4.5"},
    {0x012d, 0x04000000, "f", "---------   4.5"},
    {0x022d, 0x81ff0000, "f", "RWED,RWED,RWED "},
    {0x022d, 0x81240000, "f", "R,R,R       4.5"},
    {0x122d, 0x80000000, "f", "SHMWREXWR   4.5"},
    {0x033f, 0x81a40000, "f", "-rw-r--r--  6.3"},
    {0x133f, 0x41ed0000, "f", "drwxr-xr-x  6.3"},
    {0x003f, 0x00000020, "f", "-rw-a--     6.3"},
    {0x013f, 0x04000000, "f", "---------   6.3"},
    {0x023f, 0x81ff0000, "f", "RWED,RWED,RWED "},
    {0x023f, 0x81240000, "f", "R,R,R       6.3"},
    {0x123f, 0x80000000, "f", "SHMWREXWR   6.3"},
    {0x0363, 0x81a40000, "f", "-rw-r--r--  9.9"},
    {0x1363, 0x41ed0000, "f", "drwxr-xr-x  9.9"},
    {0x0063, 0x00000020, "f", "-rw-a--     9.9"},
    {0x0163, 0x04000000, "f", "---------   9.9"},
    {0x0263, 0x81ff0000, "f", "RWED,RWED,RWED "},
    {0x0263, 0x81240000, "f", "R,R,R       9.9"},
    {0x1263, 0x80000000, "f", "SHMWREXWR   9.9"},
    {0x0364, 0x81a40000, "f", "-rw-r--r-- 10.0"},
    {0x1364, 0x41ed0000, "f", "drwxr-xr-x 10.0"},
    {0x036d, 0x81a40000, "f", "-rw-r--r-- 10.9"},
    {0x136d, 0x41ed0000, "f", "drwxr-xr-x 10.9"},
    {0x03ff, 0x81a40000, "f", "-rw-r--r-- 25.5"},
    {0x13ff, 0x41ed0000, "f", "drwxr-xr-x 25.5"},
};

// Hosts 0-255 as -s shows them, recorded
static char const recorded_hosts[256][4] = {
    "fat", "ami", "vms", "unx", "cms", "atr", "hpf", "mac",
    "zzz", "cpm", "t20", "ntf", "qds", "aco", "vft", "mvs",
    "be ", "nsk", "ths", "osx", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "ath", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
    "???", "???", "???", "???", "???", "???", "???", "???",
};

// Methods, with flag bits 1 and 2, as -s shows them, recorded
static struct {
    u32         method;
    u32         flags;
    char const *name;
} recorded_methods[] = {
    {0, 0, "stor"},
    {0, 2, "stor"},
    {0, 4, "stor"},
    {0, 6, "stor"},
    {1, 0, "shrk"},
    {1, 2, "shrk"},
    {1, 4, "shrk"},
    {1, 6, "shrk"},
    {2, 0, "re:1"},
    {2, 2, "re:1"},
    {2, 4, "re:1"},
    {2, 6, "re:1"},
    {3, 0, "re:2"},
    {3, 2, "re:2"},
    {3, 4, "re:2"},
    {3, 6, "re:2"},
    {4, 0, "re:3"},
    {4, 2, "re:3"},
    {4, 4, "re:3"},
    {4, 6, "re:3"},
    {5, 0, "re:4"},
    {5, 2, "re:4"},
    {5, 4, "re:4"},
    {5, 6, "re:4"},
    {6, 0, "i4:2"},
    {6, 2, "i8:2"},
    {6, 4, "i4:3"},
    {6, 6, "i8:3"},
    {7, 0, "tokn"},
    {7, 2, "tokn"},
    {7, 4, "tokn"},
    {7, 6, "tokn"},
    {8, 0, "defN"},
    {8, 2, "defX"},
    {8, 4, "defF"},
    {8, 6, "defS"},
    {9, 0, "d64N"},
    {9, 2, "d64X"},
    {9, 4, "d64F"},
    {9, 6, "d64S"},
    {10, 0, "dcli"},
    {10, 2, "dcli"},
    {10, 4, "dcli"},
    {10, 6, "dcli"},
    {11, 0, "u011"},
    {11, 2, "u011"},
    {11, 4, "u011"},
    {11, 6, "u011"},
    {12, 0, "bzp2"},
    {12, 2, "bzp2"},
    {12, 4, "bzp2"},
    {12, 6, "bzp2"},
    {13, 0, "u013"},
    {13, 2, "u013"},
    {13, 4, "u013"},
    {13, 6, "u013"},
    {14, 0, "lzma"},
    {14, 2, "lzma"},
    {14, 4, "lzma"},
    {14, 6, "lzma"},
    {15, 0, "u015"},
    {15, 2, "u015"},
    {15, 4, "u015"},
    {15, 6, "u015"},
    {16, 0, "u016"},
    {16, 2, "u016"},
    {16, 4, "u016"},
    {16, 6, "u016"},
    {17, 0, "u017"},
    {17, 2, "u017"},
    {17, 4, "u017"},
    {17, 6, "u017"},
    {18, 0, "ters"},
    {18, 2, "ters"},
    {18, 4, "ters"},
    {18, 6, "ters"},
    {19, 0, "lz77"},
    {19, 2, "lz77"},
    {19, 4, "lz77"},
    {19, 6, "lz77"},
    {20, 0, "u020"},
    {20, 2, "u020"},
    {20, 4, "u020"},
    {20, 6, "u020"},
    {96, 0, "u096"},
    {96, 2, "u096"},
    {96, 4, "u096"},
    {96, 6, "u096"},
    {97, 0, "wavp"},
    {97, 2, "wavp"},
    {97, 4, "wavp"},
    {97, 6, "wavp"},
    {98, 0, "ppmd"},
    {98, 2, "ppmd"},
    {98, 4, "ppmd"},
    {98, 6, "ppmd"},
    {99, 0, "u099"},
    {99, 2, "u099"},
    {99, 4, "u099"},
    {99, 6, "u099"},
    {100, 0, "u100"},
    {100, 2, "u100"},
    {100, 4, "u100"},
    {100, 6, "u100"},
    {255, 0, "u255"},
    {255, 2, "u255"},
    {255, 4, "u255"},
    {255, 6, "u255"},
    {256, 0, "u256"},
    {256, 2, "u256"},
    {256, 4, "u256"},
    {256, 6, "u256"},
    {999, 0, "u999"},
    {999, 2, "u999"},
    {999, 4, "u999"},
    {999, 6, "u999"},
};

// The two characters after the size: (made, flags, internal
// attributes, central extra length, external attributes), recorded
static struct {
    u32         made;
    u32         flags;
    u32         intattr;
    iz          xlen;
    u32         extattr;
    char const *chars;
} recorded_flags[] = {
    {0x0314, 0, 0x0, 0, 0x81a40000, "b-"},
    {0x0314, 0, 0x0, 4, 0x81a40000, "bx"},
    {0x0314, 0, 0x0, 0, 0x81a48000, "bx"},
    {0x0314, 0, 0x0, 4, 0x81a48000, "bx"},
    {0x0314, 0, 0x0, 0, 0x81a47fff, "b-"},
    {0x0314, 0, 0x1, 0, 0x81a40000, "t-"},
    {0x0314, 0, 0x1, 4, 0x81a40000, "tx"},
    {0x0314, 0, 0x1, 0, 0x81a48000, "tx"},
    {0x0314, 0, 0x1, 4, 0x81a48000, "tx"},
    {0x0314, 0, 0x1, 0, 0x81a47fff, "t-"},
    {0x0314, 0, 0x2, 0, 0x81a40000, "b-"},
    {0x0314, 0, 0xffff, 0, 0x81a40000, "t-"},
    {0x0314, 1, 0x0, 0, 0x81a40000, "B-"},
    {0x0314, 1, 0x0, 4, 0x81a40000, "Bx"},
    {0x0314, 1, 0x0, 0, 0x81a48000, "Bx"},
    {0x0314, 1, 0x0, 4, 0x81a48000, "Bx"},
    {0x0314, 1, 0x0, 0, 0x81a47fff, "B-"},
    {0x0314, 1, 0x1, 0, 0x81a40000, "T-"},
    {0x0314, 1, 0x1, 4, 0x81a40000, "Tx"},
    {0x0314, 1, 0x1, 0, 0x81a48000, "Tx"},
    {0x0314, 1, 0x1, 4, 0x81a48000, "Tx"},
    {0x0314, 1, 0x1, 0, 0x81a47fff, "T-"},
    {0x0314, 1, 0x2, 0, 0x81a40000, "B-"},
    {0x0314, 1, 0xffff, 0, 0x81a40000, "T-"},
    {0x0314, 8, 0x0, 0, 0x81a40000, "bl"},
    {0x0314, 8, 0x0, 4, 0x81a40000, "bX"},
    {0x0314, 8, 0x0, 0, 0x81a48000, "bX"},
    {0x0314, 8, 0x0, 4, 0x81a48000, "bX"},
    {0x0314, 8, 0x0, 0, 0x81a47fff, "bl"},
    {0x0314, 8, 0x1, 0, 0x81a40000, "tl"},
    {0x0314, 8, 0x1, 4, 0x81a40000, "tX"},
    {0x0314, 8, 0x1, 0, 0x81a48000, "tX"},
    {0x0314, 8, 0x1, 4, 0x81a48000, "tX"},
    {0x0314, 8, 0x1, 0, 0x81a47fff, "tl"},
    {0x0314, 8, 0x2, 0, 0x81a40000, "bl"},
    {0x0314, 8, 0xffff, 0, 0x81a40000, "tl"},
    {0x0314, 9, 0x0, 0, 0x81a40000, "Bl"},
    {0x0314, 9, 0x0, 4, 0x81a40000, "BX"},
    {0x0314, 9, 0x0, 0, 0x81a48000, "BX"},
    {0x0314, 9, 0x0, 4, 0x81a48000, "BX"},
    {0x0314, 9, 0x0, 0, 0x81a47fff, "Bl"},
    {0x0314, 9, 0x1, 0, 0x81a40000, "Tl"},
    {0x0314, 9, 0x1, 4, 0x81a40000, "TX"},
    {0x0314, 9, 0x1, 0, 0x81a48000, "TX"},
    {0x0314, 9, 0x1, 4, 0x81a48000, "TX"},
    {0x0314, 9, 0x1, 0, 0x81a47fff, "Tl"},
    {0x0314, 9, 0x2, 0, 0x81a40000, "Bl"},
    {0x0314, 9, 0xffff, 0, 0x81a40000, "Tl"},
    {0x0014, 0, 0x0, 0, 0x81a40000, "b-"},
    {0x0014, 0, 0x0, 4, 0x81a40000, "bx"},
    {0x0014, 0, 0x0, 0, 0x81a48000, "b-"},
    {0x0014, 0, 0x0, 4, 0x81a48000, "bx"},
    {0x0014, 0, 0x0, 0, 0x81a47fff, "b-"},
    {0x0014, 0, 0x1, 0, 0x81a40000, "t-"},
    {0x0014, 0, 0x1, 4, 0x81a40000, "tx"},
    {0x0014, 0, 0x1, 0, 0x81a48000, "t-"},
    {0x0014, 0, 0x1, 4, 0x81a48000, "tx"},
    {0x0014, 0, 0x1, 0, 0x81a47fff, "t-"},
    {0x0014, 0, 0x2, 0, 0x81a40000, "b-"},
    {0x0014, 0, 0xffff, 0, 0x81a40000, "t-"},
    {0x0014, 1, 0x0, 0, 0x81a40000, "B-"},
    {0x0014, 1, 0x0, 4, 0x81a40000, "Bx"},
    {0x0014, 1, 0x0, 0, 0x81a48000, "B-"},
    {0x0014, 1, 0x0, 4, 0x81a48000, "Bx"},
    {0x0014, 1, 0x0, 0, 0x81a47fff, "B-"},
    {0x0014, 1, 0x1, 0, 0x81a40000, "T-"},
    {0x0014, 1, 0x1, 4, 0x81a40000, "Tx"},
    {0x0014, 1, 0x1, 0, 0x81a48000, "T-"},
    {0x0014, 1, 0x1, 4, 0x81a48000, "Tx"},
    {0x0014, 1, 0x1, 0, 0x81a47fff, "T-"},
    {0x0014, 1, 0x2, 0, 0x81a40000, "B-"},
    {0x0014, 1, 0xffff, 0, 0x81a40000, "T-"},
    {0x0014, 8, 0x0, 0, 0x81a40000, "bl"},
    {0x0014, 8, 0x0, 4, 0x81a40000, "bX"},
    {0x0014, 8, 0x0, 0, 0x81a48000, "bl"},
    {0x0014, 8, 0x0, 4, 0x81a48000, "bX"},
    {0x0014, 8, 0x0, 0, 0x81a47fff, "bl"},
    {0x0014, 8, 0x1, 0, 0x81a40000, "tl"},
    {0x0014, 8, 0x1, 4, 0x81a40000, "tX"},
    {0x0014, 8, 0x1, 0, 0x81a48000, "tl"},
    {0x0014, 8, 0x1, 4, 0x81a48000, "tX"},
    {0x0014, 8, 0x1, 0, 0x81a47fff, "tl"},
    {0x0014, 8, 0x2, 0, 0x81a40000, "bl"},
    {0x0014, 8, 0xffff, 0, 0x81a40000, "tl"},
    {0x0014, 9, 0x0, 0, 0x81a40000, "Bl"},
    {0x0014, 9, 0x0, 4, 0x81a40000, "BX"},
    {0x0014, 9, 0x0, 0, 0x81a48000, "Bl"},
    {0x0014, 9, 0x0, 4, 0x81a48000, "BX"},
    {0x0014, 9, 0x0, 0, 0x81a47fff, "Bl"},
    {0x0014, 9, 0x1, 0, 0x81a40000, "Tl"},
    {0x0014, 9, 0x1, 4, 0x81a40000, "TX"},
    {0x0014, 9, 0x1, 0, 0x81a48000, "Tl"},
    {0x0014, 9, 0x1, 4, 0x81a48000, "TX"},
    {0x0014, 9, 0x1, 0, 0x81a47fff, "Tl"},
    {0x0014, 9, 0x2, 0, 0x81a40000, "Bl"},
    {0x0014, 9, 0xffff, 0, 0x81a40000, "Tl"},
    {0x0614, 0, 0x0, 0, 0x81a40000, "b-"},
    {0x0614, 0, 0x0, 4, 0x81a40000, "bx"},
    {0x0614, 0, 0x0, 0, 0x81a48000, "bx"},
    {0x0614, 0, 0x0, 4, 0x81a48000, "bx"},
    {0x0614, 0, 0x0, 0, 0x81a47fff, "b-"},
    {0x0614, 0, 0x1, 0, 0x81a40000, "t-"},
    {0x0614, 0, 0x1, 4, 0x81a40000, "tx"},
    {0x0614, 0, 0x1, 0, 0x81a48000, "tx"},
    {0x0614, 0, 0x1, 4, 0x81a48000, "tx"},
    {0x0614, 0, 0x1, 0, 0x81a47fff, "t-"},
    {0x0614, 0, 0x2, 0, 0x81a40000, "b-"},
    {0x0614, 0, 0xffff, 0, 0x81a40000, "t-"},
    {0x0614, 1, 0x0, 0, 0x81a40000, "B-"},
    {0x0614, 1, 0x0, 4, 0x81a40000, "Bx"},
    {0x0614, 1, 0x0, 0, 0x81a48000, "Bx"},
    {0x0614, 1, 0x0, 4, 0x81a48000, "Bx"},
    {0x0614, 1, 0x0, 0, 0x81a47fff, "B-"},
    {0x0614, 1, 0x1, 0, 0x81a40000, "T-"},
    {0x0614, 1, 0x1, 4, 0x81a40000, "Tx"},
    {0x0614, 1, 0x1, 0, 0x81a48000, "Tx"},
    {0x0614, 1, 0x1, 4, 0x81a48000, "Tx"},
    {0x0614, 1, 0x1, 0, 0x81a47fff, "T-"},
    {0x0614, 1, 0x2, 0, 0x81a40000, "B-"},
    {0x0614, 1, 0xffff, 0, 0x81a40000, "T-"},
    {0x0614, 8, 0x0, 0, 0x81a40000, "bl"},
    {0x0614, 8, 0x0, 4, 0x81a40000, "bX"},
    {0x0614, 8, 0x0, 0, 0x81a48000, "bX"},
    {0x0614, 8, 0x0, 4, 0x81a48000, "bX"},
    {0x0614, 8, 0x0, 0, 0x81a47fff, "bl"},
    {0x0614, 8, 0x1, 0, 0x81a40000, "tl"},
    {0x0614, 8, 0x1, 4, 0x81a40000, "tX"},
    {0x0614, 8, 0x1, 0, 0x81a48000, "tX"},
    {0x0614, 8, 0x1, 4, 0x81a48000, "tX"},
    {0x0614, 8, 0x1, 0, 0x81a47fff, "tl"},
    {0x0614, 8, 0x2, 0, 0x81a40000, "bl"},
    {0x0614, 8, 0xffff, 0, 0x81a40000, "tl"},
    {0x0614, 9, 0x0, 0, 0x81a40000, "Bl"},
    {0x0614, 9, 0x0, 4, 0x81a40000, "BX"},
    {0x0614, 9, 0x0, 0, 0x81a48000, "BX"},
    {0x0614, 9, 0x0, 4, 0x81a48000, "BX"},
    {0x0614, 9, 0x0, 0, 0x81a47fff, "Bl"},
    {0x0614, 9, 0x1, 0, 0x81a40000, "Tl"},
    {0x0614, 9, 0x1, 4, 0x81a40000, "TX"},
    {0x0614, 9, 0x1, 0, 0x81a48000, "TX"},
    {0x0614, 9, 0x1, 4, 0x81a48000, "TX"},
    {0x0614, 9, 0x1, 0, 0x81a47fff, "Tl"},
    {0x0614, 9, 0x2, 0, 0x81a40000, "Bl"},
    {0x0614, 9, 0xffff, 0, 0x81a40000, "Tl"},
    {0x0b14, 0, 0x0, 0, 0x81a40000, "b-"},
    {0x0b14, 0, 0x0, 4, 0x81a40000, "bx"},
    {0x0b14, 0, 0x0, 0, 0x81a48000, "bx"},
    {0x0b14, 0, 0x0, 4, 0x81a48000, "bx"},
    {0x0b14, 0, 0x0, 0, 0x81a47fff, "b-"},
    {0x0b14, 0, 0x1, 0, 0x81a40000, "t-"},
    {0x0b14, 0, 0x1, 4, 0x81a40000, "tx"},
    {0x0b14, 0, 0x1, 0, 0x81a48000, "tx"},
    {0x0b14, 0, 0x1, 4, 0x81a48000, "tx"},
    {0x0b14, 0, 0x1, 0, 0x81a47fff, "t-"},
    {0x0b14, 0, 0x2, 0, 0x81a40000, "b-"},
    {0x0b14, 0, 0xffff, 0, 0x81a40000, "t-"},
    {0x0b14, 1, 0x0, 0, 0x81a40000, "B-"},
    {0x0b14, 1, 0x0, 4, 0x81a40000, "Bx"},
    {0x0b14, 1, 0x0, 0, 0x81a48000, "Bx"},
    {0x0b14, 1, 0x0, 4, 0x81a48000, "Bx"},
    {0x0b14, 1, 0x0, 0, 0x81a47fff, "B-"},
    {0x0b14, 1, 0x1, 0, 0x81a40000, "T-"},
    {0x0b14, 1, 0x1, 4, 0x81a40000, "Tx"},
    {0x0b14, 1, 0x1, 0, 0x81a48000, "Tx"},
    {0x0b14, 1, 0x1, 4, 0x81a48000, "Tx"},
    {0x0b14, 1, 0x1, 0, 0x81a47fff, "T-"},
    {0x0b14, 1, 0x2, 0, 0x81a40000, "B-"},
    {0x0b14, 1, 0xffff, 0, 0x81a40000, "T-"},
    {0x0b14, 8, 0x0, 0, 0x81a40000, "bl"},
    {0x0b14, 8, 0x0, 4, 0x81a40000, "bX"},
    {0x0b14, 8, 0x0, 0, 0x81a48000, "bX"},
    {0x0b14, 8, 0x0, 4, 0x81a48000, "bX"},
    {0x0b14, 8, 0x0, 0, 0x81a47fff, "bl"},
    {0x0b14, 8, 0x1, 0, 0x81a40000, "tl"},
    {0x0b14, 8, 0x1, 4, 0x81a40000, "tX"},
    {0x0b14, 8, 0x1, 0, 0x81a48000, "tX"},
    {0x0b14, 8, 0x1, 4, 0x81a48000, "tX"},
    {0x0b14, 8, 0x1, 0, 0x81a47fff, "tl"},
    {0x0b14, 8, 0x2, 0, 0x81a40000, "bl"},
    {0x0b14, 8, 0xffff, 0, 0x81a40000, "tl"},
    {0x0b14, 9, 0x0, 0, 0x81a40000, "Bl"},
    {0x0b14, 9, 0x0, 4, 0x81a40000, "BX"},
    {0x0b14, 9, 0x0, 0, 0x81a48000, "BX"},
    {0x0b14, 9, 0x0, 4, 0x81a48000, "BX"},
    {0x0b14, 9, 0x0, 0, 0x81a47fff, "Bl"},
    {0x0b14, 9, 0x1, 0, 0x81a40000, "Tl"},
    {0x0b14, 9, 0x1, 4, 0x81a40000, "TX"},
    {0x0b14, 9, 0x1, 0, 0x81a48000, "TX"},
    {0x0b14, 9, 0x1, 4, 0x81a48000, "TX"},
    {0x0b14, 9, 0x1, 0, 0x81a47fff, "Tl"},
    {0x0b14, 9, 0x2, 0, 0x81a40000, "Bl"},
    {0x0b14, 9, 0xffff, 0, 0x81a40000, "Tl"},
    {0x0214, 0, 0x0, 0, 0x81a40000, "b-"},
    {0x0214, 0, 0x0, 4, 0x81a40000, "bx"},
    {0x0214, 0, 0x0, 0, 0x81a48000, "b-"},
    {0x0214, 0, 0x0, 4, 0x81a48000, "bx"},
    {0x0214, 0, 0x0, 0, 0x81a47fff, "b-"},
    {0x0214, 0, 0x1, 0, 0x81a40000, "t-"},
    {0x0214, 0, 0x1, 4, 0x81a40000, "tx"},
    {0x0214, 0, 0x1, 0, 0x81a48000, "t-"},
    {0x0214, 0, 0x1, 4, 0x81a48000, "tx"},
    {0x0214, 0, 0x1, 0, 0x81a47fff, "t-"},
    {0x0214, 0, 0x2, 0, 0x81a40000, "b-"},
    {0x0214, 0, 0xffff, 0, 0x81a40000, "t-"},
    {0x0214, 1, 0x0, 0, 0x81a40000, "B-"},
    {0x0214, 1, 0x0, 4, 0x81a40000, "Bx"},
    {0x0214, 1, 0x0, 0, 0x81a48000, "B-"},
    {0x0214, 1, 0x0, 4, 0x81a48000, "Bx"},
    {0x0214, 1, 0x0, 0, 0x81a47fff, "B-"},
    {0x0214, 1, 0x1, 0, 0x81a40000, "T-"},
    {0x0214, 1, 0x1, 4, 0x81a40000, "Tx"},
    {0x0214, 1, 0x1, 0, 0x81a48000, "T-"},
    {0x0214, 1, 0x1, 4, 0x81a48000, "Tx"},
    {0x0214, 1, 0x1, 0, 0x81a47fff, "T-"},
    {0x0214, 1, 0x2, 0, 0x81a40000, "B-"},
    {0x0214, 1, 0xffff, 0, 0x81a40000, "T-"},
    {0x0214, 8, 0x0, 0, 0x81a40000, "bl"},
    {0x0214, 8, 0x0, 4, 0x81a40000, "bX"},
    {0x0214, 8, 0x0, 0, 0x81a48000, "bl"},
    {0x0214, 8, 0x0, 4, 0x81a48000, "bX"},
    {0x0214, 8, 0x0, 0, 0x81a47fff, "bl"},
    {0x0214, 8, 0x1, 0, 0x81a40000, "tl"},
    {0x0214, 8, 0x1, 4, 0x81a40000, "tX"},
    {0x0214, 8, 0x1, 0, 0x81a48000, "tl"},
    {0x0214, 8, 0x1, 4, 0x81a48000, "tX"},
    {0x0214, 8, 0x1, 0, 0x81a47fff, "tl"},
    {0x0214, 8, 0x2, 0, 0x81a40000, "bl"},
    {0x0214, 8, 0xffff, 0, 0x81a40000, "tl"},
    {0x0214, 9, 0x0, 0, 0x81a40000, "Bl"},
    {0x0214, 9, 0x0, 4, 0x81a40000, "BX"},
    {0x0214, 9, 0x0, 0, 0x81a48000, "Bl"},
    {0x0214, 9, 0x0, 4, 0x81a48000, "BX"},
    {0x0214, 9, 0x0, 0, 0x81a47fff, "Bl"},
    {0x0214, 9, 0x1, 0, 0x81a40000, "Tl"},
    {0x0214, 9, 0x1, 4, 0x81a40000, "TX"},
    {0x0214, 9, 0x1, 0, 0x81a48000, "Tl"},
    {0x0214, 9, 0x1, 4, 0x81a48000, "TX"},
    {0x0214, 9, 0x1, 0, 0x81a47fff, "Tl"},
    {0x0214, 9, 0x2, 0, 0x81a40000, "Bl"},
    {0x0214, 9, 0xffff, 0, 0x81a40000, "Tl"},
    {0x1314, 0, 0x0, 0, 0x81a40000, "b-"},
    {0x1314, 0, 0x0, 4, 0x81a40000, "bx"},
    {0x1314, 0, 0x0, 0, 0x81a48000, "b-"},
    {0x1314, 0, 0x0, 4, 0x81a48000, "bx"},
    {0x1314, 0, 0x0, 0, 0x81a47fff, "b-"},
    {0x1314, 0, 0x1, 0, 0x81a40000, "t-"},
    {0x1314, 0, 0x1, 4, 0x81a40000, "tx"},
    {0x1314, 0, 0x1, 0, 0x81a48000, "t-"},
    {0x1314, 0, 0x1, 4, 0x81a48000, "tx"},
    {0x1314, 0, 0x1, 0, 0x81a47fff, "t-"},
    {0x1314, 0, 0x2, 0, 0x81a40000, "b-"},
    {0x1314, 0, 0xffff, 0, 0x81a40000, "t-"},
    {0x1314, 1, 0x0, 0, 0x81a40000, "B-"},
    {0x1314, 1, 0x0, 4, 0x81a40000, "Bx"},
    {0x1314, 1, 0x0, 0, 0x81a48000, "B-"},
    {0x1314, 1, 0x0, 4, 0x81a48000, "Bx"},
    {0x1314, 1, 0x0, 0, 0x81a47fff, "B-"},
    {0x1314, 1, 0x1, 0, 0x81a40000, "T-"},
    {0x1314, 1, 0x1, 4, 0x81a40000, "Tx"},
    {0x1314, 1, 0x1, 0, 0x81a48000, "T-"},
    {0x1314, 1, 0x1, 4, 0x81a48000, "Tx"},
    {0x1314, 1, 0x1, 0, 0x81a47fff, "T-"},
    {0x1314, 1, 0x2, 0, 0x81a40000, "B-"},
    {0x1314, 1, 0xffff, 0, 0x81a40000, "T-"},
    {0x1314, 8, 0x0, 0, 0x81a40000, "bl"},
    {0x1314, 8, 0x0, 4, 0x81a40000, "bX"},
    {0x1314, 8, 0x0, 0, 0x81a48000, "bl"},
    {0x1314, 8, 0x0, 4, 0x81a48000, "bX"},
    {0x1314, 8, 0x0, 0, 0x81a47fff, "bl"},
    {0x1314, 8, 0x1, 0, 0x81a40000, "tl"},
    {0x1314, 8, 0x1, 4, 0x81a40000, "tX"},
    {0x1314, 8, 0x1, 0, 0x81a48000, "tl"},
    {0x1314, 8, 0x1, 4, 0x81a48000, "tX"},
    {0x1314, 8, 0x1, 0, 0x81a47fff, "tl"},
    {0x1314, 8, 0x2, 0, 0x81a40000, "bl"},
    {0x1314, 8, 0xffff, 0, 0x81a40000, "tl"},
    {0x1314, 9, 0x0, 0, 0x81a40000, "Bl"},
    {0x1314, 9, 0x0, 4, 0x81a40000, "BX"},
    {0x1314, 9, 0x0, 0, 0x81a48000, "Bl"},
    {0x1314, 9, 0x0, 4, 0x81a48000, "BX"},
    {0x1314, 9, 0x0, 0, 0x81a47fff, "Bl"},
    {0x1314, 9, 0x1, 0, 0x81a40000, "Tl"},
    {0x1314, 9, 0x1, 4, 0x81a40000, "TX"},
    {0x1314, 9, 0x1, 0, 0x81a48000, "Tl"},
    {0x1314, 9, 0x1, 4, 0x81a48000, "TX"},
    {0x1314, 9, 0x1, 0, 0x81a47fff, "Tl"},
    {0x1314, 9, 0x2, 0, 0x81a40000, "Bl"},
    {0x1314, 9, 0xffff, 0, 0x81a40000, "Tl"},
};

// Times from DOS fields, as -s and -T show them, recorded
static struct {
    u32         dostime;
    char const *shown;
    char const *decimal;
} recorded_ztimes[] = {
    {0x50221883, "20-Jan-02 03:04", "20200102.030406"},
    {0x00000000, "80-000-00 00:00", "19800000.000000"},
    {0x00210000, "80-Jan-01 00:00", "19800101.000000"},
    {0xffffffff, "07-015-31 31:63", "21071531.316362"},
    {0x279fbf7d, "99-Dec-31 23:59", "19991231.235958"},
    {0x285d63cf, "00-Feb-29 12:30", "20000229.123030"},
    {0x5da530e8, "26-013-05 06:07", "20261305.060716"},
    {0x5c0530e8, "26-000-05 06:07", "20260005.060716"},
    {0x5dc530e8, "26-014-05 06:07", "20261405.060716"},
    {0x5cc0c79e, "26-Jun-00 24:60", "20260600.246060"},
    {0xc6e40843, "79-Jul-04 01:02", "20790704.010206"},
    {0xc909516c, "80-Aug-09 10:11", "20800809.101124"},
    {0xef7eb000, "99-Nov-30 22:00", "20991130.220000"},
    {0xf0210000, "00-Jan-01 00:00", "21000101.000000"},
    {0x0a694929, "85-Mar-09 09:09", "19850309.090918"},
    {0x743319c4, "38-Jan-19 03:14", "20380119.031408"},
};

// Compression factors: (size, compressed size, encrypted) -> -m's
// percentage and the totals line's factor, recorded
static struct {
    u64         usize;
    u64         csize;
    b32         enc;
    char const *pct;
    char const *factor;
} recorded_ratios[] = {
    {0, 0, 0, "  0%", "0.0%"},
    {0, 5, 0, "  0%", "0.0%"},
    {100, 17, 0, " 83%", "83.0%"},
    {100, 100, 0, "  0%", "0.0%"},
    {100, 0, 0, "100%", "100.0%"},
    {100, 200, 0, "-99%", "-100.0%"},
    {100, 199, 0, "-98%", "-99.0%"},
    {100, 201, 0, "-100%", "-101.0%"},
    {1000, 995, 0, "  1%", "0.5%"},
    {1000, 1005, 0, "  0%", "-0.5%"},
    {1000, 1004, 0, "  0%", "-0.4%"},
    {1000, 996, 0, "  0%", "0.4%"},
    {3, 1, 0, " 67%", "66.7%"},
    {3, 2, 0, " 33%", "33.3%"},
    {7, 9, 0, "-28%", "-28.6%"},
    {1, 1000, 0, "-99899%", "-99900.0%"},
    {2000000, 1000001, 0, " 50%", "50.0%"},
    {2000001, 1000001, 0, " 50%", "50.0%"},
    {3000000, 1, 0, "100%", "100.0%"},
    {3000000, 6000000, 0, "-99%", "-100.0%"},
    {4294967295, 1, 0, "100%", "100.0%"},
    {4294967295, 4294967295, 0, "  0%", "0.0%"},
    {1, 2000000, 0, "-199999899%", "-199999900.0%"},
    {100, 112, 1, "  0%", "0.0%"},
    {100, 12, 1, "100%", "100.0%"},
    {100, 13, 1, " 99%", "99.0%"},
    {11, 23, 1, "  0%", "0.0%"},
    {2, 13, 1, " 50%", "50.0%"},
    {100, 2147483, 0, "-2147382%", "-2147383.0%"},
    {12345, 6789, 1, " 45%", "45.1%"},
};

// ZipInfo mode's rules (src/zipinfo.c)

static b32 zequal(zistr z, char const *want)
{
    return equals(zi_s8(&z), want);
}

// The options before the archive, word by word, resolved as with or
// without members, against what ZipInfo showed: the format (1 for names
// alone, -1 and -2 alike), and the header, totals, and comment (-v
// shows its comment section whatever -z says), or usage, status 10
static void test_zipinfo_options(void)
{
    for (i32 i = 0; i < countof(recorded_options); i++) {
        char const *w = recorded_options[i].words;
        for (i32 members = 0; members < 2; members++) {
            i32 const *want = recorded_options[i].got[members];
            ziopts     z    = zi_new();
            b32        bad  = 0;
            b32        word = 0;  // within one, past its '-'
            for (char const *p = w; *p; p++) {
                if (*p == ' ') {
                    word = 0;
                } else if (!word) {
                    TESTROW(*p == '-', i);
                    word = 1;
                } else {
                    bad |= zi_option(&z, (u8)*p) != ZI_OK;
                }
            }
            if (bad) {
                TESTROW(want[0] == 10, i);
                continue;
            }
            TESTROW(want[0] == 0, i);
            zi_resolve(&z, members);
            i32 format = z.lflag==2 ? 1 : z.lflag;
            TESTROW(format == want[1], i);
            TESTROW(z.hflag == want[2], i);
            TESTROW(z.tflag == want[3], i);
            TESTROW(format==10 || z.zflag==want[4], i);
        }
    }

    // Letters that ZipInfo takes and tugz refuses, M (its pager) and U
    // (Debian's Unicode escapes), or does not know (usage, 10)
    ziopts z = zi_new();
    TEST(zi_option(&z, 'M') == ZI_REFUSED);
    TEST(zi_option(&z, 'U') == ZI_REFUSED);
    TEST(zi_option(&z, 'W') == ZI_BAD);  // Debian's, as Apple's lacks it
    TEST(zi_option(&z, 'q') == ZI_BAD);
    // Z keeps a negation pending
    z = zi_new();
    TEST(!zi_option(&z, '-') && !zi_option(&z, 'Z') && !zi_option(&z, 'h'));
    zi_resolve(&z, 0);
    TEST(z.lflag==3 && !z.hflag && z.tflag);
}

static void test_zipinfo_names(void)
{
    for (i32 i = 0; i < countof(recorded_programs); i++) {
        s8 n = str(recorded_programs[i].name);
        TESTROW(zi_named(n, 0) == recorded_programs[i].zipinfo, i);
    }

    // With Windows conventions, after the last '\' or '/' (not ':', as
    // win32/w32cfg.h's DIR_END and DIR_END2), not recorded
    static struct {
        char const *name;
        b32         zipinfo;
    } t[] = {
        {"C:\\bin\\ZipInfo.exe", 1},
        {"C:\\bin\\unzip.exe", 0},
        {"a\\ii", 1},
        {"ZIP\\ii.exe", 1},
        {"a\\zipinfo", 1},
        {"x\\y/zipinfo.exe", 1},
        {"zipinfo\\unzip.exe", 0},
        {"zipinfo/unzip.exe", 0},
        {"C:zipinfo.exe", 0},
        {"ZIPINFO", 1},
        {"", 0},
        {"\\", 0},
    };
    for (i32 i = 0; i < countof(t); i++) {
        TESTROW(zi_named(str(t[i].name), 1) == t[i].zipinfo, i);
    }
}

static void test_zipinfo_attribs(void)
{
    for (i32 i = 0; i < countof(recorded_attribs); i++) {
        zistr a = zi_attribs(recorded_attribs[i].made,
                             recorded_attribs[i].extattr,
                             str(recorded_attribs[i].name));
        TESTROW(zequal(a, recorded_attribs[i].attr), i);
    }

    // Versions from 10.0 with other hosts' attributes, where UnZip's
    // builds abort: a column early, as with Unix's
    static struct {
        u32         made;
        u32         extattr;
        char const *attr;
    } t[] = {
        {0x0064, 0x20, "-rw-a--    10.0"},
        {0x0bff, 0x21, "-r--a--    25.5"},
        {0x016d, 0x0400u<<16, "---------  10.9"},
        {0x1264, 0x8000u<<16, "SHMWREXWR  10.0"},
        {0x0264, 0x8124u<<16, "R,R,R      10.0"},
        {0x0264, 0x81ffu<<16, "RWED,RWED,RWED "},
        {0x0264, 0x81f8u<<16, "RWED,RWED, 10.0"},
        {0x0263, 0x81f8u<<16, "RWED,RWED,  9.9"},
        {0x0263, 0x81f9u<<16, "RWED,RWED,E 9.9"},
        {0x0264, 0x81f9u<<16, "RWED,RWED,E    "},
    };
    for (i32 i = 0; i < countof(t); i++) {
        zistr a = zi_attribs(t[i].made, t[i].extattr, S("f"));
        TESTROW(zequal(a, t[i].attr), i);
    }
}

static void test_zipinfo_hosts(void)
{
    for (u32 h = 0; h < 256; h++) {
        TESTROW(equals(zi_host(h<<8 | 20), recorded_hosts[h]), h);
    }
}

static void test_zipinfo_methods(void)
{
    for (i32 i = 0; i < countof(recorded_methods); i++) {
        zistr m = zi_method(recorded_methods[i].method,
                            recorded_methods[i].flags);
        TESTROW(zequal(m, recorded_methods[i].name), i);
    }
    // Past UnZip's buffer, from 1000, as distributions' patches show them
    TEST(zequal(zi_method(1000, 0), "u1000"));
    TEST(zequal(zi_method(65535, 6), "u65535"));
    TEST(zequal(zi_method(8, 0xfff7), "defS"));
}

static void test_zipinfo_flags(void)
{
    for (i32 i = 0; i < countof(recorded_flags); i++) {
        zistr f = zi_flags(recorded_flags[i].flags, recorded_flags[i].intattr,
                           recorded_flags[i].extattr, recorded_flags[i].made,
                           recorded_flags[i].xlen);
        TESTROW(zequal(f, recorded_flags[i].chars), i);
    }
}

static void test_zipinfo_dates(void)
{
    for (i32 i = 0; i < countof(recorded_ztimes); i++) {
        i32 tm[6];
        uz_dosdate(recorded_ztimes[i].dostime, tm);
        TESTROW(zequal(zi_date(tm, 0), recorded_ztimes[i].shown), i);
        TESTROW(zequal(zi_date(tm, 1), recorded_ztimes[i].decimal), i);
    }
    // A local time from an extended timestamp, years past 2099 too
    i32 tm[6] = {2106, 2, 7, 6, 28, 15};
    TEST(zequal(zi_date(tm, 0), "06-Feb-07 06:28"));
    TEST(zequal(zi_date(tm, 1), "21060207.062815"));
}

static void test_zipinfo_ratios(void)
{
    for (i32 i = 0; i < countof(recorded_ratios); i++) {
        u64 c = recorded_ratios[i].csize - (recorded_ratios[i].enc ? 12 : 0);
        i32 f = uz_ratio(recorded_ratios[i].usize, c);
        TESTROW(zequal(zi_percent(f), recorded_ratios[i].pct), i);
        TESTROW(zequal(zi_factor(f), recorded_ratios[i].factor), i);
    }
    // Where UnZip's arithmetic overflows, as for -v (unzip.sh's
    // ratio.zip), the growth as large as fits
    i32 f = uz_ratio(1, (u64)-1);
    TEST(zequal(zi_percent(f), "-214748363%"));
    TEST(zequal(zi_factor(f), "-214748364.2%"));
}

int main(void)
{
    (void)bytemove;
    iz cap = (iz)1 << 24;
    arena a = {0};
    a.beg = malloc((uz)cap);
    a.end = a.beg + cap;

    test_recorded_names(a);
    test_message_names(a);
    test_windows_names(a);
    test_name_safety(a);
    test_dosdate();
    test_timegm();
    test_recorded_times();
    test_recorded_dates();
    test_extra_izux();
    test_recorded_modes();
    test_modes();
    test_symlink_host();
    test_dosattr();
    test_filter(a);
    test_zipinfo_options();
    test_zipinfo_names();
    test_zipinfo_attribs();
    test_zipinfo_hosts();
    test_zipinfo_methods();
    test_zipinfo_flags();
    test_zipinfo_dates();
    test_zipinfo_ratios();

    free(a.beg);
    puts("all unzip tests pass");
    return 0;
}
