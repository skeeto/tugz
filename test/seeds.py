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
