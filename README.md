# tugz: tiny unity gzip

A from-specification implementation of gzip ([RFC 1952][]), zlib
([RFC 1950][]), and DEFLATE ([RFC 1951][]) in portable C11, as a drop-in
`gzip` command, a streaming library, and an Info-ZIP compatible `zip`.
It compresses faster than zlib at every level with equal or better
ratios, and validates input exactly as strictly as zlib.

The core has no dependencies, no global state, no platform
conditionals, and does no I/O. Each program is a unity build: a platform
layer includes the core and supplies a handful of functions.

## Build

POSIX (Linux, macOS, BSD):

    $ cc -O2 -o gzip platform/gzip_posix.c

Windows, CRT-free (w64devkit):

    $ cc -O2 -fno-builtin -nostartfiles -o gzip.exe platform/gzip_windows.c -lmemory -lshell32 -lkernel32

Hardware CRC-32 is used automatically: PCLMULQDQ on x86 (detected at run
time), and the CRC instructions on ARMv8 targets that have them.

The zip program builds the same way from `platform/zip_posix.c` and
`platform/zip_windows.c`.

`make amalgamation` produces `gzip.c` and `zip.c`, the Windows builds as
single source files with their build commands in the header:

    $ cc -O2 -nostartfiles -o gzip.exe gzip.c -lmemory
    $ cc -O2 -nostartfiles -o zip.exe zip.c -lmemory

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
`platform/libtugz.c` as an object (`make libtugz.o`), or use `make tugz.c` for a
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

## zip

A batch-oriented subset of Info-ZIP Zip 3.0 for scripts that package
releases, such as `zip -qX9r release-1.2.3.zip build/`:

    zip [-options] archive[.zip] [path ...] [-x pattern ...]

| Option | Meaning |
|---|---|
| `-0`..`-9` | store only, or compression level (default 6) |
| `-r` | recurse into directories |
| `-q` | quiet: no progress or warnings |
| `-X` | no extra attributes (Unix times, uid/gid) |
| `-@` | read paths from standard input, one per line (UTF-8) |
| `-j`, `-D` | junk directory names; no directory entries |
| `-x`, `-i` | exclude or include only paths matching patterns (before `-j`) |
| `-y` | store symbolic links as links (POSIX only) |
| `-S` | include hidden and system files (Windows only) |
| `-u`, `-f` | update newer entries and add; freshen existing only |
| `-FS` | filesync: update changed entries, delete missing ones |
| `-d` | delete entries matching patterns |
| `-nw` | no wildcards, except `?` (as in Info-ZIP) |
| `-v`, `-L` | version (alone, or `--version`); license |

Long options are Info-ZIP's (`--recurse-paths`, `--strip-extra`, ...)
and, as there, may be abbreviated. Only `-X` may be negated (`-X-`).
Options in `ZIPOPT`, or else `ZIP`, apply before the arguments.

Existing archives are merged as Info-ZIP does: matching entries are
replaced in place (keeping their comments), new ones appended, and the
rest copied without recompression, with their extra fields, as `-X`
applies only to entries written. Also as there, `-u` and `-f` without
paths refresh every entry, and a path not on disk, such as a quoted
wildcard, selects the entries it matches. Data before the first entry,
such as a self-extractor's stub or a Python zipapp's `#!` line, is kept
when the archive's offsets account for it (as after `zip -A`), as in
Info-ZIP, while an archive whose offsets do not is refused (3), as
there. The new archive is written to a temporary file and renamed over
the old one. Headers, attributes,
extra fields, messages, and exit statuses match Info-ZIP's, but for
three friendlier statuses: `-u` or `-f` with nothing newer exits 0
(Info-ZIP: 12), an unreadable directory or a dangling link met while
recursing exits 18 (Info-ZIP: 0), and `-i` that matches nothing exits
12 (Info-ZIP writes an empty archive); and an archive that can be
written but not read is an error (11), which Info-ZIP replaces as
though missing, losing its entries. Zip64 is used as needed for
large files, large archives, and more than 65,535 entries. Names are
stored as UTF-8 with flag bit 11 when they are valid UTF-8 and not
ASCII. On Windows, arguments with wildcards are expanded, as `cmd` does
not, except when freshening, as in Info-ZIP. The archive is replaced
where symbolic links at its path lead, so that they survive, and a
dangling link gets its target created. Hard links do not survive:
other names keep the old archive.

Output is deterministic: entries within each directory are sorted by
name, and with `-X` an archive depends only on file contents, names,
attributes, and times. When `SOURCE_DATE_EPOCH` is set, times are
clamped to it and stored in UTC, so the archive does not depend on the
time zone either. `-u`, `-f`, and `-FS` still compare files' real
times, so a file modified after the epoch always counts as changed (and
if it is not, its entry is rewritten byte for byte).

Interactive and legacy features are not supported and are rejected:
encryption, comments, splits, adjusting self-extractors (`-A`, `-J`),
`-F` fixes, line ending conversion, streaming (with `-`, or to standard
output without an archive name), `-T`, `-m`, `-n`, `--out`, and logging.
Warnings and errors go to standard error rather than standard output.

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

Tests and benchmarks use zlib and libdeflate as references, and zip
archives are verified with unzip, Python's zipfile, and on Windows with
Explorer, .NET, and tar. Tested on
macOS, Linux (x86-64, i386, big-endian PowerPC), and Windows (x86-64,
i686). See [notes.md](notes.md) for design decisions, test coverage, and
the optimization log.

[RFC 1950]: https://www.rfc-editor.org/rfc/rfc1950
[RFC 1951]: https://www.rfc-editor.org/rfc/rfc1951
[RFC 1952]: https://www.rfc-editor.org/rfc/rfc1952
