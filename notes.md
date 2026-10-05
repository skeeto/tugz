# tugz development notes

tugz (tiny unity gzip) is a from-specification implementation of gzip
(RFC 1952), zlib (RFC 1950), and DEFLATE (RFC 1951): a drop-in `gzip`
command, and a streaming library (`tugz.h`). The command identifies
itself as `gzip (tugz) 1.0` and is installed under the name `gzip`.

## Layout

Unity build: each program's platform layer (`platform/*_posix.c`,
`platform/*_windows.c`, `platform/libtugz.c`, and the test programs)
includes the sources it needs and defines their hooks. Everything is
`static` except the entry points. Nothing at file scope in the core is
mutable; platform layers may use globals and `#ifdef`.

The core (`base`, `crc32`, `adler32`, `inflate`, `deflate`, `gzip`) does
no I/O: callers hand it input and output buffers of any size and it
resumes where it stopped. Its only hook is `os_oom`. Programs add
`src/io.c` (the `os_*` file interface and a buffered reader and writer);
gzip adds `src/gzipio.c` (descriptor drivers) and `src/cli.c`. The
library layer adds none of them. The shared `os_*` implementations live
in `platform/posix.c` and `platform/windows.c`.

| File                     | Purpose                                         |
|--------------------------|-------------------------------------------------|
| `src/base.c`             | types, arena, status codes, streaming buffers   |
| `src/crc32.c`            | CRC-32 (slicing-by-8, ARMv8 CRC, x86 PCLMUL)    |
| `src/adler32.c`          | Adler-32                                        |
| `src/inflate.c`          | resumable raw DEFLATE decoder, zlib-exact       |
| `src/deflate.c`          | resumable raw DEFLATE encoder, flushes          |
| `src/gzip.c`             | zlib and gzip containers: decoder, encoder      |
| `src/io.c`               | programs only: `os_*` interface, reader/writer  |
| `src/gzipio.c`           | gzip program: descriptor drivers for the codec  |
| `src/cli.c`              | gzip command line driver, `gzip_main`           |
| `src/zip.c`              | ZIP format: headers, Zip64, parsing, wildcards  |
| `src/zipcli.c`           | zip command line and archive driver, `zip_main` |
| `platform/posix.c`       | shared POSIX `os_*` implementation              |
| `platform/windows.c`     | shared CRT-free Win32 `os_*`, paths, arguments  |
| `platform/gzip_*.c`      | gzip entry points (POSIX, Windows)              |
| `platform/zip_*.c`       | zip file system functions and entry points      |
| `platform/libtugz.c`     | library layer; `tugz.h` is its interface        |
| `test/tests.c`           | test suite (in-memory file system)              |
| `test/libtests.c`        | library interface tests                         |
| `test/fuzz_*.c`          | libFuzzer harnesses, sharing `test/fuzzos.c`    |
| `test/bench.c`           | benchmark versus zlib and libdeflate            |
| `test/ziptests.c`        | ZIP format unit tests                           |
| `test/cli.sh`            | end-to-end tests of the binary                  |
| `test/zip.sh`            | end-to-end zip tests (unzip, zipinfo, Python)   |
| `test/zip_windows.sh`    | zip.exe under Windows' own extractors           |
| `test/seeds.py`          | fuzzing seed corpus generator                   |

The only conditional compilation in the core is CPU architecture and
feature tests in `src/crc32.c`. ARMv8 uses its CRC instructions when
`__ARM_FEATURE_CRC32` is set (by default for Apple and most ARMv8.1+
targets). On x86 with GCC or Clang, a PCLMULQDQ folding function is
compiled with `__attribute((target("pclmul,sse2")))` and selected at run
time via CPUID, so default builds benefit with no flags and still run on
CPUs without it. On an i9-12900, PCLMUL folding runs at 16.8 GB/s versus
3 GB/s for slicing-by-8, making Windows `gzip -d` 25% faster (607 to 762
MB/s on Silesia). Both paths were verified with one binary under QEMU
CPU models with and without PCLMUL.

## Library

`tugz.h` documents the interface. Design points:

- Sizes are `ptrdiff_t`; statuses, formats, levels, and flushes are
  `int`. No `long`, no `size_t`.
- State is fixed-size and lives in caller memory of any alignment
  (`tugz_*_size`, `tugz_*_init`): 310 KB to inflate and 2.7 MB to
  deflate. The optional allocator has the Lua shape
  `(ctx, ptr, old, new)` and is called once to allocate and once to free,
  with the size. Init on the same memory resets. `os_oom` traps in the
  library: init checks the size first, so it is unreachable. Programs
  allocate their codecs from exactly-sized sub-arenas, so every program
  run checks the size calculation.
- Inflate decodes in atomic units: a block header (with a whole dynamic
  table description, at most ~300 bytes) or one literal or
  length/distance pair. If input runs out mid-unit, the unit rolls back
  and its bytes go to a 1 KiB stash, completed by the next call's input.
  Invariant: between calls the bit buffer holds fewer than 8 bits (whole
  bytes are returned to the input), and the stash holds only bytes of one
  incomplete unit. So the stream end is exact: after `TUGZ_DONE`, `in`
  points just past the stream, as zlib's `avail_in` does.
- Errors are reported only once every bit of the offending field is
  present, in zlib's order, so truncation (`TUGZ_NEED_INPUT`) versus
  corruption agrees with zlib at every input length (checked by
  `fuzz-diff-inflate`).
- Deflate stages output (~576 KiB) and parses into tokens only when its
  window fills or at a flush. Each emission step (one block, a window
  slide, or a flush) needs `DEF_STAGE_NEED` bytes of room: a block has at
  most `TOK_CAP` tokens of at most 48 bits, a stored block is chosen only
  when no larger, and held-back stored data is under 64 KiB. Lacking room,
  parsing pauses at the block boundary. Output therefore depends only on
  input bytes and flush points, never on buffer sizes (checked by tests
  and fuzzers).
- SYNC emits an empty stored block (`00 00 ff ff`); FULL also clears the
  hash chains so no match reaches behind the flush. zlib headers match
  zlib's byte for byte (FLEVEL). gzip decoding stops after each member;
  the program's driver applies the GNU trailing-data policy.
- Programs reach the buffers without copying (`*_pending`/`*_consume`),
  so the program's throughput is unchanged by the restructure.
- `make libtugz.o` builds an object exporting only `tugz_*` (no writable
  data). `make tugz.c` produces a single-file amalgamation with the header
  inlined; define `TUGZ_API` as `static` to embed it.

## zip

The zip program shares the deflate core and `src/io.c`, adding a
portable format layer (`src/zip.c`, no I/O, fuzzed) and a driver
(`src/zipcli.c`) over a few more platform functions: `os_stat`,
`os_listdir`, `os_readlink`, positioned `os_readat`/`os_writeat`,
`os_truncate`, `os_commit` (atomic rename over the target), and
`os_localtime`. It needs neither inflate nor the gzip container.

- Scope: batch use by release scripts. Everything interactive or legacy
  (encryption, comments, splits, SFX, fixes, CRLF conversion, streaming,
  logging) is rejected with Info-ZIP's "not supported" usage error
  rather than silently ignored.
- Compatibility: verified field by field against Info-ZIP 3.0 on Linux
  (`-r`, `-rX9`, `-r1`, `-r0`): made-by and needed versions, flags
  (including the level bits: 0x4 for -1/-2, 0x2 for -8/-9, set whenever
  compression was attempted), method, external attributes, local and
  central `UT`/`ux` extra fields, times, and CRCs are byte-identical.
  Messages, warnings, and exit statuses (12 nothing to do, 16 usage,
  18 unreadable files, 3 bad archive) follow Info-ZIP, except warnings
  and errors go to standard error. Info-ZIP's quirks kept: `../` stays
  in names, an emptied archive remains as a 22-byte file, entries that
  do not shrink are stored, odd seconds round up.
- Departures: entries are sorted by name within each directory (Info-ZIP
  uses readdir order), doubled slashes collapse, `SOURCE_DATE_EPOCH`
  clamps times and makes them UTC, and names that are valid non-ASCII
  UTF-8 always get flag bit 11.
- Writing: entries go to a temporary file beside the archive (created
  discard-on-close, like gzip's outputs), at explicit offsets so that a
  local header can be patched once sizes are known. No data descriptors
  are written. An entry that does not shrink is rewritten stored (input
  reopened); one that grows past 4 GiB while being read is redone with a
  Zip64 local header. The file is truncated to its final length and
  renamed over the target; on Windows by handle (`FileRenameInfo`),
  after clearing delete-pending, so it never appears incomplete.
- Merging: the central directory is parsed with every field bounds
  checked; copied entries get regenerated local headers (descriptor flag
  cleared, except for traditionally encrypted entries, whose check byte
  depends on it) and raw data copies. A Zip64 end record is trusted only
  if it checks out or the plain end record calls for it, since bytes
  resembling a Zip64 locator may precede the end record by chance (found
  by fuzzing).
- Windows: made-by host 0 (FAT, the most widely understood), DOS
  attributes, `UT` extra field. Wildcard arguments are expanded per
  component, case-insensitively, without `[sets]` (as Info-ZIP on
  Windows). Recursion skips hidden and system files unless `-S`. Links
  and junctions are followed; cycles are detected by file identity.
- libdeflate issue #323: Windows' zip folder rejects incomplete Huffman
  codes (such as a lone distance code in a block with at most one
  distinct distance), which DEFLATE permits. `huff_build` always codes at
  least two symbols, so every code is complete; `test_complete_codes`
  parses emitted headers to check this, and zip_windows.sh extracts a
  literal-only input (a de Bruijn sequence: no 3-byte repeats) through
  Explorer.

## Workflow

    make check                 # unit and library tests (ASan/UBSan), CLI tests
    SLOW=1 sh test/cli.sh ./gzip   # adds a 5 GiB stream (>4 GiB offsets)
    make gzip.exe              # Win32 build (w64devkit or CROSS=...)
    make fuzz                  # build the five fuzzers
    make fuzz-seeds            # seed corpora in fuzz/corpus/
    ./fuzz-diff-inflate -fork=3 -max_len=65536 fuzz/corpus/diff-inflate
    ./fuzz-zipread -jobs=6 -workers=6 -max_len=8192 fuzz/corpus/zipread
    make bench && ./bench -l 1,6,9 bench_corpus/silesia/*
    make amalgamation          # single-file Windows sources, gzip.c and zip.c
    SLOW=1 sh test/zip.sh ./zip    # adds Zip64: 5 GiB file, 70,000 entries
    sh test/zip_windows.sh ./zip.exe   # on Windows, under w64devkit
    make tugz.c libtugz.o      # single-file library source, library object

Fuzzers:

- `fuzz-inflate`: arbitrary input to raw and gzip decoders
- `fuzz-roundtrip`: deflate then inflate; output must not depend on push
  sizes or stream offset (including past 4 GiB); zlib must agree
- `fuzz-diff-inflate`: exact accept/reject and output agreement with zlib,
  for raw DEFLATE and for multi-member gzip (GNU trailing-data policy);
  then streaming in raw, zlib, and gzip formats with fuzzer-chosen input
  and output piece sizes, which must agree with zlib on success, on
  truncation versus error, on output, and on the exact stream end
- `fuzz-diff-deflate`: zlib (every parameter, flush mode, mid-stream
  parameter change) and libdeflate streams must decode exactly; our
  streaming encoder, in every format with fuzzer-placed NONE/SYNC/FULL
  flushes and piece sizes, must produce piece-independent output that
  decodes under zlib, libdeflate, and our streaming decoder
- `fuzz-zipread`: arbitrary bytes as an existing archive; whatever
  parses is rewritten as a merge would, and must parse back identically

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

- Library: big-endian ppc (also under UBSan), Windows x86-64 and i686,
  and 32-bit Linux under UBSan produce byte-identical compressed output
  to the Mac in all formats, with SYNC flushes and odd buffer pieces.
  `test/libtests.c` passes under ASan/UBSan/LSan in WSL. `tugz.c` builds
  warning-free with GCC and mingw, and `tugz.h` parses as C++.

- zip: `test/zip.sh` passes on macOS (also `SLOW=1`: 5 GiB entries
  compressed and stored, an entry offset past 4 GiB, merging into a Zip64
  archive, 70,000 entries), on aarch64 Linux, and with the big-endian
  ppc build under QEMU. Header fields match Info-ZIP 3.0 exactly (see
  above). `test/zip_windows.sh` passes for the x86-64 build, the
  amalgamation, and the i686 build on Windows 11: Explorer's zip folder,
  `Expand-Archive`, and `tar` extract every level identically, including
  the literal-only input. zip.exe is 68 KiB, imports only KERNEL32 and
  SHELL32, and has no stack frame over 4000 bytes (no `__chkstk`).
- zip speed versus Info-ZIP 3.0 on the 267 MB benchmark corpus (Apple
  M-series): -1 2.2 s vs 1.9 s (4% smaller), -6 3.1 s vs 4.9 s, -9 6.8 s
  vs 12.6 s (smaller). 10,000 small files: 0.26 s vs 0.25 s.

## Behavior decisions

- Decoder strictness matches zlib exactly: incomplete codes rejected except
  a lone 1-bit code; an empty distance code is an error only when used;
  reserved header flags rejected; FHCRC verified.
- Concatenated members decode in sequence. Data after the last member is
  ignored with a warning (exit 2) unless it starts with the gzip magic, in
  which case it must be a valid member. Matches GNU gzip.
- Exit status: 0 success, 1 error, 2 warning; errors take precedence.
- As in GNU gzip, the program name sets the default mode: names starting
  with `un` or `gun` decompress, and `zcat` or `gzcat` decompress to
  standard output (case-insensitive; Windows drops `.exe`). Platform
  layers pass the name in `config.name`.
- In-place operation (no `-c`/`-t`) deletes its input, so its input must
  be a regular file: symbolic links and hard-linked files are skipped with
  a warning unless `-f` (then links are followed); FIFOs, devices, and
  directories are always skipped. With `-c` or `-t`, anything but a
  directory may be read (e.g. `gzip -c <(cmd)`).
- Output files are created owner-only, then given the input's mode,
  ownership (when permitted; set-ID bits dropped otherwise), and
  timestamps at full resolution. On Windows, timestamps are copied and
  access control is inherited from the directory, as with GNU gzip.
- Outputs are discarded unless explicitly kept after success
  (`os_keep`): on failure, on a failed close (which may mean lost data),
  and on interruption. POSIX uses a SIGHUP/SIGINT/SIGTERM handler
  (inherited "ignore" dispositions are respected, as under nohup). Windows marks the file delete-pending at
  creation, so even `TerminateProcess` cleans up.
- `-f` replaces an existing output by unlinking it first, never writing
  through a link.
- Windows paths get the `\\?\` prefix, lifting MAX_PATH. It turns off
  Win32 parsing, so paths are first resolved as Win32 would: against the
  current directory (UNC or not), a drive's, or the root, dropping `.`,
  `..`, and doubled separators. Trailing dots and spaces are kept, so
  names from a listing round trip. The empty path names no file.
- As in GNU gzip, when using standard input, compressed data is not
  written to, or read from, a terminal without `-f`. Named files with
  `-c` are not checked.

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
- streaming exposed timing differences from zlib, invisible when only
  whole streams were compared: an empty code length code is an error
  only at the missing end-of-block code (zlib decodes it as 1-bit zeros);
  a repeat-previous code is validated after its extra bits; gzip magic,
  method, flags, and trailer CRC are checked as soon as each is complete
- `memcpy` with a null pointer and zero length, from callers passing
  empty null buffers (UBSan under GCC)
- zip: bytes resembling a Zip64 locator before a plain end record made
  the reader insist on Zip64 (fuzzing)

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

Streaming restructure (d1de8a5): the program path is unchanged within
noise (TOTAL before/after, same machine and session: compression
175/101/38 versus 177/102/39 MB/s at levels 1/6/9, decompression 1039
versus 1032 MB/s). Through the library interface with 64 KiB input and
output buffers, on Silesia concatenated into one stream: level 1 34.3%
at 180 MB/s, level 5 32.7% at 124 MB/s, level 9 32.0% at 39 MB/s, and
decompression at 946-994 MB/s. The extra copy into the caller's buffer
costs little; the program avoids it via `*_pending`/`*_consume`.

Fuzzing after the restructure: one hour per harness on 16 cores, about
93 million executions in all, no findings beyond the empty code length
timing difference fixed above.
