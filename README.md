# tugz: tiny unity gzip/zip

<p align="center"><img src="docs/tugz.png" width="320" alt="tugz mascot: a smiling blue tugboat with tire fenders"></p>

A from-specification implementation of gzip ([RFC 1952][]), zlib ([RFC
1950][]), and DEFLATE ([RFC 1951][]) in C11 with C23 attributes, for GCC
or Clang, as a drop-in `gzip` command, a streaming library, and Info-ZIP
compatible `zip` and `unzip`. It compresses faster than zlib at every
level with equal or better ratios, and validates input exactly as
strictly as zlib.

The core has no dependencies, no global state, no platform conditionals,
and does no I/O. Each program is a unity build: a platform layer
includes the core and supplies a handful of functions.

## Build

POSIX (Linux, macOS, BSD):

    $ cc -O2 -o gzip platform/gzip_posix.c

Windows, CRT-free (w64devkit):

    $ cc -O2 -fno-builtin -nostartfiles -o gzip.exe platform/gzip_windows.c -lmemory -lshell32 -lkernel32

Or with clang targeting MSVC, taking the mem functions from its static
runtime:

    $ clang -O2 -fno-builtin -nostartfiles -Wl,/subsystem:console -o gzip.exe platform/gzip_windows.c -llibvcruntime -llibcmt -lshell32 -lkernel32

Hardware CRC-32 is used automatically: PCLMULQDQ on x86 (detected at run
time), and the CRC instructions on ARMv8 targets that have them.

The zip and unzip programs build the same way from
`platform/zip_posix.c` and `platform/zip_windows.c`, and from
`platform/unzip_posix.c` and `platform/unzip_windows.c`:

    $ cc -O2 -o unzip platform/unzip_posix.c
    $ cc -O2 -fno-builtin -nostartfiles -o unzip.exe platform/unzip_windows.c -lmemory -lshell32 -lkernel32

CMake builds the library, the programs, and the tests (see Development):

    $ cmake -B build && cmake --build build

Each release also carries `tugz-amalgams-VERSION.zip`, which holds
`gzip.c`, `zip.c`, and `unzip.c`, the Windows builds as single source
files with their build commands in the header, which `cmake -P
cmake/amalgamate.cmake` writes from a source tree:

    $ cc -O2 -nostartfiles -o gzip.exe gzip.c -lmemory
    $ cc -O2 -nostartfiles -o zip.exe zip.c -lmemory
    $ cc -O2 -nostartfiles -o unzip.exe unzip.c -lmemory

## Library

`tugz.h` declares a streaming interface for raw DEFLATE, zlib, and gzip.
The caller provides the memory and does all I/O, passing buffers of any
size; sizes are `ptrdiff_t`.

```c
ptrdiff_t      len = tugz_inflate_size(TUGZ_GZIP);
void          *mem = malloc(len);
tugz_inflator *z   = tugz_inflate_init(mem, len, TUGZ_GZIP);  // null if mem is null
tugz_buf       b   = {in, inlen, out, outlen};
int status = tugz_inflate(z, &b);  // TUGZ_DONE, NEED_INPUT, NEED_OUTPUT, or error
// ...
free(mem);  // not z
```

The state lies inside the caller's memory, aligned, so it may begin
past `mem`: keep `mem` to free it. A state holds pointers into itself,
so it may not be copied or moved. A gzip state decodes concatenated
members, returning `TUGZ_DONE` at the end of each. Once input runs
out, `TUGZ_DONE` means the stream (or the last member) is complete and
`TUGZ_NEED_INPUT` that it is truncated; a raw or zlib state stays done,
leaving any data after the stream in `b.in`.

Levels are 1 through 9; others are reserved, and init, `new`, and
reset reject them. Deflate supports SYNC, FULL, and FINISH flushes,
and a Lua-style allocator callback is available in place of caller
memory. To compress many streams, reset a state rather than initialize
it again: `tugz_deflate_reset` (which also sets the level) takes time in
proportion to at most the first 1 KiB of the previous stream's input,
rather than init's clearing of 512 KiB, so 100-byte streams compress
about 4x faster. One in 2,048 of the resets and FULL flushes that follow
a longer history still clears as init does. Inflate init and
`tugz_inflate_reset` both take a small constant time. Build
`platform/libtugz.c` as an object (`cc -c -O2 platform/libtugz.c`), or
use the `tugz.c` in a release's amalgams zip (or `cmake
-DTUGZ_ARTIFACT=tugz -P cmake/amalgamate.cmake`), a single-file
amalgamation with the header inlined. Define `TUGZ_API` as `static`
before including it to embed the library in another program.

`TUGZ_VERSION` is the release as a string, "MAJOR.MINOR.PATCH", and
`TUGZ_VERSION_MAJOR`, `TUGZ_VERSION_MINOR`, and `TUGZ_VERSION_PATCH` its
numbers, for `#if`; `tugz_version()` returns the string the library was
built with, which for a shared library may differ from the header's.

Deflate64 (ZIP method 9, as unzip reads it) is an option, off by
default: compiled with `TUGZ_DEFLATE64` defined (`cc -c -O2
-DTUGZ_DEFLATE64 platform/libtugz.c`, the same before compiling or
including `tugz.c`, or the CMake option below), the library inflates
raw Deflate64 streams as format `TUGZ_RAW64`, with a state of 407 KB
rather than 308 KB. It only decodes: deflate rejects the format.
`TUGZ_RAW64` is declared either way, and a library built without the
option rejects it as any invalid format (`tugz_inflate_size` returns
zero), so the header is the same for both builds and a program can ask
at run time. Without the option, the library is as it was.

### Using the library from CMake

Each release's source tarball can be fetched as a dependency, which
builds only the library, `tugz::tugz`:

```cmake
include(FetchContent)
FetchContent_Declare(tugz
    URL https://github.com/skeeto/tugz/releases/download/v0.4.0/tugz-0.4.0.tar.gz
    URL_HASH SHA256=<the asset's digest on the release page>
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(tugz)
target_link_libraries(app PRIVATE tugz::tugz)
```

Or, after `cmake --install`, `find_package(tugz 0.4 CONFIG REQUIRED)`
provides the same target. The library is static unless
`BUILD_SHARED_LIBS` is set, and position-independent code is the
consumer's choice (`CMAKE_POSITION_INDEPENDENT_CODE`). The targets keep
the compiler's default C standard whatever `CMAKE_C_STANDARD` says, as
C11 alone lacks the C23 attributes. Options:

| Option | Default | Builds |
|---|---|---|
| `TUGZ_BUILD_LIBRARY` | ON | the library |
| `TUGZ_BUILD_GZIP`, `TUGZ_BUILD_ZIP`, `TUGZ_BUILD_UNZIP` | top level | the programs |
| `TUGZ_BUILD_TESTS` | top level, not cross | the tests (CTest) |
| `TUGZ_BUILD_FUZZ` | OFF | the libFuzzer harnesses (LLVM clang) |
| `TUGZ_BUILD_BENCH` | OFF | the benchmark |
| `TUGZ_INSTALL` | top level | install rules and the package |
| `TUGZ_WARNINGS` | top level | warnings for tugz's own targets |
| `TUGZ_SANITIZE` | if supported | ASan and UBSan in the tests |
| `TUGZ_LIBMEMORY` | AUTO | Windows programs with w64devkit's `-lmemory` |
| `TUGZ_DEFLATE64` | OFF | the library with Deflate64 (`TUGZ_RAW64`) |

Built by clang targeting MSVC, the programs link the static runtime's
mem functions, and the shell-script tests run unsanitized programs, as
the sanitizers need the C runtime's startup code.

A plain `cmake --install` installs whatever was built: the library, its
header, and the package, and the programs as `gzip`, `zip`, and `unzip`
in the prefix's `bin`, where they can serve as the system's own.
Configure with `-DTUGZ_BUILD_GZIP=OFF -DTUGZ_BUILD_ZIP=OFF
-DTUGZ_BUILD_UNZIP=OFF` to install the library alone, or install with
`--component programs` for the programs alone.

## Usage

    gzip [OPTION]... [FILE]...

| Option | Meaning |
|---|---|
| `-1`..`-9` | compression level, `--fast` to `--best` (default 6) |
| `-c`, `--stdout` | write to standard output, keep input files |
| `-d`, `--decompress` | decompress |
| `-f`, `--force` | force: overwrite outputs, follow links, allow terminals; with `-dc`, copy data that is not gzip |
| `-k`, `--keep` | keep input files |
| `-n`, `--no-name` | neither save nor restore the name and time (always the case) |
| `-q`, `--quiet` | suppress warnings |
| `--rsyncable` | accepted, without effect (below) |
| `-t`, `--test` | test compressed file integrity |
| `-h`, `-V` | help (`--help`, GNU gzip's, less what tugz lacks), version |

Long options have GNU gzip's other names too (`--to-stdout`,
`--uncompress`, `--silent`), and as there may be abbreviated to any
prefix that is not ambiguous (`--dec --std`).
Installed (or linked) as `gunzip` it decompresses, and as `zcat` or
`gzcat` it decompresses to standard output. On Windows, where `cmd` and
PowerShell pass wildcards on as they are, arguments with them are
expanded as zip expands them, ignoring case, but for hidden and system
files, as a POSIX shell leaves out dotfiles.

Behavior follows GNU gzip: concatenated members, warnings for trailing
garbage (but not zero padding), `zcat -f` passing other data through,
its suffixes (`.gz`, `.z`, `-gz`, `-z`, `_z`, and `.tgz` or `.taz` for
`.tar`), its messages (with the system's reason for a failure), exit
status 0/1/2 for success/error/warning, a stop at the first read or
write error, metadata copied to outputs, and no partial outputs on
failure or interruption. In place, it skips links and sticky files
unless `-f`, and special and set-ID files always, and it reads a header
before replacing a file.

Headers record no file name or time, as GNU gzip's do under `-n`, which
is therefore accepted (as in `gzip -9n`). `--rsyncable` is accepted and
ignored: it asks only for rsync-friendly output, which tugz's is not, but
the result is an ordinary gzip file either way. Not yet supported, and
refused rather than ignored: `-r` (`--recursive`), `-l` (`--list`), `-v`
(`--verbose`), `-S` (`--suffix`), `-N` (`--name`), `-a` (`--ascii`), `-L`
(`--license`), `--synchronous`, the `GZIP` environment variable, and GNU
gzip's other input formats (compress, pack, LZH, zip).

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
| `-v`, `-L`, `-h` | version (alone, or `--version`); license; help (also `-H`, `-?`) |

Long options are Info-ZIP's (`--recurse-paths`, `--strip-extra`, ...)
and, as there, may be abbreviated. Only `-X` may be negated (`-X-`).
`-p` (store paths, the default) has no effect, nor does `-v` with other
arguments. Options in `ZIPOPT`, or else `ZIP`, apply before the
arguments.

Existing archives are merged as Info-ZIP does: matching entries are
replaced in place (keeping their comments), new ones appended, and the
rest copied without recompression, with their extra fields, as `-X`
applies only to entries written. Also as there, `-u` and `-f` without
paths refresh every entry, and a path not on disk, such as a quoted
wildcard, selects the entries it matches. As in Info-ZIP, a file also
matches an entry by its Unicode path field, which Windows tools add to
names they store in a code page, and on Windows by its name decoded from
the OEM code page, in which Explorer stores names, or, if made elsewhere
and not UTF-8, from the ANSI code page. Data before the first entry,
such as a self-extractor's stub or a Python zipapp's `#!` line, is kept
when the archive's offsets account for it (as after `zip -A`), as in
Info-ZIP, while an archive whose offsets do not is refused (3), as
there. The new archive is written to a temporary file and renamed over
the old one, where symbolic links at its path lead, so that they survive
(a dangling link gets its target created). Zip64 is used as needed for
large files, large archives, and more than 65,535 entries. Names are
stored as UTF-8 with flag bit 11 when they are valid UTF-8 and not
ASCII. On Windows, arguments with wildcards are expanded, as `cmd` does
not, except when freshening, as in Info-ZIP.

Headers, attributes, extra fields, messages, and exit statuses match
Info-ZIP's, but for deliberate departures, all listed in
[docs/notes.md](docs/notes.md#departures-from-info-zip). Friendlier exit
statuses include these: `-u` or `-f` with nothing newer exits 0
(Info-ZIP: 12), an unreadable directory or a dangling link met while
recursing exits 18 (Info-ZIP: 0) unless `-x` or `-i` leaves out all it
could add, and `-i` that matches nothing exits 12 (Info-ZIP writes an
empty archive). An archive that can be written but not read is an error
(11), which Info-ZIP replaces as though missing, losing its entries, as
is one that is found but then cannot be examined, and a new archive
never replaces a file made at its path meanwhile. Warnings and errors go
to standard error, without the tab that starts Info-ZIP's warnings on
standard output. Every entry that does not shrink is stored (Info-ZIP
stores only small ones), and none is marked as text. A directory loop
through links is not followed. A hard-linked archive is replaced by a
new file, so its other names keep the old archive. Entries select only
files within the current directory, never by absolute names, `..`, or
links, which they do not follow (with `-y`, a link is stored as one), so
that refreshing an untrusted archive cannot read other files, and an
archive whose entries overlap, as in a zip bomb, is refused rather than
copied. A file that cannot be read to its end is
left out (18), where Info-ZIP stores what it read (0).

Output is deterministic: entries within each directory are sorted by
name, and with `-X` an archive depends only on file contents, names,
attributes, and times. When `SOURCE_DATE_EPOCH` is set, times are
clamped to it and stored in UTC, so the archive does not depend on the
time zone either, and access times (kept without `-X`) are the epoch
itself, as reading the files may change them. `-u`, `-f`, and `-FS`
still compare files' real times, so a file modified after the epoch
always counts as changed (and if it is not, its entry is rewritten byte
for byte).

Other options are rejected (16) rather than ignored, among them the
interactive and legacy features: encryption, comments, splits, adjusting
self-extractors (`-A`, `-J`), `-F` fixes, line ending conversion,
streaming (adding `-`, which `-d` takes as an entry's name, or to
standard output without an archive name), `-T`, `-m`, `-n`, `--out`, and
logging.

## unzip

A subset of Info-ZIP UnZip 6.0 with busybox unzip's features, such as
`unzip -q release-1.2.3.zip -d dist`:

    unzip [-opts[modifiers]] file[.zip] [list] [-x xlist] [-d exdir]

| Option | Meaning |
|---|---|
| `-l`, `-v` | list entries, briefly or verbosely (`-v` alone: version) |
| `-t` | test entries' data |
| `-p`, `-c` | extract to standard output, silently, or naming each entry |
| `-z` | show only the archive comment |
| `-d` | extract into the directory that follows (or `-dDIR`) |
| `-x` | exclude the members that follow |
| `-n`, `-o` | never, or always, replace existing files |
| `-f`, `-u` | freshen existing files only; update them and add new ones |
| `-j` | junk paths: extract every entry into one directory |
| `-C` | match member names ignoring case |
| `-D`, `-DD` | restore no directory times, or no times at all |
| `-V` | keep a `;N` version suffix |
| `-K`, `-X` | keep set-ID and sticky bits; restore owners (POSIX only) |
| `-q`, `-qq` | quiet, quieter |
| `-h`, `-hh` | usage, more help |

Options follow Info-ZIP's grammar: letters combine (`-qo`), a `-` before
one negates it (`--q` cancels a `-q` from the environment), `-d` may
come anywhere, and `-x` takes the members after it. Options in `UNZIP`,
or if it has none `UNZIPOPT`, apply first. As in busybox, an argument
after the archive made only of option letters is taken for options
(`unzip a.zip -o`), and an archive of `-` is read from standard input, a
file in place and a pipe into memory, never replacing a file unless
`-o`, as a prompt's answers would come from the archive. The archive is
tried as named, then with `.zip` and `.ZIP` added, and a name with
wildcards processes each match, in name order, with Info-ZIP's summary.
Members are wildcards, matched against entries' names as Info-ZIP
matches them: `*` matches `/` too, `[...]` is a set, and `\` escapes,
except on Windows, where it separates, as `/` does.

Extraction makes directories as needed, and the `-d` directory one level
deep, as Info-ZIP does. An existing file is never replaced under `-n`,
always under `-o`, and otherwise only once Info-ZIP's question, `replace
NAME? [y]es, [n]o, [A]ll, [N]one, [r]ename: `, is answered on standard
input: `A` and `N` answer for the rest of the run, and the end of input
answers `N`, with a warning (1). `-f` and `-u` compare times as Info-ZIP
does. A file is replaced by removing it, then creating a new one.
Symbolic links are made last, once the files are extracted, and nothing
is written through a link on disk, whether from the archive or already
there: an entry whose path passes through one fails ("exists but is not
directory", 2), and one in the way of a file is removed, not followed.
Only the `-d` directory, and the path to it, may be links. A file whose
data is damaged (a bad CRC, invalid data, more than its size) or cannot
be written is not kept. Modes, times (from extended timestamps, else the
DOS time, local), and under `-X` owners are restored as Info-ZIP
restores them, directories' once their files are in.

Names become paths as Info-ZIP's make them, with its warnings: leading
`/` and `..` components are dropped, `\` separates in a name from MS-DOS
that has no `/`, control characters are dropped, and so is a `;N`
version, unless `-V`. A name is UTF-8 if flag bit 11 says so, or if it
comes from a Unicode path field whose CRC checks out. Otherwise its
bytes are used as they are, except on Windows, where, as Info-ZIP's port
reads them, a name made on MS-DOS (as Explorer's zip folder makes them),
OS/2, or by WinZip on NTFS is decoded from the OEM code page, and any
other that is not UTF-8 from the ANSI code page. Names are shown in
UTF-8, with control characters as `^X`.

On Windows, names are also mapped as Info-ZIP's port maps them: `:`,
`\`, `<`, `>`, `|`, `"`, `?`, and `*` become `_`, and a device's name
(`aux.txt`, `com1`) gets a `_` before it, while trailing dots and spaces
are dropped, and paths may be of any length. Links become files holding
their targets, as in the port, and junctions count as links. Read-only,
hidden, and system attributes are restored, and DOS times are local by
each year's own daylight saving rules. `-K` and `-X` are refused (10).
The console is read and written in UTF-16, so a new name typed at the
prompt may be any Unicode.

Listings show dates as `YYYY-MM-DD`, as Debian's UnZip does; otherwise
messages, the streams they go to, and listing formats are Info-ZIP's, as
are exit statuses: 0 success, 1 warnings, 2 errors in some entries, 3 a
damaged archive, 4 out of memory, 9 no archive found, 10 bad or
unsupported options, 11 no matching entries, 50 a failed write, 51 an
archive cut short, and 81 entries skipped, plus Debian's 12 for
overlapped entries, a zip bomb. Entries may be stored, deflated, or
compressed by Deflate64 (PKWARE's "enhanced deflating", which Explorer's
zip folder uses for large files). Encrypted entries, and those
compressed by other methods (bzip2, LZMA, ...), are skipped (81).

Deliberate departures from Info-ZIP, all listed in
[docs/notes.md](docs/notes.md#departures-from-info-zip-unzip), include
busybox's options after the archive and `-`, overlapped entries found
before any is read, links never followed below the `-d` directory,
damaged and failed files removed rather than kept as written, output
that stops at an entry's size, control characters in comments shown as
`^X`, a new name from the prompt mapped as entry names are, below the
`-d` directory, and on Windows, links made last and junctions never
followed. UnZip's other options are refused (10) rather than ignored:
`-a`, `-B`, `-E`, `-F`, `-i`, `-I`, `-J`, `-L`, `-M`, `-N`, `-O`, `-P`,
`-Q`, `-s`, `-S`, `-T`, `-U`, `-W`, `-Y`, `-Z`, `-$`, `-:`, `-^`, `-2`,
and `-/`, among them text conversion, passwords, ZipInfo mode, and
`-:`'s paths outside the destination.

## Performance

Silesia corpus on Apple M-series, compression ratio @ MB/s:

| | level 1 | level 6 | level 9 | decompress |
|---|---|---|---|---|
| tugz | 34.0% @ 176 | 32.2% @ 102 | 31.7% @ 39 | 1049 |
| zlib | 36.4% @ 166 | 32.2% @ 50 | 31.9% @ 20 | 1256 |
| libdeflate | 34.7% @ 347 | 31.9% @ 147 | 31.5% @ 51 | 1359 |

## Development

    $ cmake -B build && cmake --build build
    $ ctest --test-dir build   # unit, library, and in-memory zip and
                               # unzip tests (ASan/UBSan), then gzip,
                               # zip, and unzip end to end

unzip's tests are `tests-unzip`, its rules against tables recorded from
UnZip 6.0, `tests-unzipcli`, the program in memory (with Windows
conventions too), and `unzip-cli`, which runs `test/unzip.sh` to compare
its statuses, output, and extracted trees with Info-ZIP's (`REF=unzip sh
test/unzip.sh build/unzip`); on Windows, `unzip-windows` runs
`test/unzip_windows.sh` under w64devkit instead (`TUGZ_ZIP=zip.exe sh
test/unzip_windows.sh unzip.exe`), comparing with Explorer, .NET, and
tar. Options add the libFuzzer harnesses, `fuzz-unzip` among them, and
differential ones (`-DTUGZ_BUILD_FUZZ=ON`, with LLVM clang as
`CMAKE_C_COMPILER`), and a benchmark against zlib and libdeflate
(`-DTUGZ_BUILD_BENCH=ON`).

Tests and benchmarks use zlib and libdeflate as references, and zip
archives are verified with unzip, Python's zipfile, and on Windows with
Explorer, .NET, and tar. So the full test run needs zlib, libdeflate, a
reference gzip (`/usr/bin/gzip`, or `-DTUGZ_REF_GZIP=...`), and
Info-ZIP's `unzip` (`-DTUGZ_UNZIP=...`) and `zipinfo`, while Python
(through `uv` if installed) adds checks, and crafts most of unzip's
archives. Configuring warns of a test skipped or disabled for want of
one. Tested on macOS, Linux (x86-64, i386, aarch64, big-endian PowerPC),
and Windows (x86-64, i686). See [docs/notes.md](docs/notes.md) for
design decisions, test coverage, and the optimization log.

[RFC 1950]: https://www.rfc-editor.org/rfc/rfc1950
[RFC 1951]: https://www.rfc-editor.org/rfc/rfc1951
[RFC 1952]: https://www.rfc-editor.org/rfc/rfc1952
