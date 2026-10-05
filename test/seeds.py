# Generate libFuzzer seed corpora under fuzz/corpus/
# Usage: uv run --no-project python test/seeds.py
import os, random, zlib

random.seed(1)
root = "fuzz/corpus"
samples = [
    b"",
    b"x",
    b"hello, world\n" * 50,
    bytes(range(256)) * 4,
    bytes(5000),
    bytes(random.getrandbits(8) for _ in range(3000)),
    open("/usr/share/dict/words", "rb").read()[:20000],
    open("src/deflate.c", "rb").read()[:30000],
    bytes(random.choice(b"ab") for _ in range(4000)),
]

def put(target, name, data):
    d = os.path.join(root, target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)

n = 0
for i, s in enumerate(samples):
    for level in (0, 1, 6, 9):
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED,
                         zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE):
            for wbits in (-15, 31):
                c = zlib.compressobj(level, zlib.DEFLATED, wbits, 8, strategy)
                z = c.compress(s) + c.flush()
                for t in ("inflate", "diff-inflate"):
                    put(t, f"s{n}", z)
                n += 1
    for k in range(3):
        cfg = bytes(random.getrandbits(8) for _ in range(8))
        put("roundtrip", f"s{i}_{k}", cfg[:3] + s[:8000])
        put("diff-deflate", f"s{i}_{k}", cfg + s[:8000])
print(n, "deflate seeds")

# ZIP archives for fuzz-zipread, mostly from Python's zipfile, some
# patched into forms it does not write: Zip64 end records and central
# extras, saturated disk numbers, encryption flags, and data prepended.
import io, shutil, struct, subprocess, tempfile, zipfile

def entry(name, method=zipfile.ZIP_DEFLATED, comment=b"", extra=b""):
    zi = zipfile.ZipInfo(name, date_time=(2024, 5, 6, 7, 8, 10))
    zi.compress_type = method
    zi.comment = comment
    zi.extra = extra
    return zi

def archive(entries, comment=b"", fp=None):
    fp = fp or io.BytesIO()
    with zipfile.ZipFile(fp, "a" if fp.getvalue() else "w") as zf:
        zf.comment = comment
        for zi, data in entries:
            zf.writestr(zi, data)
    return fp.getvalue()

class Unseekable(io.RawIOBase):
    """Output that cannot seek, so zipfile writes data descriptors."""
    def __init__(self):
        self.buf = bytearray()
    def writable(self):
        return True
    def write(self, b):
        self.buf += b
        return len(b)
    def getvalue(self):
        return bytes(self.buf)

def end_record(z):
    end = z.rindex(b"PK\x05\x06")
    count, cdsize, cdoff, clen = struct.unpack("<HIIH", z[end+10:end+22])
    return end, count, cdsize, cdoff, z[end+22:end+22+clen]

def with_end64(z):
    """Precede the end record with Zip64 records, saturating its fields."""
    end, count, cdsize, cdoff, comment = end_record(z)
    rec = struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0,
                      count, count, cdsize, cdoff)
    loc = struct.pack("<IIQI", 0x07064b50, 0, cdoff+cdsize, 1)
    e = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 0xffff, 0xffff,
                    0xffffffff, 0xffffffff, len(comment))
    return z[:end] + rec + loc + e + comment

def with_central64(z, disk=False):
    """Move central headers' sizes and offsets into Zip64 extras, with
    disk also saturating their disk numbers."""
    end, count, cdsize, cdoff, comment = end_record(z)
    cd = b""
    p = cdoff
    for _ in range(count):
        h = bytearray(z[p:p+46])
        csize, usize = struct.unpack("<II", h[20:28])
        nlen, xlen, clen = struct.unpack("<HHH", h[28:34])
        off = struct.unpack("<I", h[42:46])[0]
        x64 = struct.pack("<QQQ", usize, csize, off)
        if disk:
            x64 += struct.pack("<I", 0)
            h[34:36] = struct.pack("<H", 0xffff)
        h[20:28] = struct.pack("<II", 0xffffffff, 0xffffffff)
        h[42:46] = struct.pack("<I", 0xffffffff)
        h[30:32] = struct.pack("<H", 4 + len(x64) + xlen)
        name = z[p+46:p+46+nlen]
        rest = z[p+46+nlen:p+46+nlen+xlen+clen]
        cd += bytes(h) + name + struct.pack("<HH", 1, len(x64)) + x64 + rest
        p += 46 + nlen + xlen + clen
    e = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, count, count, len(cd),
                    cdoff, len(comment))
    return z[:cdoff] + cd + e + comment

def encrypted(z):
    """Set the encryption flag (bit 0) in the first entry's headers."""
    cdoff = end_record(z)[3]
    z = bytearray(z)
    z[6] |= 1
    z[cdoff+8] |= 1
    return bytes(z)

text = open("src/zip.c", "rb").read()[:3000]
noise = bytes(random.getrandbits(8) for _ in range(500))
fakeloc = b"note" + struct.pack("<IIQI", 0x07064b50, 0, 0, 1)
ut = b"UT\x05\x00\x03\x00\xf1\x53\x65"
ux = b"ux\x0b\x00\x01\x04\xe8\x03\x00\x00\x04\xe8\x03\x00\x00"
up = b"up\x0a\x00\x01\x00\x00\x00\x00caf\xc3\xa9"
uc = b"uc\x09\x00\x01\x00\x00\x00\x00note"
sh = b"#!/bin/sh\nexit 0\n"

basic = [
    (entry("dir/", zipfile.ZIP_STORED), b""),
    (entry("dir/stored.txt", zipfile.ZIP_STORED), b"hello, world\n" * 8),
    (entry("dir/deflated.c"), text),
    (entry("noise.bin"), noise),
]
zips = {
    "empty": archive([]),
    "empty-comment": archive([], comment=b"empty archive"),
    "stored": archive([(entry("a.txt", zipfile.ZIP_STORED), b"abc\n" * 20)]),
    "deflated": archive([(entry("a.c"), text)]),
    "basic": archive(basic),
    "comments": archive(
        [(entry("a.txt", comment=b"first entry"), b"a\n"),
         (entry("b.txt", zipfile.ZIP_STORED, comment=b"second"), b"b\n")],
        comment=b"archive comment"),
    "extras": archive([(entry("caf\xe9", extra=ut+ux+up+uc), b"x" * 100),
                       (entry("b", extra=ut+b"\x99\x99\x02\x00zz"), b"y")]),
    "utf8": archive([(entry("caf\xe9/na\xefve.txt"), b"utf-8\n"),
                     (entry("日本/\U0001f600"), b"names\n")]),
    "descriptor": archive(basic[1:3], fp=Unseekable()),
    "encrypted": encrypted(archive([(entry("secret"), noise[:100])])),
    "fake-locator": archive([(entry("a.txt", comment=fakeloc), b"a\n")]),
    "prefix": sh + archive(basic[:2]),
    "prefix-adjusted": archive(basic[:2], fp=io.BytesIO(sh)),  # zip -A
}

# Zip64 local headers, as zipfile writes when forced
fp = io.BytesIO()
with zipfile.ZipFile(fp, "w") as zf:
    for zi, data in basic[1:3]:
        with zf.open(zi, "w", force_zip64=True) as f:
            f.write(data)
zips["zip64-local"] = fp.getvalue()
zips["zip64-end"] = with_end64(zips["basic"])
zips["zip64-central"] = with_central64(zips["comments"])
zips["zip64-disk"] = with_central64(zips["stored"], disk=True)
zips["zip64-all"] = with_end64(with_central64(zips["zip64-local"]))
zips["zip64-fake-locator"] = with_end64(zips["fake-locator"])

# A few from a zip program, if present (likely Info-ZIP)
zipprog = shutil.which("zip")
if zipprog:
    with tempfile.TemporaryDirectory() as tmp:
        files = {"a.txt": b"hello, world\n" * 8, "d/b.c": text,
                 "d/e/n.bin": noise}
        for name, data in files.items():
            path = os.path.join(tmp, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
        for top, dirs, names in os.walk(tmp):
            for name in dirs + names:
                os.utime(os.path.join(top, name), (1700000000, 1700000000))
        runs = {
            "zip-recurse": ["-r", "out.zip", "a.txt", "d"],
            "zip-noextra": ["-X", "-r", "out.zip", "a.txt", "d"],
            "zip-store": ["-0", "out.zip", "a.txt", "d/b.c"],
            "zip-zip64": ["-fz", "out.zip", "a.txt", "d/b.c"],
            "zip-comment": ["-z", "out.zip", "a.txt"],
            "zip-stream": ["-", "a.txt", "d/b.c"],  # data descriptors
        }
        out = os.path.join(tmp, "out.zip")
        for name, args in runs.items():
            if os.path.exists(out):
                os.remove(out)
            r = subprocess.run([zipprog, "-q", *args], cwd=tmp,
                               input=b"archive comment\n", capture_output=True)
            if r.returncode == 0:
                stream = args[0] == "-"
                zips[name] = r.stdout if stream else open(out, "rb").read()

for name, z in zips.items():
    put("zipread", name + ".zip", z)
print(len(zips), "zip seeds")
