# Write crafted archives for test/unzip.sh into the current directory:
# layouts that zip programs rarely or never write, damaged archives, and
# entries that unzip must skip or refuse.
# Usage: python3 test/unzipcraft.py
import struct
import zlib

# 2020-01-02 03:04:06, as an MS-DOS date<<16 | time
DOSTIME = (40 << 25 | 1 << 21 | 2 << 16) | (3 << 11 | 4 << 5 | 3)
TEXT = b"hello hello hello hello\n" * 40


def deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def ut(mtime):
    """An extended timestamp field ("UT") with a modification time."""
    return struct.pack("<HHBI", 0x5455, 5, 1, mtime)


class Entry:
    def __init__(self, name, data=b"", method=8, **kw):
        self.name = name if isinstance(name, bytes) else name.encode()
        self.data = data
        self.method = method
        self.comp = kw.get("comp")
        if self.comp is None:
            self.comp = deflate(data) if method == 8 else data
        self.crc = kw.get("crc", zlib.crc32(data))
        self.usize = kw.get("usize", len(data))
        self.csize = kw.get("csize", len(self.comp))
        self.flags = kw.get("flags", 0)
        self.made = kw.get("made", 3 << 8 | 30)
        self.needed = kw.get("needed", 20)
        self.extattr = kw.get("extattr", 0o100644 << 16)
        self.dostime = kw.get("dostime", DOSTIME)
        self.cextra = kw.get("cextra", b"")
        self.lextra = kw.get("lextra", b"")
        self.comment = kw.get("comment", b"")
        self.lname = kw.get("lname", self.name)
        self.desc = kw.get("desc", False)  # data descriptor, bit 3
        self.offset = kw.get("offset")  # a central offset, else its own
        self.local = kw.get("local", True)  # write its local header
        self.z64 = kw.get("z64", False)  # sizes and offset in Zip64 fields
        if self.z64:
            self.needed = 45
            self.lextra = struct.pack("<HHQQ", 1, 16, self.usize,
                                      self.csize) + self.lextra

    def local_header(self):
        flags = self.flags | (8 if self.desc else 0)
        crc, csize, usize = self.crc, self.csize, self.usize
        if self.z64:
            csize = usize = 0xFFFFFFFF
        if self.desc:
            crc = csize = usize = 0
        return struct.pack(
            "<IHHHIIIIHH", 0x04034B50, self.needed, flags, self.method,
            self.dostime, crc, csize, usize, len(self.lname),
            len(self.lextra)) + self.lname + self.lextra


def build(entries, comment=b"", prefix=b"", shifted=True, zip64=False,
          disk=0):
    """An archive of entries after prefix, whose offsets account for the
    prefix if shifted, with Zip64 end records if zip64."""
    out = bytearray(prefix)
    base = len(prefix) if shifted else 0
    offsets = []
    for e in entries:
        offsets.append(len(out) - len(prefix) + base)
        if e.local:
            out += e.local_header() + e.comp
            if e.desc:
                out += struct.pack("<IIII", 0x08074B50, e.crc, e.csize,
                                   e.usize)
    cd = bytearray()
    for e, off in zip(entries, offsets):
        off = e.offset if e.offset is not None else off
        flags = e.flags | (8 if e.desc else 0)
        csize, usize, cextra = e.csize, e.usize, e.cextra
        if e.z64:
            cextra = struct.pack("<HHQQQ", 1, 24, usize, csize, off) + cextra
            csize = usize = off = 0xFFFFFFFF
        cd += struct.pack(
            "<IHHHHIIIIHHHHHII", 0x02014B50, e.made, e.needed, flags,
            e.method, e.dostime, e.crc, csize, usize, len(e.name),
            len(cextra), len(e.comment), 0, 0, e.extattr, off)
        cd += e.name + cextra + e.comment
    cdoff = len(out) - len(prefix) + base
    end = len(out) - len(prefix) + base + len(cd)
    out += cd
    count = len(entries)
    if zip64:
        out += struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0,
                           count, count, len(cd), cdoff)
        out += struct.pack("<IIQI", 0x07064B50, 0, end, 1)
        out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF,
                           0xFFFFFFFF, 0xFFFFFFFF, len(comment)) + comment
    else:
        out += struct.pack("<IHHHHIIH", 0x06054B50, disk, disk, count, count,
                           len(cd), cdoff, len(comment)) + comment
    return bytes(out)


def write(name, data):
    with open(name, "wb") as f:
        f.write(data)


def basic():
    return [
        Entry("dir/", method=0, extattr=0o40755 << 16 | 0x10),
        Entry("dir/text.txt", TEXT, comment=b"an entry comment"),
        Entry("dir/stored.txt", b"stored\n", method=0),
        Entry("empty", b"", method=0),
        Entry("ut.txt", b"times\n", cextra=ut(1600000000)),
    ]


write("basic.zip", build(basic(), comment=b"The archive comment.\n"))
write("nocomment.zip", build(basic()))
write("sfx.zip", build(basic(), prefix=b"#!/bin/sh\nexit 0\n" * 4,
                       shifted=False))
write("sfxok.zip", build(basic(), prefix=b"#!/bin/sh\nexit 0\n" * 4))
write("zip64.zip", build(basic(), zip64=True))
write("desc.zip", build([Entry("a.txt", TEXT, desc=True),
                         Entry("b.txt", b"b\n", method=0, desc=True)]))
write("empty.zip", build([]))
write("emptycomment.zip", build([], comment=b"nothing here"))
write("emptysfx.zip", struct.pack("<16s", b"#!/bin/sh\nexit\n") +
      struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0, 0, 0, 0, 0))
write("multi.zip", build(basic(), disk=1))

# Entries that cannot be read: encrypted, and methods other than stored
# and deflated, among others that can
write("methods.zip", build([
    Entry("ok.txt", TEXT),
    Entry("enc.txt", method=8, comp=b"\x00" * 30, crc=1, usize=20, flags=1),
    Entry("d64.bin", method=9, comp=b"\x00" * 10, crc=2, usize=20),
    Entry("bzip2.bin", method=12, comp=b"BZh9" + b"\x00" * 10, crc=3,
          usize=20),
    Entry("lzma.bin", method=14, comp=b"\x00" * 10, crc=4, usize=20),
    Entry("shrunk.bin", method=1, comp=b"\x00" * 10, crc=5, usize=20),
    Entry("aes.bin", method=99, comp=b"\x00" * 10, crc=6, usize=20),
    Entry("new.bin", TEXT, needed=63),
    Entry("last.txt", b"last\n", method=0),
]))
write("encrypted.zip", build([
    Entry("enc.txt", method=8, comp=b"\x00" * 30, crc=1, usize=20, flags=1),
]))
write("deflate64.zip", build([
    Entry("d64.bin", method=9, comp=b"\x00" * 10, crc=2, usize=20),
]))

# More entries than one of UnZip's blocks (16384), one skipped in each,
# whose message comes before the tests of its block
many = [Entry("f%05d" % i, b"%d\n" % i, method=0) for i in range(16400)]
for i in (5, 16390):
    many[i] = Entry("f%05d" % i, method=14, comp=b"\x00" * 4, crc=0, usize=2)
write("many.zip", build(many))

# Damaged data
write("badcrc.zip", build([Entry("good.txt", b"good\n"),
                           Entry("bad.txt", TEXT, crc=0x12345678),
                           Entry("badstored.txt", b"x\n", method=0, crc=7)]))
write("baddata.zip", build([Entry("bad.txt", TEXT, comp=b"\xff" * 20,
                                  csize=20),
                            Entry("good.txt", b"good\n")]))
write("truncdata.zip", build([Entry("cut.txt", TEXT,
                                    comp=deflate(TEXT)[:10])]))
write("overrun.zip", build([Entry("big.txt", TEXT, usize=10,
                                  crc=zlib.crc32(TEXT[:10]))]))
write("storedsize.zip", build([Entry("s.txt", b"stored\n", method=0,
                                     usize=99)]))
write("localname.zip", build([Entry("central.txt", b"x\n",
                                    lname=b"local.txt")]))
write("badextra.zip", build([Entry("x.txt", b"x\n",
                                   lextra=b"UT\x09\x00\x01")]))

# Damaged structure
good = build(basic())
write("truncated.zip", good[:len(good) - 10])
write("notzip", b"This is not a zip file.\n" * 20)
bad = bytearray(good)
cdoff = struct.unpack("<I", good[-6:-2])[0]
bad[-6:-2] = struct.pack("<I", cdoff - 3)  # as if 3 bytes preceded
write("badcdoff.zip", bytes(bad))
second = bytearray(good)
first_len = 46 + len(b"dir/")
second[cdoff + first_len:cdoff + first_len + 4] = b"XXXX"
write("badheader.zip", bytes(second))
local = bytearray(good)
local[0:4] = b"PK\x00\x00"  # the first entry's local header
write("badlocal.zip", bytes(local))

# Overlapped components: zip bombs
shared = Entry("a.txt", TEXT)
write("overlap.zip", build([shared, Entry("b.txt", TEXT, offset=0,
                                          local=False)]))
write("overlapcd.zip", build([Entry("a.txt", TEXT, csize=len(deflate(TEXT))
                                    + 20)]))
nested = Entry("inner.txt", b"inner\n", method=0)
outer = Entry("outer.bin", b"", method=0)
body = nested.local_header() + nested.comp
outer.comp = body
outer.data = body
outer.csize = outer.usize = len(body)
outer.crc = zlib.crc32(body)
arc = bytearray(build([outer]))
# a second central entry for the stored entry inside the first's data
inner_off = 30 + len(outer.name)
cd = struct.pack(
    "<IHHHHIIIIHHHHHII", 0x02014B50, nested.made, 20, 0, 0, DOSTIME,
    nested.crc, nested.csize, nested.usize, len(nested.name), 0, 0, 0, 0,
    nested.extattr, inner_off) + nested.name
end = arc.rindex(b"PK\x05\x06")
cdoff = struct.unpack("<I", arc[end + 16:end + 20])[0]
cdsize = struct.unpack("<I", arc[end + 12:end + 16])[0]
arc = arc[:end] + cd + struct.pack(
    "<IHHHHIIH", 0x06054B50, 0, 0, 2, 2, cdsize + len(cd), cdoff, 0)
write("inner.zip", bytes(arc))

# Names: UTF-8 (bit 11), a Unicode path field (0x7075), controls, DOS
utf = "café.txt".encode()
upath = struct.pack("<HHBI", 0x7075, 5 + len(utf), 1,
                    zlib.crc32(b"cafe.txt")) + utf
write("names.zip", build([
    Entry(utf, b"utf8\n", flags=1 << 11),
    Entry(b"cafe.txt", b"upath\n", cextra=upath),
    Entry(b"ctl\x01\x1b[1mx.txt", b"ctl\n"),
    Entry(b"dos\\path.txt", b"dos\n", made=0 << 8 | 20),
]))
write("comments.zip", build(
    [Entry("c.txt", b"c\n", comment=b"tab\there\x1b[1m bold\r\n")],
    comment=b"line one\r\nline\x1b[31m two\x07\x00hidden"))

# Extraction: trees with directories, modes, times, owners, and links

# 2020-07-04 12:34:56, in summer where it has daylight saving time
SUMMER = (40 << 25 | 7 << 21 | 4 << 16) | (12 << 11 | 34 << 5 | 28)
DIR, LINK, FILE = 0o40000, 0o120000, 0o100000


def utl(mtime, atime=None):
    """A local extended timestamp ("UT"): modification, access times."""
    if atime is None:
        return struct.pack("<HHBI", 0x5455, 5, 1, mtime)
    return struct.pack("<HHBII", 0x5455, 9, 3, mtime, atime)


def owner(uid, gid):
    """A Unix owner field ("ux", version 1, with 4-byte IDs)."""
    return struct.pack("<HHBBIBI", 0x7875, 11, 1, 4, uid, 4, gid)


def unix(name, data=b"", mode=0o644, kind=FILE, method=None, **kw):
    """An entry made on Unix, of a type and mode."""
    if method is None:
        method = 8 if data else 0
    dos = 0x10 if kind == DIR else 0
    return Entry(name, data, method=method,
                 extattr=(kind | mode) << 16 | dos, **kw)


def fat(name, data=b"", attr=0x20, **kw):
    """An entry made on MS-DOS, with its attributes."""
    return Entry(name, data, method=0, made=0 << 8 | 20, extattr=attr, **kw)


write("tree.zip", build([
    unix("top/", kind=DIR, mode=0o755, lextra=utl(1500000000),
         cextra=ut(1500000000)),
    unix("top/a.txt", TEXT, lextra=utl(1600000000, 1600000100),
         cextra=ut(1600000000)),
    unix("top/exec.sh", b"#!/bin/sh\necho hi\n", mode=0o755),
    unix("top/private", b"secret\n", mode=0o600, dostime=SUMMER),
    unix("top/setuid", b"s\n", mode=0o4755),
    unix("top/group/", kind=DIR, mode=0o2775),
    unix("top/sticky/", kind=DIR, mode=0o1777),
    unix("top/empty", mode=0o640),
    unix("top/link", b"a.txt", kind=LINK, mode=0o777),
    unix("top/dlink", b"sub/deep/f.txt", kind=LINK, mode=0o755, method=8),
    unix("top/dangling", b"nowhere", kind=LINK, mode=0o777),
    unix("top/zerolink", kind=LINK, mode=0o777),  # empty: a file
    unix("top/sub/", kind=DIR, mode=0o700, dostime=SUMMER),
    unix("top/sub/deep/", kind=DIR, mode=0o555),
    unix("top/sub/deep/f.txt", TEXT, mode=0o444),
    fat("fat/", attr=0x10),
    fat("fat/RO.TXT", b"ro\r\n", attr=0x21),
    fat("fat/RW.TXT", b"rw\r\n", dostime=SUMMER),
    fat("fat/SUB/", attr=0x10),
]))
write("implicit.zip", build([unix("implicit/dir/file.txt", b"implicit\n")]))
write("owners.zip", build([
    unix("own/", kind=DIR, mode=0o755, lextra=owner(0, 0)),
    unix("own/root.txt", b"root\n", lextra=owner(0, 0)),
]))
write("prompt.zip", build([
    unix("p/%s.txt" % c, (c * 3 + "\n").encode(), lextra=utl(1600000000))
    for c in "abcdef"
]))

# Names that would reach outside, or that name nothing, or that UnZip
# maps: "..", absolute paths, backslashes from MS-DOS, controls, ";N"
write("traversal.zip", build([
    unix("../evil1.txt", b"1\n"),
    unix("a/../../evil2.txt", b"2\n"),
    unix("/abs/evil3.txt", b"3\n"),
    unix("//abs/evil4.txt", b"4\n"),
    fat(b"..\\..\\evil5.txt", b"5\n"),
    fat(b"dir\\sub\\fat6.txt", b"6\n"),
    unix(b"ctl\x01\x1b[1mx\x7f.txt", b"7\n"),
    unix("./dot/./x.txt", b"8\n"),
    unix("..", b"9\n"),
    unix("dd/..", b"10\n"),
    unix("../", kind=DIR, mode=0o755),
    unix("ver.txt;1", b"11\n"),
    unix("semi;x", b"12\n"),
    unix("a/b/;1", b"13\n"),
]))

# Links that a later entry would write through, and a link that a file
# of the same name follows
write("linkfile.zip", build([
    unix("lnk", b"../outside", kind=LINK, mode=0o777),
    unix("lnk/pwned.txt", b"pwned\n"),
]))
write("linkabs.zip", build([
    unix("abs", b"/", kind=LINK, mode=0o777),
    unix("abs/tmp/pwned.txt", b"pwned\n"),
]))
write("linkdir.zip", build([
    unix("d/", kind=DIR, mode=0o755),
    unix("d/up", b"../..", kind=LINK, mode=0o777),
    unix("d/up/pwned.txt", b"pwned\n"),
]))
write("linkdup.zip", build([
    unix("same", b"a target", kind=LINK, mode=0o777),
    unix("same", b"a file\n"),
]))
write("plain.zip", build([unix("pre/x.txt", b"x\n"), unix("f", b"f\n")]))

# Sizes and offsets in Zip64 fields of each entry
write("zip64entries.zip", build([Entry("big.txt", TEXT, z64=True),
                                 Entry("small", b"s\n", method=0, z64=True)],
                                zip64=True))
write("large.zip", build([unix("large.txt", TEXT * 100),
                          unix("after.txt", b"after\n")]))
