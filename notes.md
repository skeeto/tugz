# gzip development notes

## Layout

Unity build: each `main_*.c` is a platform layer that includes the core
sources it needs and defines the `os_*` hooks declared in `src/base.c`.
Everything is `static` except the entry point, and nothing at file scope
is mutable, so the core is ready to become a library.

| File                     | Purpose                                         |
|--------------------------|-------------------------------------------------|
| `src/base.c`             | types, arena, `os_*` interface, reader/writer   |
| `src/crc32.c`            | CRC-32 (slicing-by-8, ARMv8 CRC instructions)   |
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

The only conditional compilation in the core is a CPU feature test
(`__ARM_FEATURE_CRC32`) in `src/crc32.c`.

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

## Behavior decisions

- Decoder strictness matches zlib exactly: incomplete codes rejected except
  a lone 1-bit code; an empty distance code is an error only when used;
  reserved header flags rejected; FHCRC verified.
- Concatenated members decode in sequence. Data after the last member is
  ignored with a warning (exit 2) unless it starts with the gzip magic, in
  which case it must be a valid member. Matches GNU gzip.
- Exit status: 0 success, 1 error, 2 warning; errors take precedence.

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

RESULTS
