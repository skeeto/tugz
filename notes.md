# gzip development notes

## Layout

Unity build: each `main_*.c` is a platform layer that includes the core
sources it needs and defines the `os_*` hooks declared in `src/base.c`.
Everything is `static` except the entry point, and nothing at file scope
is mutable, so the core is ready to become a library.

| File                     | Purpose                                         |
|--------------------------|-------------------------------------------------|
| `src/base.c`             | types, arena, `os_*` interface, reader/writer   |
| `src/crc32.c`            | CRC-32 (slicing-by-8, ARMv8 CRC, x86 PCLMUL)    |
| `src/inflate.c`          | raw DEFLATE decoder, zlib-exact validation      |
| `src/deflate.c`          | raw DEFLATE encoder                             |
| `src/gzip.c`             | gzip container (RFC 1952), multi-member         |
| `src/cli.c`              | command line driver, `gzip_main`                |
| `main_posix.c`           | POSIX platform layer                            |
| `main_windows.c`         | CRT-free Win32 platform layer                   |
| `main_test.c`            | test suite (in-memory file system)              |
| `main_fuzz_*.c`          | libFuzzer harnesses, sharing `test/fuzzos.c`    |
| `main_bench.c`           | benchmark versus zlib and libdeflate            |
| `test/cli.sh`            | end-to-end tests of the binary                  |
| `test/seeds.py`          | fuzzing seed corpus generator                   |

The only conditional compilation in the core is CPU feature tests in
`src/crc32.c`: `__ARM_FEATURE_CRC32` (on by default for Apple and most
ARMv8.1+ targets) and `__PCLMUL__` (x86: build with `-mpclmul` or
`-march=native`; no x86-64 baseline level includes it). On an i9-12900,
PCLMUL folding runs at 16.8 GB/s versus 3 GB/s for slicing-by-8, making
Windows `gzip -d` 25% faster (607 to 762 MB/s on Silesia).

## Workflow

    make check                 # unit tests (ASan/UBSan) + CLI tests
    SLOW=1 sh test/cli.sh ./gzip   # adds a 5 GiB stream (>4 GiB offsets)
    make gzip.exe              # Win32 build (w64devkit or CROSS=...)
    make fuzz                  # build the four fuzzers
    make fuzz-seeds            # seed corpora in fuzz/corpus/
    ./fuzz-diff-inflate -fork=3 -max_len=65536 fuzz/corpus/diff-inflate
    make bench && ./bench -l 1,6,9 bench_corpus/silesia/*

Fuzzers:

- `fuzz-inflate`: arbitrary input to raw and gzip decoders
- `fuzz-roundtrip`: deflate then inflate; output must not depend on push
  sizes or stream offset (including past 4 GiB); zlib must agree
- `fuzz-diff-inflate`: exact accept/reject and output agreement with zlib,
  for raw DEFLATE and for multi-member gzip (GNU trailing-data policy)
- `fuzz-diff-deflate`: zlib (every parameter, flush mode, mid-stream
  parameter change) and libdeflate streams must decode exactly; our output
  must decode under libdeflate

## Cross-platform verification

- WSL Debian x86-64 (GCC 14): unit tests under ASan/UBSan/LeakSanitizer,
  `test/cli.sh` for release, sanitized, and `-m32` builds; terminal
  behavior via `script(1)`. A 32-bit build compresses and decompresses a
  3 GiB file (and fails to open it without `_FILE_OFFSET_BITS=64`).
- Windows: the 64-bit and 32-bit (`i686-w64-mingw32`) builds pass
  `test/cli.sh`; `Stop-Process -Force` mid-compression leaves the input
  and no output; console detection checked under ConPTY (`ssh -tt`).

- Windows 11, i9-12900, w64devkit GCC 16: `make gzip.exe` with the real
  CRT-free flags (imports only KERNEL32 and SHELL32), `test/cli.sh`
  against busybox gzip, plain and `-mpclmul` builds. Non-ASCII and
  non-BMP file names, and 396-character paths, work.
- Big-endian: `powerpc-linux-gnu` under QEMU user mode. `test/cli.sh`
  passes under UBSan against GNU gzip 1.14, and compressed output is
  byte-identical to the little-endian build at every level. The build
  before the endian fix produced corrupt output.

## Behavior decisions

- Decoder strictness matches zlib exactly: incomplete codes rejected except
  a lone 1-bit code; an empty distance code is an error only when used;
  reserved header flags rejected; FHCRC verified.
- Concatenated members decode in sequence. Data after the last member is
  ignored with a warning (exit 2) unless it starts with the gzip magic, in
  which case it must be a valid member. Matches GNU gzip.
- Exit status: 0 success, 1 error, 2 warning; errors take precedence.
- In-place operation (no `-c`/`-t`) deletes its input, so its input must
  be a regular file: symbolic links and hard-linked files are skipped with
  a warning unless `-f` (then links are followed); FIFOs, devices, and
  directories are always skipped. With `-c` or `-t`, anything but a
  directory may be read (e.g. `gzip -c <(cmd)`).
- Output files are created owner-only, then given the input's mode,
  ownership (when permitted; set-ID bits dropped otherwise), and
  timestamps. Timestamps are whole seconds: the POSIX nanosecond field
  names differ between macOS and Linux. On Windows, timestamps are copied
  and access control is inherited from the directory, as with GNU gzip.
- Outputs are discarded unless explicitly kept after success
  (`os_keep`): on failure, on a failed close (which may mean lost data),
  and on interruption. POSIX uses a SIGHUP/SIGINT/SIGTERM handler (the
  only global, in `main_posix.c`; inherited "ignore" dispositions are
  respected, as under nohup). Windows marks the file delete-pending at
  creation, so even `TerminateProcess` cleans up.
- `-f` replaces an existing output by unlinking it first, never writing
  through a link.
- Compressed data is not written to, or read from, a terminal without
  `-f`. Stricter than GNU in one case: `gzip -c file` to a terminal is
  refused too.

## Bugs found along the way

- deflate stored hash positions as 32-bit stream offsets and broke past
  4 GiB of input (now window-relative, rebased on slide)
- `deflate_push` overflowed its window for pushes over 1 MiB
- Huffman length limiting could yield invalid (over-subscribed) codes
- a final stored block of exactly k*65535 bytes lacked the final flag
- signed overflow in the inflate distance-1 fill (found by fuzzing)
- big-endian hosts: match length computed with `ctz` on native loads
- decoder accepted streams zlib rejects (incomplete codes, missing
  end-of-block code, reserved flags, bad header CRC)

## Performance log

Silesia corpus (211 MB), Apple M-series, in-memory, gzip format, best of
repeated runs. Compression: ratio and MB/s. Decompression: MB/s, every
decoder on the same zlib -6 stream. Baseline is the original algorithms
(commit 824d6d5, after the restructure).

|                 | level 1      | level 6      | level 9      | decompress |
|-----------------|--------------|--------------|--------------|-----------:|
| baseline        | 33.9% @ 109  | 32.3% @  58  | 32.1% @  37  |   142      |
| **final**       | 34.0% @ 176  | 32.2% @ 102  | 31.7% @  39  |  1049      |
| zlib (macOS)    | 36.4% @ 166  | 32.2% @  50  | 31.9% @  20  |  1256      |
| libdeflate 1.26 | 34.7% @ 347  | 31.9% @ 147  | 31.5% @  51  |  1359      |

Changes, roughly in order of impact:

1. inflate: table-driven decoding (zlib-style root tables plus
   subtables) replacing bit-at-a-time canonical decoding; 64-bit refills;
   fast loop; decode straight into a history window. ~3x.
2. CRC-32: byte table (458 MB/s) to slicing-by-8 (2 GB/s), then ARMv8
   CRC instructions (9 GB/s). Decompression +25% from the latter alone.
3. inflate fast loop: entries carry code+extra bit totals, flag bits for
   kinds, three literals per refill, next lookup preloaded during match
   copies, 32-byte copies, pattern copies for distances 2..7.
4. deflate output: 64-bit bit accumulator spilling 8 bytes at a time
   instead of a function call per byte; code and extra bits combined.
   Level 1 +35%.
5. deflate block costs from symbol frequencies, not per-token walks.
6. deflate block splitting by symbol statistics: ~0.35 points of ratio.
7. Level table re-derived from a parameter sweep (scratch tuning
   harness); level 6 search depth 128 -> 32 kept ratio at zlib parity
   while nearly doubling speed.

Things tried that did not help: inflate chunk size (64 KiB..1 MiB, no
difference), hash table 14..17 bits (16 is best; 17 is ~3% faster within
noise for twice the memory), shallower 3-byte chains at level 1.

Remaining opportunities:

- Decompression is ~83% of zlib and ~77% of libdeflate. The decode loop is
  bound by the lookup->shift->lookup dependency; multi-symbol tables
  (two literals per entry) would be the next step.
- Level 1 is half libdeflate's speed: per-position overhead (two hash
  chain insertions, chain walk) dominates, not search depth. A
  chainless, bucketed hash table for the fast levels is the likely fix.
- High levels: libdeflate's lazy2 and near-optimal parsing reach ~0.2-0.4
  points better ratio.
- x86 PCLMUL CRC is selected at compile time; runtime CPU detection
  (cpuid plus a target attribute) would give it to default builds.

Per-file results (final):

```
file               size  O1:ours        Z1:zlib        L1:libdeflate  O6:ours        Z6:zlib        L6:libdeflate  O9:ours        Z9:zlib        L9:libdeflate   decompress MB/s (O/Z/L)
dickens        10192446   40.2%   152   45.0%   146   41.4%   290   38.0%    61   38.0%    31   37.9%    94   37.6%    34   37.8%    24   37.4%    44      970   1175   1193
mozilla        51220480   38.2%   142   40.2%   139   39.4%   307   37.0%    96   37.3%    43   36.7%   144   36.8%    27   37.2%    11   36.5%    36      876   1017   1139
mr              9970564   37.1%   181   38.4%   187   37.5%   404   36.1%    90   36.9%    35   36.6%   130   36.0%    36   36.7%    13   36.4%    49      961   1152   1209
nci            33553445   11.7%   566   13.8%   414   11.4%   803    9.7%   304    9.5%   139    9.2%   326    8.9%    58    8.9%    26    8.7%    76     2699   3642   4159
ooffice         6152192   51.4%   104   53.5%    99   53.2%   209   50.3%    71   50.3%    33   49.9%   105   50.1%    44   50.3%    21   49.9%    58      624    740    805
osdb           10085684   38.6%   178   40.4%   156   38.4%   359   36.3%   107   36.6%    63   36.3%   164   36.3%    94   36.4%    49   36.2%   132     1105   1266   1441
reymont         6627202   32.2%   194   35.9%   167   33.2%   365   28.7%    75   28.1%    31   28.3%   110   27.4%    18   27.5%    12   27.3%    27     1329   1519   1682
samba          21606400   27.0%   232   29.3%   207   27.2%   435   25.3%   135   25.2%    74   24.7%   178   24.9%    55   25.0%    39   24.4%    57     1364   1600   1794
sao             7251944   74.8%    89   76.8%    79   76.9%   197   73.7%    54   73.5%    31   73.6%    85   73.5%    43   73.4%    26   73.4%    60      554    685    684
webster        41458703   32.4%   182   36.2%   174   32.7%   337   29.9%    92   29.5%    50   29.3%   128   29.1%    39   29.1%    34   28.7%    51     1106   1326   1413
x-ray           8474240   71.4%    85   71.2%    97   74.3%   176   71.3%    74   71.3%    42   70.8%   130   71.2%    72   71.3%    42   70.6%   113      483    602    605
xml             5345280   15.7%   406   18.1%   324   16.6%   628   13.3%   214   12.9%   108   12.8%   261   12.2%    71   12.3%    60   12.2%    88     2354   2985   3437
TOTAL         211938580   34.0%   176   36.4%   166   34.7%   347   32.2%   102   32.2%    50   31.9%   147   31.7%    39   31.9%    20   31.5%    51     1049   1256   1359
```
