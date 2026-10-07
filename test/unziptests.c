// Unit tests for unzip's rules (src/unzip.c)
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
#include "../src/base.c"
#pragma GCC diagnostic push  // unzip.c needs only a little of zip.c
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../src/zip.c"
#pragma GCC diagnostic pop
#include "../src/unzip.c"

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

    free(a.beg);
    puts("all unzip tests pass");
    return 0;
}
