# tugz: tiny unity gzip

A from-specification implementation of gzip ([RFC 1952][]), zlib
([RFC 1950][]), and DEFLATE ([RFC 1951][]) in portable C11, as a drop-in
`gzip` command and a streaming library. It compresses faster than zlib
at every level with equal or better ratios, and validates input exactly
as strictly as zlib.

The core has no dependencies, no global state, no platform
conditionals, and does no I/O. Each program is a unity build: a platform
layer includes the core and supplies a handful of functions.

## Build

POSIX (Linux, macOS, BSD):

    $ cc -O2 -o gzip main_posix.c

Windows, CRT-free (w64devkit):

    $ cc -O2 -fno-builtin -nostartfiles -o gzip.exe main_windows.c -lmemory -lshell32 -lkernel32

Hardware CRC-32 is used automatically: PCLMULQDQ on x86 (detected at run
time), and the CRC instructions on ARMv8 targets that have them.

`make amalgamation` produces `gzip.c`, the Windows build as a single
source file with its build command in the header:

    $ cc -O2 -nostartfiles -o gzip.exe gzip.c -lmemory

## Library

`tugz.h` declares a streaming interface for raw DEFLATE, zlib, and gzip.
The caller provides the memory and does all I/O, passing buffers of any
size; sizes are `ptrdiff_t`.

```c
ptrdiff_t      len = tugz_inflate_size(TUGZ_GZIP);
tugz_inflator *z   = tugz_inflate_init(malloc(len), len, TUGZ_GZIP);
tugz_buf       b   = {in, inlen, out, outlen};
int status = tugz_inflate(z, &b);  // TUGZ_DONE, NEED_INPUT, NEED_OUTPUT, or error
```

Deflate supports SYNC, FULL, and FINISH flushes, and a Lua-style
allocator callback is available in place of caller memory. Build
`libtugz.c` as an object (`make libtugz.o`), or use `make tugz.c` for a
single-file amalgamation with the header inlined.

## Usage

    gzip [-123456789cdfhkqtV] [FILE]...

| Option | Meaning |
|---|---|
| `-1`..`-9` | compression level (default 6) |
| `-c` | write to standard output, keep input files |
| `-d` | decompress |
| `-f` | force: overwrite outputs, follow links, allow terminals |
| `-k` | keep input files |
| `-q` | suppress warnings |
| `-t` | test compressed file integrity |
| `-h`, `-V` | help, version |

Long forms (`--stdout`, `--decompress`, `--best`, ...) are accepted.
Installed (or linked) as `gunzip` it decompresses, and as `zcat` or
`gzcat` it decompresses to standard output.

Behavior follows GNU gzip: concatenated members, warnings for trailing
garbage, exit status 0/1/2 for success/error/warning, metadata copied to
outputs, no partial outputs on failure or interruption, and refusal to
replace links or special files without `-f`. Not yet supported: `-r`,
`-l`, `-v`, `-S`, `-n`/`-N`, and the `GZIP` environment variable.

## Performance

Silesia corpus on Apple M-series, compression ratio @ MB/s:

| | level 1 | level 6 | level 9 | decompress |
|---|---|---|---|---|
| tugz | 34.0% @ 176 | 32.2% @ 102 | 31.7% @ 39 | 1049 |
| zlib | 36.4% @ 166 | 32.2% @ 50 | 31.9% @ 20 | 1256 |
| libdeflate | 34.7% @ 347 | 31.9% @ 147 | 31.5% @ 51 | 1359 |

## Development

    $ make check     # unit and library tests (ASan/UBSan), end-to-end CLI tests
    $ make fuzz      # libFuzzer harnesses, including differential
    $ make bench     # benchmark against zlib and libdeflate

Tests and benchmarks use zlib and libdeflate as references. Tested on
macOS, Linux (x86-64, i386, big-endian PowerPC), and Windows (x86-64,
i686). See [notes.md](notes.md) for design decisions, test coverage, and
the optimization log.

[RFC 1950]: https://www.rfc-editor.org/rfc/rfc1950
[RFC 1951]: https://www.rfc-editor.org/rfc/rfc1951
[RFC 1952]: https://www.rfc-editor.org/rfc/rfc1952
