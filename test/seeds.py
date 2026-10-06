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

def streaming(fmt, inpiece, outpiece=0):
    """First byte of a fuzz-diff-inflate input, selecting its streaming
    check's format and its input and output piece sizes (fuzz_pieces
    indices) for the rest of the input."""
    for low in range(4):
        c = outpiece<<5 | inpiece<<2 | low
        if c % 3 == fmt:
            return bytes([c])

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
                # The streaming check reads the rest after a config byte
                cfg = streaming(0 if wbits < 0 else 2, 1 + n%7, n//7 % 8)
                put("diff-inflate", f"s{n}c", cfg + z)
                n += 1
    for k in range(3):
        cfg = bytes(random.getrandbits(8) for _ in range(8))
        put("roundtrip", f"s{i}_{k}", cfg[:3] + s[:8000])
        put("diff-deflate", f"s{i}_{k}", cfg + s[:8000])
print(n, "deflate seeds")

# Dynamic blocks with a lone 1-bit distance code, which Go's
# compress/flate writes when a block's matches share one distance code,
# or an empty one. zlib never writes either, and random mutation rarely
# makes one, yet their invalid 1-bit entries once turned truncation inside
# a length's extra bits into an error in our streaming decoder.
import heapq, struct

LEN_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35,
            43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
LEN_EXTRA = [0]*8 + [1]*4 + [2]*4 + [3]*4 + [4]*4 + [5]*4 + [0]
DIST_BASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
             257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193,
             12289, 16385, 24577]
DIST_EXTRA = [0, 0] + [i//2 for i in range(28)]
CL_ORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]

class Bits:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0
    def put(self, v, n):
        self.acc |= v << self.n
        self.n += n
        while self.n >= 8:
            self.out.append(self.acc & 255)
            self.acc >>= 8
            self.n -= 8
    def code(self, c, n):  # Huffman codes go most significant bit first
        self.put(int(f"{c:0{n}b}"[::-1], 2), n)
    def done(self):
        return bytes(self.out) + (bytes([self.acc]) if self.n else b"")

def symbol(v, base, extra):
    i = max(i for i, b in enumerate(base) if b <= v)
    return i, extra[i], v - base[i]

def code_lengths(freq):
    """Huffman code lengths, at most 15, for {symbol: count}."""
    if len(freq) == 1:
        return {s: 1 for s in freq}  # a lone 1-bit code
    while True:
        heap = [(f, s, [s]) for s, f in freq.items()]
        heapq.heapify(heap)
        depth = dict.fromkeys(freq, 0)
        while len(heap) > 1:
            fa, ka, a = heapq.heappop(heap)
            fb, kb, b = heapq.heappop(heap)
            for s in a + b:
                depth[s] += 1
            heapq.heappush(heap, (fa+fb, min(ka, kb), a+b))
        if max(depth.values()) <= 15:
            return depth
        freq = {s: (f+1)//2 for s, f in freq.items()}

def canonical(lens):
    codes, code = [0]*len(lens), 0
    for n in range(1, 16):
        for s, l in enumerate(lens):
            if l == n:
                codes[s] = code
                code += 1
        code <<= 1
    return codes

def dynamic(w, tokens, final, bad=None):
    """One dynamic block of tokens, literals or (length, distance), with
    the code length code {0..15: 4 bits}. bad="empty" gives the matches
    an empty distance code, and bad="unused" codes the last match's
    distance with a lone code's unused codeword: both invalid."""
    lfreq, dfreq = {256: 1}, {}
    for t in tokens:
        if isinstance(t, int):
            lfreq[t] = lfreq.get(t, 0) + 1
        else:
            ls = 257 + symbol(t[0], LEN_BASE, LEN_EXTRA)[0]
            ds = symbol(t[1], DIST_BASE, DIST_EXTRA)[0]
            lfreq[ls] = lfreq.get(ls, 0) + 1
            dfreq[ds] = dfreq.get(ds, 0) + 1
    llens = code_lengths(lfreq)
    dlens = code_lengths(dfreq) if dfreq and bad != "empty" else {}
    assert bad != "unused" or len(dlens) == 1
    hlit = max(llens) + 1
    hdist = max(dlens) + 1 if dlens else 1
    lens = ([llens.get(s, 0) for s in range(hlit)] +
            [dlens.get(s, 0) for s in range(hdist)])
    w.put(final, 1)
    w.put(2, 2)
    w.put(hlit - 257, 5)
    w.put(hdist - 1, 5)
    w.put(19 - 4, 4)
    for s in CL_ORDER:
        w.put(4 if s < 16 else 0, 3)
    for l in lens:
        w.code(l, 4)
    lc, dc = canonical(lens[:hlit]), canonical(lens[hlit:])
    last = max((k for k, t in enumerate(tokens) if not isinstance(t, int)),
               default=-1)
    for k, t in enumerate(tokens):
        if isinstance(t, int):
            w.code(lc[t], lens[t])
            continue
        ls, le, lv = symbol(t[0], LEN_BASE, LEN_EXTRA)
        ds, de, dv = symbol(t[1], DIST_BASE, DIST_EXTRA)
        w.code(lc[257+ls], lens[257+ls])
        w.put(lv, le)
        if bad == "empty":
            w.put(0, 1)
        elif bad == "unused" and k == last:
            w.put(1, 1)
        else:
            w.code(dc[ds], lens[hlit+ds])
        w.put(dv, de)
    w.code(lc[256], lens[256])

def expand(tokens, out):
    for t in tokens:
        if isinstance(t, int):
            out.append(t)
        else:
            for _ in range(t[0]):
                out.append(out[-t[1]])

def matches(rng, groups, dists):
    """Groups of literals, each followed by a match at a distance drawn
    from dists, never farther back than the output so far."""
    toks, total = [], 0
    for _ in range(groups):
        d = rng.choice(dists)
        lits = max(1, d - total)
        toks += [rng.choice(b"abcdefgh") for _ in range(lits)]
        n = rng.randint(3, 258)
        toks.append((n, d))
        total += lits + n
    return toks

rng = random.Random(2)
blocks = {  # name: [(tokens, bad)] per block
    "dist1":   [(matches(rng, 40, [1]), None)],       # Go on byte runs
    "dist7":   [(matches(rng, 40, [7]), None)],
    "dist8":   [(matches(rng, 30, [7, 8]), None)],    # 1 extra bit
    "dist300": [(matches(rng, 30, range(257, 385)), None)],  # 7 extra bits
    "empty":   [([rng.choice(b"xyz") for _ in range(300)], None)],
    "blocks":  [(matches(rng, 20, [3]), None),
                ([rng.choice(b"xyz") for _ in range(50)], None),
                (matches(rng, 20, [1, 2]), None)],
    "bad-empty":  [(matches(rng, 10, [1]), "empty")],
    "bad-unused": [(matches(rng, 10, [5]), "unused")],
}
nlone = 0
for name, spec in blocks.items():
    w, data = Bits(), bytearray()
    for k, (tokens, bad) in enumerate(spec):
        dynamic(w, tokens, k == len(spec)-1, bad)
        expand(tokens, data)
    raw, data = w.done(), bytes(data)
    d = zlib.decompressobj(-15)
    try:
        ok = d.decompress(raw) == data and d.eof and not d.unused_data
    except zlib.error:
        ok = False
    assert ok == (not name.startswith("bad")), name
    gz = (b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff" + raw +
          struct.pack("<II", zlib.crc32(data), len(data)))
    zl = b"\x78\x01" + raw + struct.pack(">I", zlib.adler32(data))
    for t in ("inflate", "diff-inflate"):
        put(t, f"lone-{name}", raw)
        put(t, f"lone-{name}.gz", gz)
    for fmt, z in enumerate((raw, zl, gz)):
        for piece in range(1, 8):
            put("diff-inflate", f"lone-{name}-f{fmt}p{piece}",
                streaming(fmt, piece, rng.randrange(8)) + z)
            nlone += 1
print(nlone, "lone and empty distance code seeds")

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
