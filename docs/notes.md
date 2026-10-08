# tugz development notes

tugz (tiny unity gzip) is a from-specification implementation of gzip
(RFC 1952), zlib (RFC 1950), and DEFLATE (RFC 1951): a drop-in `gzip`
command, a streaming library (`tugz.h`), and Info-ZIP compatible `zip`
and `unzip`. The commands identify themselves as `gzip (tugz) 0.2.0`,
`tugz zip 0.2.0`, and `tugz unzip 0.2.0, a subset of Info-ZIP UnZip
6.0`, and are installed under the names `gzip`, `zip`, and `unzip`.

## Layout

Unity build: each program's platform layer (`platform/*_posix.c`,
`platform/*_windows.c`, `platform/libtugz.c`, and the test programs)
includes the sources it needs, and it or the program's own layer
(`src/gzipio.c`, `src/zipcli.c`, `src/unzipcli.c`) defines their hooks.
Everything is `static` except the entry points. Nothing at file scope in
the core is mutable; platform layers may use globals and `#ifdef`.

The core (`base`, `crc32`, `adler32`, `inflate`, `deflate`, `gzip`) does
no I/O: callers hand it input and output buffers of any size and it
resumes where it stopped. Its only hooks are `os_oom` and `os_extend`
(for an arena that runs out). Programs add `src/io.c` (the `os_*` file
interface and a buffered reader and writer); gzip adds `src/gzipio.c`
(descriptor drivers) and `src/cli.c`, and zip `src/zip.c`, `src/wild.c`
(wildcards), `src/dir.c` (directories), `src/zipin.c` (archive reading),
and `src/zipcli.c`, and unzip, which takes only `base`, `crc32`, and
`inflate` of the core, the same, with `src/unzip.c` (its rules) and
`src/unzipcli.c` in place of `src/zipcli.c`. The library layer adds none
of them. The shared `os_*` implementations live in `platform/posix.c`
and `platform/windows.c`, and the file system functions that zip shares
with unzip in `platform/zipfs_*.c`.

| File                     | Purpose                                         |
|--------------------------|-------------------------------------------------|
| `src/base.c`             | types, arena, status codes, streaming buffers   |
| `src/crc32.c`            | CRC-32 (slicing-by-8, ARMv8 CRC, x86 PCLMUL)    |
| `src/adler32.c`          | Adler-32                                        |
| `src/inflate.c`          | resumable DEFLATE/Deflate64 decoder, zlib-exact |
| `src/deflate.c`          | resumable raw DEFLATE encoder, flushes          |
| `src/gzip.c`             | zlib and gzip containers: decoder, encoder      |
| `src/io.c`               | programs only: `os_*` interface, reader/writer  |
| `src/gzipio.c`           | gzip program: descriptor drivers for the codec  |
| `src/cli.c`              | gzip command line driver, `gzip_main`           |
| `src/zip.c`              | ZIP format: headers, Zip64, parsing, names      |
| `src/wild.c`             | wildcard matching, as Info-ZIP's                |
| `src/dir.c`              | directory listings, wildcard expansion on disk  |
| `src/zipin.c`            | archive reading, shared by zip and unzip        |
| `src/zipcli.c`           | zip command line and archive driver, `zip_main` |
| `src/unzip.c`            | unzip's rules: names to paths, times, modes     |
| `src/unzipcli.c`         | unzip command line and driver, `unzip_main`     |
| `platform/posix.c`       | shared POSIX `os_*` implementation              |
| `platform/windows.c`     | shared CRT-free Win32 `os_*`, paths, arguments  |
| `platform/gzip_*.c`      | gzip entry points (POSIX, Windows)              |
| `platform/zipfs_*.c`     | file system functions for zip and unzip         |
| `platform/zip_*.c`       | zip's archive replacement and entry points      |
| `platform/unzip_*.c`     | unzip's extraction hooks and entry points       |
| `platform/libtugz.c`     | library layer; `tugz.h` is its interface        |
| `test/tests.c`           | test suite (in-memory file system)              |
| `test/libtests.c`        | library interface tests                         |
| `test/fuzz_*.c`          | libFuzzer harnesses: codec ones share           |
|                          | `fuzzos.c`, `fuzz_zip.c` uses `zipos.c`,        |
|                          | `fuzz_unzip.c` `unzipos.c`                      |
| `test/bench.c`           | benchmark versus zlib and libdeflate            |
| `test/ziptests.c`        | ZIP format unit tests                           |
| `test/zipclitests.c`     | zip program tests, in memory                    |
| `test/zipos.c`           | in-memory platform layer for the zip program    |
| `test/unziptests.c`      | unzip rule tests, with tables from UnZip 6.0    |
| `test/unzipclitests.c`   | unzip program tests, in memory                  |
| `test/unzipos.c`         | in-memory platform layer for the unzip program  |
| `test/cli.sh`            | end-to-end gzip tests                           |
| `test/zip.sh`            | end-to-end zip tests (unzip, zipinfo, Python)   |
| `test/zipcheck.py`       | zip.sh's verifier through Python's `zipfile`    |
| `test/zip_windows.sh`    | zip.exe under Windows' own extractors           |
| `test/unzip.sh`          | end-to-end unzip tests against Info-ZIP's UnZip |
| `test/unzipcraft.py`     | unzip.sh's crafted archives (Python)            |
| `test/unzip_windows.sh`  | unzip.exe beside Windows' own extractors        |
| `test/pty.cs`            | pseudo console for the Windows tests' prompts   |
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
CPU models with and without PCLMUL. CPUID is asked once per codec state
(and zip run), which keeps the answer, not on each update: a hypervisor
traps it, as under Windows 11 with virtualization-based security (620 ns
on the i9-12900) and in WSL2 (650 ns). Library inflate into 256-byte
output buffers there ran at 258 MB/s, and now at 772, and into 4 KiB at
698, now 790.

## Library

`tugz.h` documents the interface. Design points:

- Sizes are `ptrdiff_t`; statuses, formats, levels, and flushes are
  `int`. No `long`, no `size_t`.
- State is fixed-size and lives in caller memory of any alignment
  (`tugz_*_size`, `tugz_*_init`): 308 KB to inflate and 2.7 MB to
  deflate. The optional allocator has the Lua shape `(ctx, ptr, old,
  new)` and is called once to allocate and once to free, with the size.
  Init on the same memory starts over, as does a reset, which for
  deflate is far cheaper (below). `os_oom` traps in the library: init
  checks the size first, so it is unreachable. Programs allocate their
  codecs from exactly-sized sub-arenas, so every program run checks the
  size calculation.
- Inflate decodes in atomic units: a block header (for a dynamic block,
  up to its code lengths: at most 74 bits), a run of code lengths, or
  one literal or length/distance pair. If input runs out mid-unit, the
  unit rolls back and its bytes go to a 32-byte stash, completed by the
  next call's input. Invariant: between calls the bit buffer holds fewer
  than 8 bits (whole bytes are returned to the input), and the stash
  holds only bytes of one incomplete unit. So the stream end is exact:
  after `TUGZ_DONE`, `in` points just past the stream, as zlib's
  `avail_in` does. The code lengths resume where they stopped, as zlib's
  do, where a whole dynamic header (up to ~300 bytes) once was one unit,
  decoded again from its first bit on every call: fed a byte at a time,
  a stream of 20,000 header-only blocks decoded at 355 ns per input byte
  (zlib 14), now 25.
- Errors are reported only once every bit of the offending field is
  present, in zlib's order, and a unit stops at the first field input
  cannot finish, never looking up the next one in the bits at hand. So
  truncation (`TUGZ_NEED_INPUT`) versus corruption agrees with zlib at
  every input length (checked by `fuzz-diff-inflate`, and by
  `test_inflate_splits` at every split of streams with lone and empty
  distance codes, whose invalid 1-bit entries expose an early lookup).
  That includes zlib streams with a preset dictionary (FDICT), which
  tugz does not support: `TUGZ_EHEADER` comes once the 4-byte dictionary
  ID is in, where zlib asks for the dictionary, and not sooner, at the
  flag.
- Inflate decodes ahead of the caller's output buffer, into its window
  (up to 256 KiB), so a call returns `TUGZ_NEED_OUTPUT` whenever decoded
  output remains, even with the input used up or an error found. Any
  other status means everything decoded has been delivered, as with
  zlib, which decodes no further than its output buffer. A caller can
  then wait for input at `TUGZ_NEED_INPUT` (an interactive SYNC-flushed
  stream) or stop there (a truncated file) without losing output, and
  gets all output before an error. `test_held_output` checks prefixes of
  streams, and of broken ones, against zlib with small buffers, and
  `fuzz-diff-inflate` checks the output at truncation and at errors.
  Programs take output copy-free, so for them `GZ_NEEDOUT` only asks
  them to take it and call again.
- Deflate stages output (~576 KiB) and parses into tokens only when its
  window fills or at a flush. Each emission step (one block, a window
  slide, or a flush) needs `DEF_STAGE_NEED` bytes of room: a block has
  at most `TOK_CAP` tokens of at most 48 bits, a stored block is chosen
  only when no larger, and held-back stored data is under 64 KiB.
  Lacking room, parsing pauses at the block boundary. Output therefore
  depends only on input bytes and flush points, never on buffer sizes
  (checked by tests and fuzzers).
- SYNC emits an empty stored block (`00 00 ff ff`); FULL also clears the
  hash chains so no match reaches behind the flush. zlib headers match
  zlib's byte for byte (FLEVEL). gzip decoding stops after each member;
  the program's driver applies the GNU trailing-data policy.
- A flush falls due once its call has consumed all input, and later
  calls complete it before anything else, whatever their mode. A caller
  may thus move on before `TUGZ_DONE`, as to FINISH at the end of input,
  and get the stream it would have had by waiting (zlib's documentation
  requires repeating the mode). Once FINISH is due, other calls get
  `TUGZ_EUSAGE`.
- Of the deflator's large tables only the hash heads start zeroed:
  tokens are written before use, and chains lead only to links that
  insertions wrote. A slide rebases every link, written or not, so it
  neither branches on one nor passes one to a call, either of which
  MemorySanitizer reports (as it did at -O0). Forgetting history (a FULL
  flush, or `deflate_reset` before a new stream) must empty the heads.
  After at most 1024 inserted positions, it rehashes them and clears
  their slots. After more, rather than clear 512 KiB, it advances a
  stamp: each hash table entry is its position plus one plus the stamp,
  which moves in steps of 2^21, more than any window position plus a
  window, and `find_match` measures distances from p's own entry, so an
  older entry lies more than a window back, even after a reset restarts
  positions at zero, and ends a chain as an empty one does. Slides zero
  such entries, and after 2,047 advances the heads are zeroed and the
  stamp starts over. A reset or FULL flush thus costs at most a
  1024-position rehash, and one in 2,048 of those after a longer history
  costs the clearing. Sampling for 3-byte matching keeps its own 8 KiB
  bitmap, since the 3-byte heads it borrowed may now hold forgotten
  entries. zip resets one deflator per entry, gzip one encoder per file,
  and the library exposes this as `tugz_deflate_reset`; a fresh deflator
  still zeroes 512 KiB. The reset also sets the level, which only
  selects parameters and the zlib header, so `tugz_deflate_size` takes
  only the format. Per 100-byte gzip stream (M4 Max / Pi 4): deflate
  11.4 / 78 us after init, 2.5 / 21 us after reset.
- An inflator starts uncleared: the fixed codes' decoding tables are
  constants (2 KiB, which `test_tables` checks against `htable_build`),
  and every other field is written before it is read, so init sets only
  what `tugz_inflate_reset` does. On the M4 Max / Pi 4, init took 2.0 /
  12.6 us, clearing 14 KiB and building the fixed tables, and now takes
  3 / 39 ns. A 100-byte gzip stream decodes in 1.2 / 7.3 us after init
  or reset, where it took 3.2 / 19.9 us after init. The library object
  grew by 2 KiB (+5%) of constant tables. gzip -d and -t reset one
  decoder per file, as compression does its encoder, though with init
  this cheap that saves nothing measurable: over 20,000 gzipped 200-360
  B slices of dickens, `-dc` took 0.38 / 0.80 s of CPU with the old init
  and 0.31 / 0.54 s with the new, with or without the reuse.
- Programs reach the buffers without copying (`*_pending`/`*_consume`),
  so the program's throughput is unchanged by the restructure.
- Deflate64 (ZIP method 9, for unzip; not in the library's interface)
  shares the decoder. It differs in a 64 KiB window, length code 285 (16
  extra bits over a base of 3, lengths up to 65538, rather than 258),
  and distance codes 30 and 31 (base 32769 and 49153, 14 extra bits).
  An entry's value has 15 bits, so those bases set a flag, `F_HI` (bit
  5, unused before), worth 32768. `inflate64_new` claims a window with
  64 KiB of history and slack for a whole 65538-byte match plus the
  copy's overrun, so no match is ever split and the careful path still
  writes a unit whole, and a distance table for 32 symbols (at most 768
  entries: a subtable of 2^k entries holds at least k+1 codes);
  `inflate_new`'s inflator is as it was. The fast loop is one function,
  always inlined, with the format a constant parameter, compiled as
  `decode_fast` and `decode_fast64`; Deflate's has no added instruction.
  Deflate64's refills once more after a length of more than 20 bits
  (only code 285's), as a pair may then take 60 bits, and adds `F_HI`
  to distances. Its fixed blocks build their tables, which differ from
  DEFLATE's constant ones, into the dynamic tables' room. `copy_match`
  is always inlined too: called from two loops, clang made it a call,
  which cost Deflate 2% on Silesia. Deflate64 is tested with hand-built
  streams (`test_deflate64`: whole, at every split, a byte at a time,
  with output in pieces, across window slides) and fuzzed by
  `fuzz-inflate` against itself in pieces, there being no reference.
- `platform/libtugz.c` builds an object, or the CMake target
  `tugz::tugz` (static, or shared with `BUILD_SHARED_LIBS`), exporting
  only `tugz_*` (no writable data): the core's other functions are all
  static, so ELF, Mach-O, and MinGW DLL builds export no more without a
  visibility attribute. `cmake/amalgamate.cmake` (a `cmake -P` script,
  sorting by byte, so the same in every locale) produces `tugz.c`, a
  single-file amalgamation with the header inlined; define `TUGZ_API` as
  `static` to embed it. Around the core it saves and restores
  (`push_macro`, `pop_macro`) every name the core defines as a macro, so
  its macros neither leak into the program nor replace the program's:
  its `assert` once turned a program's assertions into optimizer
  assumptions, even under `NDEBUG`. Its type names become `tugz__` ones,
  as they collide with common headers: `<windows.h>` defines `byte`, and
  on LP64 a program's `int64_t i64` is `long`. Static functions and
  enumerators still share the program's names (documented in `tugz.h`).
  `test/amalgtests.c` (ctest's `amalgamation`) embeds it beside a
  program's own `assert`, `MIN`, `i64`, and `byte`.
- The targets leave `C_STANDARD` unset, so the compiler's default (gnu17
  or later, which accepts C23 attributes) applies even when a consumer
  sets `CMAKE_C_STANDARD` to 11, and compile at `-O2`, as the
  performance figures were measured, rather than Release's `-O3`.

## zip

The zip program shares the deflate core and `src/io.c`, adding a
portable format layer (`src/zip.c`, no I/O, fuzzed), wildcard matching
and directory listings (`src/wild.c`, `src/dir.c`), an archive reader
(`src/zipin.c`), and a driver (`src/zipcli.c`), also tested and fuzzed
over an in-memory platform layer (`test/zipos.c`), over more platform
functions, declared at the top of `src/zipin.c`, `src/zipcli.c`, and
`src/dir.c`: `os_stat`, `os_fstat`, and `os_missing` (whether a failed
`os_stat` found nothing there), `os_listdir`, `os_readlink`, positioned
`os_readat`/`os_writeat`, `os_truncate`, `os_resolve` (the file that
links at the archive's path lead to), `os_writable` (whether the archive
may be replaced), `os_commit` (atomic rename over the target, only once
the file is closed, or on Windows flushed, without error),
`os_localtime`, `os_isatty`, `os_error` (the last failure's reason),
and, needed only on Windows (POSIX stubs them), `os_fromcp` (a name in a
code page), `os_fullpath` (a file's final path, to tell files apart
without IDs), and `os_upcase` (a name in upper case, as the file system
ignores case). It needs neither inflate nor the gzip container.

- Scope: batch use by release scripts. Other options, everything
  interactive or legacy among them (encryption, comments, splits, SFX
  adjustment, fixes, CRLF conversion, streaming, logging), are rejected
  with Info-ZIP's "not supported" usage error rather than silently
  ignored. Deliberate departures from Info-ZIP are listed at the end of
  this section.
- Compatibility: verified field by field against Info-ZIP 3.0 on Linux
  (`-r`, `-rX9`, `-r1`, `-r0`): made-by and needed versions, flags
  (including the level bits: 0x4 for -1/-2, 0x2 for -8/-9, set whenever
  compression was attempted), method, external attributes, local and
  central `UT`/`ux` extra fields, times, and CRCs are byte-identical,
  but for the method and version needed of an incompressible file over
  32 KiB, and for Zip64, the end records and the order of extra fields
  (see the departures). Symbolic links (`-y`) are always stored, never
  compressed, as in Info-ZIP. So, below `-9`, are files whose names end
  in a suffix of its default `-n` list (`.Z .zip .zoo .arc .lzh .arj`,
  ignoring case only on Windows), with no level flag bits, as it does
  not try to compress them; `-n` itself is rejected. Messages, warnings,
  and exit statuses (12 nothing to do, silently for `-u` and `-f`; 16
  usage; 18 unreadable files; 3 bad archive; 10 temporary file failure,
  in creating it or, as a deferred write error, closing it; 15 for an
  archive that cannot be created or replaced, "was replacing the
  original zip file") follow Info-ZIP, but for the departures below. As
  there, an I/O error (10, 11, 14, 15, 18) first gives the system's
  reason, worded by `strerror` ("zip I/O error: Permission denied"),
  when there is one; on Windows, as its C runtime words the errno to
  which its `_dosmaperr` maps the error: every error, so that an unknown
  share is "No such file or directory" and a link loop or a network
  error "Invalid argument" (the table checked against msvcrt and the
  UCRT on Windows 11; the port itself inferred from its sources, as it
  uses the runtime's `strerror`). A read error in the archive is its
  "Input file read failure" (11), and one that ends early, having shrunk
  while zip works, is its "Unexpected end of zip file" (2), either
  naming the entry being copied ("was copying a.txt"), else the archive.
  A read-only archive fails with 15 once there is something to do,
  before doing it, as Info-ZIP finds by opening it to update it
  (replacing it needs no permission to write it): as `access` judges on
  POSIX, and on Windows by the read-only attribute, which would
  otherwise refuse the rename only after all the work. So does, on
  Windows, an archive that another process holds open without sharing
  delete access, which the rename needs (by its sources, Info-ZIP's port
  refuses it up front only if that process also refuses reading or
  writing, and otherwise at the end, 15 either way): zip finds this by
  opening the archive with that access, and holds it so until the
  rename, so that no process can open it that way in the meantime. One
  that can be neither read nor written is, as in Info-ZIP, taken for a
  missing archive, which then cannot be written. So is a path that
  cannot be examined for any reason but nothing there, as Info-ZIP fails
  to create it up front: one that `stat` refuses on POSIX, and on
  Windows a name that no file can have (`a<b.zip`, a component over 255
  characters), one that another process holds delete-pending, or a share
  or server not found (a name with a colon, as `rel-12:30.zip`, is still
  found invalid only by the rename, after the work, since Windows takes
  it for a stream of a file not found). A missing or empty archive gets
  Info-ZIP's "not found or empty" warning under `-u`, `-f`, and `-d`,
  which go on with their arguments (warning of unmatched names,
  rejecting repeated ones). A file that cannot be added still gets its
  progress line, then the system's reason as Info-ZIP's `perror` gives
  it (even under `-q`, as there), then a warning under its entry's name
  that tells a failed open from a failed read. Should writing the
  archive fail, its progress line lacks the result, and the error is
  Info-ZIP's "Output file write failure (write error on zip file)". The
  "Not all files were readable" summary, before any "zip file empty", as
  there, counts the files and entries read and skipped as Info-ZIP does,
  with its abbreviated byte counts ("292K"), in the same words.
  Info-ZIP's quirks kept: `../` stays in names, an emptied archive
  remains as a 22-byte file, odd seconds round up (but not past
  `SOURCE_DATE_EPOCH`). Times before the DOS range clamp to its start
  (and after it, a departure, to its end). A name over 65,535 bytes
  (possible in deep Windows paths) is skipped with a warning, exiting
  18, as is, its entry kept, a file that an entry names by its name
  decoded from the OEM code page (Windows), which UTF-8 can make that
  long.
- Options: Info-ZIP's grammar and names. Long names may be abbreviated
  to a prefix of exactly one of Info-ZIP's long names (supported or not,
  so `--rec` is ambiguous). Only `-X` is negatable; other negations and
  values on options without them are errors, quoting Info-ZIP's
  descriptions. `-y` exists only on POSIX and `-S` only on Windows, as
  in Info-ZIP's builds, whose long names differ too: on Windows, there
  is no `--symlinks`, and the port's `--archive-clear`, `--archive-set`,
  `--ignore-case`, and `--use-privileges` (unsupported) make `--i`
  ambiguous. Its two-letter short options (those of its Windows port
  there) are matched before single letters, so that unsupported ones are
  rejected by name (`-fd`), `-mm` with Info-ZIP's own message
  ("Must_Match is -MM"), and `-h2` is `-h`. A pattern list, `-x` or
  `-i`, is its attached value alone, if it has one (`-x=` and
  `--exclude=` attach an empty pattern, which matches nothing), and
  otherwise the next argument, whatever it is (`-x -dash`), and those
  after it up to one that starts with `-` (a lone `-` included) or a
  lone `@`, which is dropped. An action, `-u`, `-f`, or `-d`, may be
  given once ("specify just one action"), and `-FS`, a flag in Info-ZIP,
  may be repeated but not combined with one (its message, but for a
  stray line break before the closing parenthesis). `--` ends options
  only after the archive name. `ZIPOPT`, or if it holds only whitespace
  `ZIP`, supplies options before the arguments, split as Info-ZIP's
  envargs does: at whitespace, except that a word starting with a double
  quote runs to the next one, both dropped; on POSIX, a backslash in it
  is dropped too, keeping the byte after it (`"a\"b\\c"` is `a"b\c`),
  while Info-ZIP's Windows port, by its sources, keeps backslashes, its
  path separators. A lone `-v` (after those) or `--version` prints the
  version, `-L` the license (the Unlicense), `-h` (or Info-ZIP's `-H`
  and `-?`) the usage; `-v` with other arguments (verbose) does nothing,
  nor does `-p` (store paths, the default). With no arguments and no
  terminal on standard output, or no archive name, Info-ZIP streams to
  standard output; that is rejected as streaming. A terminal gets the
  usage, or Info-ZIP's "cannot write zip file to terminal". A `-` path,
  which Info-ZIP reads from standard input, is rejected as streaming
  too, but under `-d`, as there, it names the entry that reading makes
  (`zip -d a.zip -`), never a file. `-d` warns, as Info-ZIP does, that
  `-r` and `-0` are ignored.
- Names: as Info-ZIP's ex2in makes them, `/` and `./` prefixes are
  dropped and `../` kept, and on POSIX too a leading `//host/share/` is
  dropped (`zip t.zip //h/s/f` stores `f`). A directory is named with
  its separator, as procname names it, so a share root `//h/s` is named
  by nothing, like `//h/s/`, and so are its entries' prefixes (`zip -r
  t.zip //h/s` stores `f`). As procname names them, the entries of `.`
  have paths without `./` (`d/a`, in messages too). The same path
  reached twice (`f f`, `d d/a`, `d d/`, `. d/a`, `find d | zip -r@`) is
  added once, silently, as in Info-ZIP; different paths giving one name
  (`./d/a d/a`, `. ./d/a`, or a `-j` collision) are an error (16),
  reported as there once every path is scanned and matched: only the
  first name repeated in order of names, by the first two of its
  different paths in order (a directory's with a slash; `a ./a ./a`
  gives `./a` and `a`), in one warning whose lines Info-ZIP indents to
  line up past its tab. Also as there, a file whose name is the
  archive's path as given (with `.zip` added) is left out silently even
  when it is another file, as `-j` may name it (`zip -j dist.zip
  build/dist.zip`).
- Entry names in code pages: files match an entry by its stored name,
  then, as in Info-ZIP, by an Info-ZIP Unicode path field (0x7075) whose
  CRC is the stored name's, as Info-ZIP's Windows port, WinZip, and
  7-Zip write one for a name not stored as UTF-8, but by that only an
  entry that no file matches by its stored name (see the departures).
  Info-ZIP warns of a stale field, of version 0 or 1 (it skips later
  ones with another warning) but without that CRC or too short to hold
  one, in an entry without flag bit 11, and so does tugz, naming the
  entry where Info-ZIP prints "(null)" (and a stray debugging line on
  standard output, `unicode_mismatch = 1`, even under `-q`); it warns
  again of one in a local header, which tugz does not read until
  copying, and then ignores. On Windows, as in Info-ZIP's port, a name
  without flag bit 11 or such a field, made on DOS or Windows (or OS/2,
  or WinZip's NTFS), is decoded from the OEM code page
  (`MultiByteToWideChar`), as Explorer's zip folder stores names, and
  any other that is not UTF-8 from the ANSI code page, in which the port
  keeps names, as PKZIP for Windows (2.5, 2.6, 4.0) stored them (the
  port takes UTF-8 ones for ANSI too, where tugz takes them for UTF-8).
  There patterns also match the decoded name, which names the file that
  `-u`, `-f`, and patterns select. A replaced entry is written under the
  Unicode name, flagged UTF-8; copied entries keep their bytes. Messages
  give the Unicode name. Elsewhere, as in Info-ZIP's Unix port, patterns
  match stored names only, and an entry they select names its file by
  its stored name.
- Selection: as Info-ZIP's procname does, a path not on disk is a
  pattern for the archive's entries (taken up after the paths on disk,
  which come first), and `-u` and `-f` without paths select every entry.
  Info-ZIP's Windows port takes such patterns only when freshening;
  otherwise it expands wildcards on disk and stops there, and so does
  tugz. The file a selected entry names is examined without recursion,
  `-D`, or `-j` (which does not cut the pattern either), if the name
  passes `-i` and `-x` and stays within the current directory (see the
  departures). Leading `./`, which bsdtar and Windows' `tar` write,
  stays there, so those entries are refreshed, as in Info-ZIP, a `./`
  entry by the current directory. As there, every entry selected is
  refreshed, each of an archive's entries with one name (as Python's
  `zipfile` appends them) included, while a path names only the first
  (Info-ZIP's binary search finds any one). A missing file leaves its
  entry (deleted by `-FS`). One that has changed between file and
  directory keeps it, with Info-ZIP's warning and status 18. Special
  files, named, met while recursing, or named by an entry (which is then
  left as for a missing file, where Info-ZIP would wait reading a FIFO),
  are left out with Info-ZIP's warnings ("ignoring special file: ", for
  a FIFO "ignoring FIFO (Named Pipe)"), which leave the exit status
  alone (12 if nothing else is left), and a `-d` name that is one on
  disk marks nothing. A pattern without wildcards, such as a `-d` name
  of a file gone from disk, is looked up by name rather than matched
  with every entry, unless some names share the index's slots
  (duplicates, or names that differ only in case on Windows, or by
  Unicode names): `-d` of 2,000 such names from 100,000 entries took 3.1
  s, now 0.02 s (Info-ZIP: 1.7 s).
- Updates: as in Info-ZIP, `-u` and `-f` take a file that is newer than
  its entry by the Unix time of the entry's last `UT` field, if that has
  one, or failing any, of an old `UX` field (Info-ZIP 2's), so the time
  zone does not matter, else by DOS times; `-FS` replaces one whose DOS
  time or size differs (time zone and all), and reports "Archive is
  current" when nothing changed. Under `SOURCE_DATE_EPOCH`, files' times
  are compared unclamped, so a file modified after the epoch is always
  newer than its entry, which the clamped archive could not record; an
  unchanged one is rewritten with identical bytes. At an odd epoch, a
  time clamped to it rounds down, and so does one equal to it when
  compared, so a file modified at the epoch is not newer, and one a
  second later is.
- Patterns: `-x`, `-i`, and `-d` patterns, `@file` lines included, are
  normalized as names are, so that `./` and `/` prefixes (and on
  Windows, backslashes and drives) match. Matching follows Info-ZIP's
  recmatch, quirks included: the first unescaped `]` closes a set, a
  byte before `-` only starts a range, an unclosed set or a trailing
  backslash matches nothing, a trailing `**` needs a byte, and after a
  `*` followed by no wildcards the rest is compared literally. `-nw`
  leaves `?` a wildcard. Filters see paths as before `-j`, and a `-d`
  name on disk is taken literally, but under `-j`, as Info-ZIP names it,
  by its last part (a directory then names nothing), so that `zip -dj
  t.zip d/f` deletes the `f` that `zip -j t.zip d/f` added. `-@` and
  `@file` lines are read as Info-ZIP's getnam does (any CR or LF ends a
  line, a NUL ends a name), and `-@` names come before the arguments. A
  directory, as an `@file` or as standard input, is an empty list, as
  Info-ZIP's getc fails at once to read it on POSIX; Windows' C runtime
  refuses to open one as an `@file`, so there that fails (18) with the
  reason Info-ZIP's port gives, "Permission denied". As in Info-ZIP,
  patterns with nothing to select from, no paths (even from `-@`) unless
  `-u` or `-f` selects entries, are a usage error, found before the
  archive is read.
- Writing: entries go to a temporary file beside the archive (created
  discard-on-close, like gzip's outputs), at explicit offsets so that a
  local header can be patched once sizes are known: new entries need no
  data descriptors (some copied ones keep theirs: see Merging). An entry
  that does not shrink is rewritten stored, from the input buffer if one
  read got it all, else by reopening the input; one that grows past 4
  GiB while being read is redone with a Zip64 local header. A size of
  exactly 0xffffffff gets Zip64 too, as APPNOTE reserves that value, and
  a central Zip64 extra, once there is one, always holds both sizes
  (their 32-bit fields saturated), then the offset if it overflows.
  APPNOTE allows the offset alone, as Info-ZIP writes it, but having
  read a size of exactly 0xffffffff from one Zip64 extra, UnZip 6.0
  takes every later one to start with a size, so that a later entry's
  offset became its size. A file is opened only as a regular file, so a
  FIFO swapped in since the scan fails as unreadable (18) rather than
  block, and under `-y` only if it is still the file the scan found, by
  identity, so that nothing is read through a link swapped in for it or
  for a directory above it once it was found. The scan itself goes by
  path, as Info-ZIP's does, so a directory swapped for a link while it
  is being listed, or its entries examined, leads the scan, and then the
  reading, through the link (making it safe would take listing and
  examining relative to an open directory). Likewise a link's target,
  read by path, is stored only if the link is still the one scanned.
  Output is buffered 1 MiB at a time, and rewinding to an entry's start
  or patching its header stays in the buffer when it can, so small
  entries cost no writes of their own. The file is truncated to its
  final length and renamed over the target, or for a new archive, moved
  there only if nothing has appeared there meanwhile (on POSIX, by a
  hard link, then unlinking the temporary name, where the file system
  has hard links, which FAT lacks). On POSIX that follows closing it,
  which reports the write errors that network file systems defer, but
  there is no fsync, as in Info-ZIP: on a Raspberry Pi's SD card,
  waiting for the device turned a 0.85 s run storing 256 MiB into 20-40
  s, for only durability across a crash and the rare device error that
  nothing else reports. On Windows the rename is by handle, after
  flushing and clearing delete-pending, so it never appears incomplete.
  Closing comes after it there, so the flush, which does wait for the
  device, is the only check for deferred errors before the archive is
  replaced. `FileRenameInfoEx` with POSIX semantics replaces an archive
  that a scanner or indexer holds open (with delete sharing);
  `FileRenameInfo` is the fallback (SMB shares refuse the former). The
  temporary file of a drive-relative archive, `D:x.zip`, is in `D:`,
  that drive's current directory. A concurrent run's temporary file,
  delete-pending, refuses access on Windows, so that counts as an
  existing name and the next is tried, up to 64 in a row: a directory
  that refuses even the check (no traverse rights) makes every name look
  taken.
- Replacing: the archive replaced is the file that any symbolic links at
  its path lead to, so that they survive an atomic update, as Info-ZIP
  updates archives through them (by copying into the file). On POSIX the
  links are read one at a time, so that a dangling one leads where it
  points, but only links the system would follow are: not a loop, nor
  one that Linux's `protected_symlinks` refuses. On Windows, links and
  junctions are resolved by opening the file and asking for its final
  path, or for a dangling link, by creating the file, discarded at once.
  That path names the volume by drive letter where the system can, else
  by GUID (`\\?\Volume{...}`, as for a volume mounted nowhere), else by
  device (`\\?\GLOBALROOT\Device\...`, for one the mount manager does
  not know). Messages name the temporary file beside the target, so a
  target within the directory where the archive was named is named from
  there, as the user named it, and otherwise a drive or share path is
  given without `\\?\`. A link that cannot be followed cannot be written
  (15, as in Info-ZIP). On POSIX the new archive keeps the old one's
  mode, and its group where the user may give it (a member of it, or
  root), but not its owner or ACL. Where the group is not kept, the new
  file's (the user's, or on BSD the directory's) gets only the
  permissions of others rather than the old group's, and set-ID and
  sticky bits stay only with the same owner and group (Info-ZIP keeps
  the whole mode for any group). Should examining the old archive fail
  for any reason but its absence, the new one stays owner-only, as
  Info-ZIP's would. On Windows it keeps the old one's hidden, system,
  and not-indexed attributes, with the archive bit set, but not its
  access control: as for gzip's outputs, that is inherited from the
  directory (copying it would take advapi32).
- Merging: the central directory is parsed with every field bounds
  checked, and as in Info-ZIP, an entry without a name makes it invalid
  (3, after "zero-length name for entry #1"); copied entries get
  regenerated local headers and raw data copies, warning as Info-ZIP
  does of a local header that disagrees with the central one in its
  version needed, flags, CRC (unless a descriptor gives it), or name
  ("Local Entry CRC does not match CD: a.txt"). Their descriptor flag is
  cleared, except, as in Info-ZIP, for encrypted entries (flag bit 0, as
  traditional encryption's check byte depends on it, though AES's does
  not): those keep it, and a data descriptor after their data. They keep
  their extra fields, even with `-X`, which as in Info-ZIP applies only
  to entries written (some fields are needed to extract, such as AES's),
  except Zip64 fields, made anew; one whose fields leave no room for a
  Zip64 field it now needs is an error (3), not a wrapped length.
  Saturated sizes without a Zip64 extra are literal, as Info-ZIP, which
  uses Zip64 only beyond them, writes a file of exactly 4 GiB - 1 bytes,
  and as it, UnZip, and Python read it; such an entry is copied with
  Zip64. So are they beside a Zip64 extra that holds only a saturated
  offset, as Info-ZIP writes such a file past 4 GiB, though APPNOTE
  would have the extra begin with the sizes. UnZip and Info-ZIP itself
  so take the offset for the uncompressed size, and Info-ZIP then cannot
  copy the entry (3, "Did not find entry"), while Python refuses the
  archive. The other reading, from Info-ZIP an entry past 4 GiB that
  begins at exactly 0xffffffff, is far less likely, and for it a local
  header would not be found when copying (3). A replaced entry keeps its
  comment, as in Info-ZIP. Data before the first entry (a
  self-extractor's stub after `zip -A`, a Python zipapp's `#!` line) is
  copied first, as Info-ZIP copies it, so that offsets accounting for it
  stay absolute and the file still runs. Offsets that do not account for
  a preamble are refused (3), as in Info-ZIP, which needs `-A` to fix
  them, with a warning saying so. With no entries, the preamble is what
  precedes the central directory, so a file added to an emptied
  self-extractor keeps its stub. An empty central directory's offset is
  written as 0, as Info-ZIP writes it, since only so does UnZip find an
  emptied self-extractor empty (1) rather than corrupt (3); read, an
  offset of 0 there places it at the end record, after any preamble. The
  Zip64 end record gives the program's version made by, as Info-ZIP's
  does. A Zip64 end record is trusted only if it checks out, or the
  plain end record calls for it or leaves room for it (its central
  directory ending short of it, as some writers make small archives),
  since bytes resembling a Zip64 locator may precede the end record by
  chance (found by fuzzing); where there is room, a fault in the Zip64
  records is theirs, not taken for data before the archive. As in
  Info-ZIP, an archive is split by its disk numbers (the end record's,
  the locator's, and the Zip64 record's central directory disk), not by
  counts of entries on this disk, which writers can get wrong on a
  single disk, nor by the Zip64 record's own. A locator's total of zero
  disks, as Microsoft's writers (Windows Explorer, .NET) put there, is
  taken for one, as Debian's UnZip (6.0-29) takes it, though Info-ZIP's
  zip and UnZip 6.0 take it for a split. Only a missing archive is new:
  anything else at its path must be a zip file, so an empty file, a
  directory, a FIFO, or a device fails with 3 before any work, as
  Info-ZIP fails (it waits on a FIFO, and cannot open a socket, 15). An
  empty file therefore never adds itself.
- Reading the old archive: through a 1 MiB window, so that one read
  serves the headers and data of many small entries (a pread for each
  local header, name, and data made adding a file to many entries
  syscall-bound: for 200K empty ones, 400,405 preads, now 48, against
  Info-ZIP's 200,216 seeks and 7,962 reads). Entries are copied in
  central directory order, usually but not necessarily file order, so
  the read-ahead doubles from a page to the window with each fill that
  carries on from the last, and drops back to a page at a jump
  elsewhere: an entry out of order costs a small read, and data larger
  than the window is read a window at a time. Read-ahead stays within
  the size the archive had when examined, and once the central directory
  is read, before it, where the entries end; should a fill fail, as when
  the archive has shrunk since, the bytes needed are read alone, so that
  it fails only where reading just those would. The end records are
  found among the final 64 KiB, read into the window, and the central
  directory is read through it too, whole if it fits (so a small archive
  is read once), and parsed a header at a time, each entry keeping only
  its name, extra fields (without Zip64), and comment, packed, not the
  directory. Its entries' memory is claimed only once its first header
  checks out, and is filled as each is parsed, so that a file that only
  ends like an archive (sparse, or damaged), whose end record claims
  millions of entries, costs nothing for them.
- Windows: made-by host 0 (FAT), as in Info-ZIP's port, DOS attributes,
  and a `UT` extra field (see the departures), with DOS times local by
  each year's own daylight saving rules, as on POSIX
  (`SystemTimeToTzSpecificLocalTime`). Readers apply DOS attributes
  under host 0 most widely (libarchive, as in `bsdtar`, under no other,
  so that under host 11, NTFS, a read-only file extracts writable),
  though macOS's `/usr/bin/unzip` then ignores flag bit 11 and converts
  names from the OEM code page, so it cannot extract a non-ASCII name
  (50, after asking whether to go on past a "write error"), nor can it
  from the archives of Info-ZIP's port. Names drop a drive or a UNC
  `//server/share/` prefix, as Info-ZIP's do, and likewise a device
  path's `//?/X:/` or `//?/UNC/server/share/`. Wildcard arguments,
  drive-relative ones (`C:*.txt`) included, are expanded per component
  (except when freshening, when, as in Info-ZIP's port, they only match
  entries) and matched as Info-ZIP's Windows port does: ignoring case,
  beyond ASCII too (`É*` finds `école.txt`), by comparing in upper case
  as the file system does (`LCMapStringW`'s file system rules), as the
  port compares by `towupper` (by its sources), without `[sets]`, by
  characters, and by its dosmatch, under which a name without a period
  matches as if it ended in one (`*.*` matches every name). By
  characters, `?` matches a UTF-8 character, not a byte, as the port
  matches characters of its multibyte code page, or wide ones
  (unverified against the port; there a character beyond the BMP would
  be two wide ones); on Unix, Info-ZIP and tugz match bytes. Filters
  match the same way, except that they ignore only ASCII case, as the
  port's narrow dosmatch does by its sources, and case matters against
  entries (`-d`) and when freshening. Files replace existing entries
  whose names differ only in case, and those keep their names. A path
  given again in another case (`D/A.txt d/a.txt`) is skipped like any
  repeat, keeping the first spelling, and different files whose names
  differ only in case collide, as exactly repeated names do (both
  departures: see Names and selection). Hidden and system files are
  skipped unless `-S`, even when named; recursion and wildcards judge
  them, as Info-ZIP does, by the listing (a link's own attributes), so
  that unopenable ones like `pagefile.sys` are never opened. `-@` and
  `@file` lines lose trailing spaces and periods, as in Info-ZIP. Links
  and junctions are followed; cycles are detected by file identity, the
  128-bit `FileIdInfo` where available (64-bit indexes are not unique on
  ReFS). An unknown (zero) ID matches nothing, but where the archive's
  is unknown, as on file systems that report none, a file the archive's
  size is compared by its final path (`GetFinalPathNameByHandleW`), so
  that the archive is still left out (Info-ZIP compares sizes and times,
  never IDs, on Windows). Recursion takes plain files' attributes, size,
  and times from the listing, opening only directories, links, and files
  the size of the archive, whose identity it needs. (A directory entry
  can lag for a file changed through another of its hard links, as
  Microsoft notes.) Only a letter is a drive: `1:x` names stream `x` of
  file `1`.
- Memory: zip has no fixed cap. Its memory is one reservation of address
  space, as much as the system lends up to 16 GiB on 64-bit POSIX hosts
  and 64 GiB on 64-bit Windows (1 GiB for 32-bit processes), halving on
  refusal (on POSIX, the most that leaves 4 MiB for the C library's
  `malloc`, which `opendir` and the like still use, and the stack, which
  a limit on address space, `ulimit -v`, counts too, found by
  bisection), holding a double-ended arena: perm, for what lasts the
  run, grows up from the bottom, and scratch, passed by value and so
  freed by returning, grows down from the top. When an allocation does
  not fit, the allocator calls the platform's `os_extend` hook, which
  gives the arena more of the unclaimed middle, a megabyte at a time, so
  that neither side strands memory the other could use. Only scratch's
  high-water mark stays scratch's, since by value it cannot tell when
  that is free again; scratch below the frame asking is always free.
  Both platforms reserve it inaccessible and commit each megabyte as it
  is claimed (Windows `MEM_COMMIT`, POSIX `mprotect` to read-write), so
  the commit charge grows with use. A writable reservation would be
  charged in full under Linux's strict overcommit
  (`vm.overcommit_memory=2`), which ignores `MAP_NORESERVE`; that flag
  is not used, since in the other modes it would only leave the
  committed chunks out of `Committed_AS`. Where `_POSIX_C_SOURCE` hides
  `MAP_ANON` (FreeBSD, NetBSD, OpenBSD, and glibc before 2.37), the
  reservation maps `/dev/zero` privately instead, and without that (some
  chroots and sandboxes) zip says so: "zip error: Out of memory (opening
  /dev/zero)" (4). The hook refuses arenas other than perm and scratch,
  such as a codec's exactly sized one. Running out (of address space, of
  the commit limit, or of `ulimit -d`) is still "zip error: Out of
  memory" (4), as `test/zip.sh` checks under `ulimit -v` (over a sweep
  of limits, where reserving all but a sliver once failed `opendir` and
  the temporary file instead, 15 or 10) and on Linux `ulimit -d`,
  wherever the shell sets them (not macOS), for a build without
  sanitizers that reserve shadow memory (UBSan alone passes): ctest
  gives it the release `zip` (`ZIPOOM`). gzip and the library keep their
  fixed arenas: their hooks only report running out.
- Memory per entry: perm keeps only what is recorded. A file's path and
  name are built in scratch and copied to perm once it is added, as one
  string when the name ends the path, as most do (`d/f`, `./d/f`, `-j`'s
  `f`), so files excluded, hidden, unreadable, or repeated leave nothing
  behind. Strings are packed (`newstr`, byte-aligned; `newbytes` keeps
  64-byte alignment for buffers). Each file's record is allocated alone,
  so that only an array of pointers moves as it grows. Arrays that are
  perm's last allocation grow in place (`push` returns zeroed slots), as
  `-@` lists and their text do, and the list of items is sized exactly.
  Whatever the write grows with is allocated before the temporary file
  is created: the central directory, as pointers to entries, kept and
  replaced ones updated in place; added entries and new ones' central
  extra fields; the buffers and deflate state (the old archive's read
  window comes earlier, as it is read); and a megabyte of room for any
  one entry's headers and messages, each forgotten before the next. So
  running out of memory cannot strike once output has started, as
  `test/zipclitests.c` and `fuzz-zip` check: their platform layer
  commits exactly what is asked, and traps on committing more once the
  temporary file exists. Only a failure's reason, from a file that
  cannot be read or a write that fails, is then copied into perm. A
  file's name is its key, too, in the map that finds recorded files by
  name: on Windows, where names that differ only in ASCII case are one,
  the map folds them as it hashes and compares them, rather than keep a
  folded copy of each, as it did: for 50,000 files with paths of about
  120 bytes, peak commit went from 32.1 to 26.0 MiB on x86-64 (i686 27.3
  to 21.3), with archives byte-identical.
- Memory per existing entry: about 200 bytes for names of 17 bytes with
  `UT` and `ux` fields (Info-ZIP 3.0: about 270 on macOS, 390 on Linux).
  A 112-byte `zentry` (its Zip64 flag a byte, in what was padding), its
  strings (41 bytes), a 24-byte item, a slot in the table that finds
  entries by name (8 to 16 bytes: open addressing over entry indexes, at
  most half full, where a hash trie took a 56-byte node), and while
  writing, a pointer for the central directory. Unicode names, which few
  entries have, are listed sparsely by index rather than given every
  entry a slot. A replacement is written over its entry, which is
  restored if the file cannot be read, so that only added files take new
  entries. What remains: the `zentry`'s `lextra` (16 bytes, used only
  for new entries and while copying, but tests build entries with it),
  the items, and for a refresh (`zip -r` over the archive's own files),
  each file's record as in a fresh run (a 112-byte `zfile` holding a
  72-byte `os_info`, its path, and a 56-byte trie node by name), which
  Info-ZIP avoids by marking the entry it found instead.
- Scanning: the directories being listed form a stack in scratch rather
  than on the call stack, so that only memory bounds a tree's depth
  (with `\\?\` paths of up to 32K characters, the recursive x86-64
  Windows build's 2 MiB stack held 5,000 levels, not 8,000; see
  Cross-platform verification). Each listing is sorted as pointers to
  its entries rather than moving them, and each entry's strings are
  forgotten as the next is taken. Each open level keeps one copy of its
  path, followed by a slash, whose end is its name too where the name
  ends the path, as it usually does, rather than three copies (path,
  name, and name with a slash): at 450 levels on the Mac, the scan's
  scratch went from 716 KB to 306 KB. `os_listdir` and `os_readlink`
  take a single arena for their results and temporaries, since callers
  that wanted transient results passed one arena as both `perm` and
  `scratch`, whose allocations then overlapped. On POSIX a listing reads
  its names first, packed, then makes their entries at once, since an
  array doubled as it grew in scratch, which grows down and so never in
  place, left each smaller copy behind: about 265 bytes a name where 89
  and the name do, which put one directory of 200K files 27% above
  Info-ZIP's peak, and now level with it. On Windows a listing does the
  same, each name packed with what the listing tells of the file until
  its entry is made, 117 bytes and the name, and names are converted
  from UTF-16 at their length rather than at three bytes a unit, which
  left the rest of each in scratch: for one directory of 100,000 empty
  files, zip's peak commit went from 54.1 to 40.1 MiB on x86-64 (i686
  44.3 to 32.3), with archives byte-identical.
- libdeflate issue #323: Windows' zip folder rejects incomplete Huffman
  codes (such as a lone distance code in a block with at most one
  distinct distance), which DEFLATE permits. `huff_build` always codes
  at least two symbols, so every code is complete; `test_complete_codes`
  parses emitted headers to check this, and zip_windows.sh extracts a
  literal-only input (a de Bruijn sequence: no 3-byte repeats) through
  Explorer.

### Departures from Info-ZIP

Each is deliberate, for safety, determinism, or a friendlier result.
`test/zip.sh` asserts most of them that POSIX shows (most marked
"Departure" there), `test/zip_windows.sh` the Windows ones, and
`test/zipclitests.c` an archive that shrinks (also in `zip.sh`, when its
race allows).

- Exit statuses: `-u` and `-f` with nothing newer exit 0 (Info-ZIP: 12).
  An unreadable directory, or a dangling link (or one whose target
  vanished), met while recursing warns and exits 18 (Info-ZIP adds the
  directory silently and warns "name not matched" for the link, exiting
  0), unless `-x` or `-i` leaves out all it could add, when it passes
  silently: a link by its name, a directory when an `-x` pattern of the
  start of its name and then only stars, such as `'node_modules/*'`,
  covers all below it, or no `-i` pattern can match there, judged by
  their parts before any wildcard. `-i` that matches nothing has nothing
  to do, 12 (Info-ZIP writes an empty archive, exiting 0, when the
  archive is new). A directory named by nothing, `./`, `/`, or a share
  root, adds nothing without `-r`, silently, where Info-ZIP fails with
  an internal logic error (5, "empty name without -j or -r"). A list
  that cannot be read (`@file` or `-@`, standard input closed included)
  fails, 18 or for `-@` 11, where Info-ZIP's getc ends the list at any
  read error, which silently selects, or excludes, less. An archive that
  can be written but not read fails (11, "Could not open archive"),
  where Info-ZIP takes it for a missing one and replaces it, losing its
  entries. So does, before any work, one that is found but then cannot
  be examined, as when an I/O error strikes once its path is resolved
  (Info-ZIP opens it regardless, and replaces one that it cannot open);
  a path that cannot be examined at all is taken for a missing archive,
  as in Info-ZIP (see Compatibility). A file that cannot be read to its
  end is left out, or its entry kept, with the reason and "could not
  read input file" (18), where Info-ZIP, when deflating, stores what it
  read, exiting 0, warning only that the file's size changed (under
  `-0`, on Linux, it fails to write, 14).
- Messages: warnings and errors go to standard error, where Info-ZIP
  writes all but `perror`'s to standard output, and without the tab that
  starts most of its warnings. Advice on options that tugz rejects is
  left out: Info-ZIP's lines on binary transfers and `-F` after "missing
  end signature--probably not a zip file", and its " - use -FI to read"
  after "ignoring FIFO (Named Pipe)". An I/O error's reason is the one
  found when the error occurred, where Info-ZIP's can be a later call's.
  A stale Unicode path field is warned of by its entry's name (see Entry
  names in code pages). A read error in a local or central header of the
  archive gives its reason once, where Info-ZIP also warns with it first
  ("reading local entry: Input/output error").
- Archive contents: entries are sorted by name within each directory
  (Info-ZIP uses readdir order). Any entry that does not shrink is
  stored, though that may take reading the file again, where Info-ZIP
  stores only a file it deflated in a single block (about 32 KiB at
  most), keeping larger ones deflated even when they grow. The text bit
  of the internal attributes is left clear, where Info-ZIP sets it for
  text that it deflates, judged by the literals of its first deflate
  block, whose extent only a replay of its matcher reproduces.
  `SOURCE_DATE_EPOCH`, which Info-ZIP ignores, clamps times and makes
  them UTC, and a `UT` field's access time is the epoch itself, since
  reading a file may change its access time (on Linux, under
  `relatime`): an earlier one, recorded as it was, would differ in a
  second run. Names that are valid non-ASCII UTF-8 always get flag bit
  11 (Info-ZIP's builds differ: Ubuntu's sets it, macOS's never). A
  directory reached again through links within it, a loop, is added but
  not entered, with a warning ("skipping directory loop"), where
  Info-ZIP follows the links until the system refuses the path
  (`ELOOP`), adding the loop's entries again at every level. Zip64 end
  records are written when the count, size, or offset of the central
  directory needs them, as APPNOTE has it, so for exactly 65,535 entries
  too, as readers take 0xffff to defer to them (and likewise a size or
  offset of 0xffffffff), though not merely because an entry is Zip64;
  Info-ZIP writes them for more than 65,535 entries, or when any entry
  is Zip64. And an entry's Zip64 extra field comes before its others,
  where Info-ZIP's comes last. Both forms are read alike. A time after
  2107 is stored as the DOS range's end, 2107-12-31 23:59:58, where
  Info-ZIP wraps the year modulo 128 (2108 becomes 1980). On Windows, a
  `UT` field holds no creation time (flags 3, not 7), and no NT security
  descriptor (`SD`, 0x4453) field is stored, where Info-ZIP's port
  stores both, which would tie the archive to when its files were copied
  and to their access control. DOS times there follow each year's own
  daylight saving rules, where the port, by its C runtime's `localtime`,
  applies the current year's to every year: for dates under other rules
  (in the US, before 2007), the two are an hour apart, so that `-FS`
  (and `-u` and `-f`, for an entry without a `UT` field) takes such an
  entry that the port wrote for changed, as the port takes tugz's.
- Names and selection: doubled slashes collapse, in patterns too, so
  that `d/a d//a` is one path rather than two entries, and `.//a` names
  `a`, where Info-ZIP stores `/a`. Different paths giving one name are
  an error even when the archive has an entry of that name (Info-ZIP
  lets the last path replace it, silently). Two files that find one
  entry, one by its stored name and the other by its Unicode path field,
  are both kept: the one by its stored name replaces it, and the other
  is added, as Info-ZIP's builds without Unicode support do (with it, as
  on Linux, the last file replaces the entry, silently). Patterns given
  after `-j` stay whole (Info-ZIP's name conversion cuts them to their
  last component, so that `-j -x 'dir/*'` excludes everything). Entry
  names select files only within the current directory, so that
  refreshing an untrusted archive reads nothing beyond it: names zip
  would make (but for leading `./`), without `..` components, through
  directories that are not links (on Windows, nor junctions); a link the
  name ends in is followed, as for any path, unless `-y`. Info-ZIP reads
  them all, absolute names included. Other entries are refreshed only by
  naming their files as paths (`zip -u a.zip ../f`), and as when their
  files are missing, `-FS` deletes them unless so named. A file named as
  the archive is left out before repeats are looked for, as an excluded
  one is, so two such (`zip -j out.zip a/out.zip b/out.zip`) are no
  repeat, where Info-ZIP looks for repeats first and fails (16). On
  Windows, names that differ only in case are one name (see Windows), as
  Info-ZIP's port finds entries, but by its sources, it compares files
  to add by bytes (strcmp), adding both of `D/A.txt d/a.txt`, or of two
  such files under `-j`, whose entries then collide when extracted
  there, and it reports the first repeat in order of bytes, not ignoring
  case. Also on Windows, `zip -r t.zip C:` adds the entries of the
  drive's current directory, where Info-ZIP's port, by its sources,
  lists that directory but examines and reads its entries at the drive's
  root.
- Special files: sockets and block devices are left out too. Info-ZIP's
  Unix port (judging by its behavior, a type test by masking mode bits)
  takes a socket for a regular file, which it then cannot open (18; `-d`
  deletes the socket's entry), and a block device for a directory,
  adding an entry such as `dev/sda/`, with a "file and directory with
  the same name" warning (18).
- Files changed while zip works (see Writing): Info-ZIP blocks on a FIFO
  swapped in for a file, and under `-y`, though it examines each file
  again just before reading it, still reads through a directory swapped
  for a link once its files were found, and stores whichever link is
  there. (Both read through one swapped in while the scan is in it.) A
  file whose data turns out larger or smaller than it was is stored as
  read, with the warning of Info-ZIP's Unix port ("file size changed
  while zipping", leaving the status alone; its Windows port gives none,
  nor does tugz there), but compared with its size in the scan, from
  which its entry's time comes, where Info-ZIP examines each file again
  just before reading it, and so warns only of changes while it reads.
- Existing archives: under `-FS`, an entry whose replacement cannot be
  read is kept, as in other modes (Info-ZIP warns that it will copy it
  over, then drops it). A file added to an emptied self-extractor keeps
  its stub, which Info-ZIP drops. An archive that shrinks while zip
  works fails at the entry being copied (2), where Info-ZIP notices only
  at an entry's header, and copies a cut-off entry's data without error.
  Saturated sizes beside a Zip64 extra that holds only the offset are
  read as Info-ZIP means them, not as it and UnZip read them (see
  Merging). Offsets that do not account for data before the archive are
  refused before any work, with a warning saying so, where Info-ZIP
  fails only on copying an entry (warning that it "did not find" it), so
  it updates one whose every entry it replaces or deletes. The end
  records must lie exactly where they place each other: the central
  directory ending where the Zip64 end record, or failing one the end
  record, begins, and the Zip64 end record, by its size, ending where
  its locator begins, at the offset that gives. Info-ZIP, which reads
  from the directory's offset to the next record, and compensates for a
  locator that misses, also updates archives with bytes between these,
  or a wrong Zip64 record size, directory size, or locator offset. No
  writer is known to make them, and tugz refuses them (3), as layouts
  that do not add up are how unadjusted data before the archive shows.
  An archive whose Zip64 locator gives a total of zero disks, as
  Microsoft's writers make them, is read as on one disk, where Info-ZIP
  asks for the next part of a split archive (`Could not find: x.z01`).
- Replacing the archive: a hard-linked archive is replaced by a new
  file, which its other names do not share (Info-ZIP copies into it).
  The temporary file goes beside the file that links at the archive path
  lead to, to be renamed over it, so an archive in a directory that
  refuses new files cannot be updated through a link from one that
  allows them (10, "Temporary file failure"), where Info-ZIP writes its
  temporary file beside the link and copies it into the archive, which a
  failure partway through would leave damaged. A new archive replaces
  nothing: a file made at its path while zip works is kept, and zip
  fails to replace it (15), where Info-ZIP replaces it. When the archive
  cannot be replaced, the temporary file is removed, where Info-ZIP
  keeps it ("new zip file left as"), as zip does, with that warning,
  only if removing it fails too. After a failed read or write, one that
  cannot be removed is named in a warning ("could not remove temporary
  file"), where Info-ZIP removes it as it can, saying nothing. A
  dangling link at its path gets its target created and survives
  (Info-ZIP leaves an empty file there and replaces the link with the
  archive). On Windows the new archive is flushed to the device before
  the rename (see Writing), which Info-ZIP never does. The old archive's
  group is kept where the user may give it, where Info-ZIP's new file
  has the user's (or on BSD the directory's) unless it copied into the
  archive through a link. If not kept, the group gets the permissions of
  others, and in any case the set-ID and sticky bits are kept only if
  the new archive has the old owner and group, where Info-ZIP keeps the
  whole mode for a new owner or group.

## unzip

The unzip program reads archives through the reader that zip uses for an
existing archive (`src/zipin.c`: the end records, then the central
directory through the read-ahead window, and each entry's local header
found and its data bounds-checked), with `src/zip.c`'s parsers, decodes
through `src/inflate.c` alone, and applies `src/unzip.c`'s rules: pure
functions, ported from UnZip 6.0's source, that `test/unziptests.c`
checks against tables recorded from macOS's `/usr/bin/unzip` and
Debian's UnZip (6.0-29), for names to paths (`uz_mapname`), DOS dates
(`uz_dosdate`, `uz_timegm`), extra fields' times and owners
(`uz_extra_izux`), modes (`uz_mode`, `uz_symlink_host`), Windows
attributes (`uz_dosattr`), and names made safe to show (`uz_filter`).
The driver, `src/unzipcli.c` (`unzip_main`), follows UnZip's `unzip.c`,
`envargs.c`, `process.c`, `list.c`, `extract.c`, and `fileio.c`, cited
at each function, with Debian's additions: ISO dates in listings and the
check for overlapped components. Beyond zip's file system functions
(`platform/zipfs_*.c`), its platform hooks, declared at the top of
`src/unzipcli.c`, are `os_isatty`, `os_error`, `os_mkdir`, `os_umask`,
`os_mktime` (Unix seconds from a local time, the inverse of
`os_localtime`), `os_setattrs` (owner, mode, and times, on the
descriptor of a file not yet kept), `os_setdirattrs` (a directory's,
through a handle that refuses a link at its end), and `os_symlink`, in
`platform/unzip_posix.c` and `platform/unzip_windows.c`, which also hold
the entry points. Neither expands member arguments, which are patterns
for entries.

- Scope: busybox unzip's features, and UnZip 6.0's that are cheap:
  listings (`-l`, `-v`), testing, extraction to disk or to standard
  output (`-p`, `-c`), the comment (`-z`), selection (`-x`, `-C`),
  overwriting (`-n`, `-o`, `-f`, `-u`, the prompt), `-j`, `-D`, `-V`,
  `-K`, `-X`, `-q`, the environment (`UNZIP`, else `UNZIPOPT`, split as
  zip splits `ZIPOPT`), and wildcard archive names. UnZip's other
  options are refused with its "error:  -a option not supported" (10),
  as are `-K` and `-X` on Windows. Entries that are encrypted,
  compressed by a method other than stored, deflated, or Deflate64, or
  need a version past 4.5 are skipped with UnZip's messages for a build
  without them, and count as skipped (81).
- Messages: UnZip's words, and its routing (`UzpMessagePrnt`): a message
  for standard error goes there, but under `-t` everything goes to
  standard output, so that redirecting it keeps the whole report, and
  errors are repeated on standard error when that alone is a terminal.
  Whether the last message ended a line is tracked across both streams,
  for UnZip's newlines before and after messages. Statuses combine as in
  `extract.c` and `process.c`: per archive the most severe; an unmatched
  member raises a status of at most 1 to 11, while an unmatched `-x`
  pattern only warns; no entry processed is 81 if any was skipped, else
  11; and skipped entries raise a status of at most 1 to 81. Over the
  matches of a wildcard, the most severe but a directory's, then
  Info-ZIP's summary on standard error ("2 archives were successfully
  processed.").
- Archives: a name is tried as given, then with `.zip` and `.ZIP`. A
  name with wildcards is expanded by `src/dir.c`'s `expand_wild`, on
  POSIX as UnZip's `do_wild` matches (case matters, `[sets]` and escapes
  work, and a name starting with `.` matches only a pattern that does),
  on Windows as zip and gzip expand arguments there (ignoring case, by
  DOS rules, with either separator), but with hidden and system files,
  which they leave out, as UnZip's port lists them (`win32/win32.c`'s
  `Readdir`), and the matches are processed in name order. An archive of
  `-`, as in busybox, is standard input: a regular file is read in place
  (and left open), anything else read whole into memory first
  (`zin_memory`, one window that never moves), and `-n` is implied
  unless `-o` is given, as no prompt could read its answers.
- Data before the archive: the comment is shown, and `-z` stops, once
  the end record is found, before the central directory is read, as
  UnZip's `find_ecrec` does. Data before the archive that its offsets do
  not account for (a self-extractor's stub), the distance from where the
  central directory ends by its offset and size to where the Zip64 end
  record, or failing one the end record, begins, shifts every entry's
  offset, with UnZip's "extra bytes at beginning or within zipfile"
  warning (1); a negative distance is its "missing N bytes in zipfile"
  (2), and an offset of 0 for a directory that is not empty its "NULL
  central directory offset" (2). Where the first entry's local header is
  not found so shifted, the shift is undone ("attempting to
  re-compensate"), and redone should a later entry then not be found, as
  UnZip does; only the entries read are shifted. A central directory
  invalid after some entries is processed up to there, then reported, as
  UnZip finds the fault only once it gets there. Disk numbers in the end
  record alone are taken, as UnZip takes them, for the parts of an
  archive concatenated, with its warning; a split that Zip64 records
  describe is refused (11). The shared parser takes a Zip64 locator's
  total of zero disks for one (see Merging, under zip).
- Overlapped components (zip bombs): before any entry's data is read, by
  `-t`, `-p`, `-c`, or extraction (listings read none), the least that
  each selected, readable entry takes, a local header's fixed 30 bytes
  and its compressed data, is sorted by offset (a stable merge sort),
  and none may overlap the next or reach the central directory. Each
  entry is checked again once its local header is read, by its real
  length, against the next one's offset. Either failure is Debian's
  "invalid zip file with overlapped components (possible zip bomb)"
  (12), before anything is written, the `-d` directory included.
  Debian's UnZip finds overlaps entry by entry, as it reads them, and
  does not check the central directory.
- Decoding: an entry is read by its central header's method, flags,
  sizes, and CRC. Its local header gives the data's offset, its DOS time
  and extra fields (times and owner, as UnZip takes them from there),
  and its name, which is decoded as the central one is and compared with
  it, as Debian's UnZip compares them ("mismatching "local" filename",
  1). A stored entry copies its compressed size (warning, as UnZip does,
  when the two sizes differ). A deflated entry's input stops at its
  compressed size, and its output is decoded to its end, to check it,
  but written only up to its size; more than its size with a CRC that
  matches is invalid data (2). Deflate64 (method 9, PKWARE's "enhanced
  deflating", which Explorer's zip folder writes for large files: a 2.2
  GiB one, not one of 64 MiB) is decoded as UnZip's `USE_DEFLATE64`
  build does, and listed as `Def64N` (with the level as for `Defl:N`);
  its inflator, with a 64 KiB history and room for a 65538-byte match
  (407 KB, to Deflate's 308 KB), is claimed only for an archive with
  entries by it, after its central directory. Output goes out 64 KiB at
  a time, as UnZip flushes its slide, so that an error in a short
  entry's data leaves none of it on standard output, and there each
  entry's output is flushed as it ends, so that a write that fails,
  however short, is reported for it (50). A read error in the archive ends the run (3,
  "zipfile read error"), as UnZip's `readbyte` does, and an archive that
  ends early while its central directory is read is 51.
- Names: as zip reads them (`zar_uname`), a name is UTF-8 if flag bit 11
  is set, or comes from a Unicode path field (0x7075) of version 1 whose
  CRC is the stored name's. Otherwise, on Windows, as Info-ZIP's port
  reads them, one made on MS-DOS (but by PKZIP for Windows 2.5 to 4.0),
  OS/2, or by WinZip on NTFS is decoded from the OEM code page, and
  another that is not UTF-8 from the ANSI code page; on POSIX it is its
  bytes. Members match, and messages show, that name, through
  `uz_filter`: bytes below 0x20 as `^` and a letter, and DEL, C1
  controls, and bytes not UTF-8 as `?` (UnZip relies on the locale).
  Paths follow `uz_mapname`, in UnZip's order: a name made on MS-DOS
  without `/` separates with `\` (warned of once per archive), leading
  `/` are stripped (warned of), `-j` keeps the last component, empty and
  `.` components are dropped, as are `..` (warned of), control
  characters are dropped, a final `;N` too unless `-V`, and on POSIX a
  final `.` or `..` component becomes `_` or `__`; on Windows,
  `:\<>|"?*` become `_`, trailing dots and spaces are dropped (UnZip
  leaves that to the system), and a device's name, by a list (CON, PRN,
  AUX, NUL, CONIN$, CONOUT$, CLOCK$, and COM and LPT with a digit or a
  superscript one, two, or three, in any case, with any extension), gets
  a `_` before it (UnZip asks the system). A name left empty fails
  ("mapname:  conversion of ... failed", 2), its directories still made.
  A path so made never holds a `..` component, a leading `/`, NUL, or
  control characters, nor on Windows a drive, a stream, or a device's
  name: `tests-unzip` checks every name of up to four characters from an
  alphabet of the troublesome ones, under every option and host, then
  100,000 longer ones at random.
- Extraction: the `-d` directory is made, if missing, one level only, as
  UnZip's `checkdir` makes it (a trailing `/` dropped), and an entry's
  missing directories silently; a directory entry says "creating:" only
  when it made its directory. A file there already is judged by UnZip's
  `check_for_newer` (the entry's extended time, else its DOS time
  against the file's rounded up to two seconds; a link is older, with
  UnZip's note), then `-f`, `-u`, `-n`, `-o`, or the prompt decide. The
  prompt is UnZip's, on standard error, its answers read through a
  reader allocated before any output, into 10 bytes as UnZip's `fgets`
  reads them, so that a longer answer is taken in pieces: `y`, `n`, `A`
  and `N` for the rest of the run, `r` for "new name: " (asked again
  while empty; the end of input keeps the name), and others "error:
  invalid response [...]" (`{ENTER}` for an empty one); at the end of
  input, " NULL" and "treating as "[N]one"", status 1. A file to replace
  is removed, then the new one created exclusively ("cannot delete old",
  "cannot create": 50), as UnZip's `open_outfile` and gzip do. On POSIX
  the owner (`-X`), mode, and times are set on the descriptor, in that
  order, before the file is kept; Unix hosts' modes are taken as they
  are, others' with the umask, and set-ID and sticky bits only under
  `-K`. Times come from the local `UT` field, else a `UX` field, else
  the DOS time as local time (`os_mktime`: `mktime` with `tm_isdst` -1).
  Directories made by their own entries get theirs at the end, deepest
  first (their paths sorted in reverse), through a handle opened without
  following a link (`O_NOFOLLOW`): owner, times, then mode, as UnZip
  orders them, keeping an inherited set-group-ID bit unless `-X` or `-K`
  for a Unix host's.
- Links: never through one. Below the `-d` directory, every directory on
  an entry's path is checked not to be a link (`linkless`, from the `-d`
  directory on, remembering the directories found, so that a run of
  entries in one directory examines it once), and missing ones are made.
  A link there, from the archive or from before, is UnZip's "exists but
  is not directory" (2), where UnZip follows it. A file is removed and
  created anew, never opened where it is, so a link in its place is
  replaced, not followed. Between a check and the creation, another
  process could still put a link in the way. Links with data, from the
  hosts that UnZip takes links from, are deferred, as UnZip defers them:
  each gets an empty file, created exclusively, to hold its place, whose
  identity (device and inode) is recorded, and its target, up to 4096
  bytes (a longer one is refused with a warning, 1), is kept in memory,
  checked by the CRC. Once every file is extracted ("finishing deferred
  symbolic links:"), each placeholder still there, reached through no
  link, empty, and the same file, is removed and the link made in its
  place (with `-X`, given its owner by `lchown`); otherwise "invalid
  placeholder file".
- Discarding: extracted files are opened as gzip's outputs are, to be
  removed unless kept (on Windows, delete-pending), and kept only once
  their data has been checked and their attributes set. A bad CRC,
  invalid data, data beyond the size, or a failed write or close leaves
  no file, and so does an interruption, through the signal handlers that
  gzip and zip share, after which the process dies by the signal. A
  failed write asks UnZip's "write error (disk full?).  Continue?
  (y/n/^C)" and goes on to the next entry only for `y` (50).
- Memory: zip's reserve-and-commit arenas, perm and scratch. The
  inflator, its 64 KiB output window, and the prompt's reader are
  claimed before the first archive, and whatever an archive needs before
  its first change to the file system, its memory then reused for the
  next: its entries (the central directory whole) and their Unicode
  names, and, as extraction is planned, every selected entry's mapped
  path, room for links' targets and paths and for the directories
  finished at the end, the sorted spans of the overlap check, the cache
  of directories found, and the room that extracting any one entry takes
  (its messages, its path, a new name, and sorting the directories),
  claimed last, just before the `-d` directory is made. So running out
  of memory (4, "not enough memory") extracts nothing, not even the `-d`
  directory, as `test/unzip.sh` checks under `ulimit -v` and on Linux
  `ulimit -d`, and nothing is claimed once the file system has changed,
  as `test/unzipos.c` checks in every run of `tests-unzipcli` and
  `fuzz-unzip`, but for a link or directory renamed at the prompt, whose
  path is kept to the end, and the POSIX layer's `malloc` of a pending
  output's path, outside the arenas. Unlike UnZip, which reads the
  central directory as it goes, unzip holds it whole: on the M4 Max, for
  100,000 entries with names of 17 bytes and no extra fields, `-t`
  peaked at 18.6 MB and extraction at 31.9 MB, and for 200,000 at 34.6
  and 61.3 MB, about 160 and 290 bytes an entry (UnZip: 2.8 MB either
  way).
- Windows: `platform/unzip_windows.c` is CRT-free like zip's. Paths are
  `\\?\` paths, of any length, and the console is read and written in
  UTF-16, so a new name typed at the prompt may be any Unicode; an
  answer's CR before its newline is dropped, as the port's text-mode
  `fgets` drops it. A `\` in member patterns is a `/`, as the port takes
  it. `os_setattrs` gives a file its times and its read-only, hidden,
  system, and archive attributes (from the external attributes' low
  byte, whatever the host) in one `FileBasicInfo` call on the handle
  that wrote it, still delete-pending, which Windows allows and
  `os_keep` then clears, trying times and attributes apart should that
  fail, to tell which failed. Directories get times only, as in the
  port, through a handle opened with `FILE_FLAG_OPEN_REPARSE_POINT` that
  refuses a reparse point. Times include a local `UT` field's creation
  time, as the port's `SetFileTime` sets it (`win32.c`, `getNTfiletime`,
  `close_outfile`, `set_direc_attribs`): only along with the field's
  modification time, for files and directories, `-D` and `-DD` skipping
  it with the others (`test_ctime`; `unzip_windows.sh`, "Times").
  Junctions, like links, count as links on the way to an entry, and a
  junction or a directory link at a file entry's own name is replaced,
  as a link is on POSIX: removed by a handle to the link itself, opened
  with `FILE_FLAG_OPEN_REPARSE_POINT`, checked to be a junction or a
  link, and marked deleted, which removes neither its target nor what
  is in it, so nothing is written through it. A directory that is not
  a link, even an empty one, stays ("cannot delete old", 50, as in
  UnZip). At a directory entry's own name, a junction is "exists but is
  not directory" (2), as a link is on POSIX. A link becomes a regular
  file holding its target, as in the port, made last, as on POSIX. DOS
  times are local by each year's own daylight saving rules
  (`TzSpecificLocalTimeToSystemTime`), as zip writes them there. There
  is no umask (access control is inherited), and `-K` and `-X` are
  refused. Explorer, `tar`, and .NET's `Expand-Archive` extract the
  archives of tugz's zip as unzip does, and Explorer and `tar`
  Explorer's own, while `Expand-Archive` decodes those OEM names from
  the ANSI code page.

### Departures from Info-ZIP UnZip

Each is deliberate, for safety, for busybox's features, or for a simpler
or friendlier result. `test/unzip.sh` compares everything else with
Info-ZIP's UnZip and checks these on their own (some marked "Departure"
there), `test/unzipclitests.c` (`tests-unzipcli`) checks them in memory,
`test/unzip_windows.sh` the Windows ones, and `fuzz-unzip` the safety
ones as invariants. Those that no test asserts are marked untested.

- Arguments: as in busybox, an argument after the archive made only of
  option letters that tugz takes is options (`unzip a.zip -o`), where
  UnZip takes it for a member (`unzip.sh`, "options after the archive"),
  and an archive of `-` is standard input, implying `-n` unless `-o`
  (`unzip.sh`, "Standard input as the archive"; `tests-unzipcli`,
  `test_stdin`). `-v` alone prints one line, "tugz unzip 0.2.0, a subset
  of Info-ZIP UnZip 6.0", rather than UnZip's version report, and the
  usage and `-hh` are tugz's own (`unzip.sh`, "Usage"). UnZip's options
  that tugz lacks are refused (10) rather than taken (`unzip.sh`).
- Archives: the matches of a wildcard are processed in name order,
  rather than the directory's (`unzip.sh`, "w/*.zip order"). A split
  archive that Zip64 records describe is refused (11), where UnZip reads
  on as though its parts were concatenated (untested), and an entry made
  on VMS is extracted without UnZip's question "stored in VMS format.
  Extract anyway? (y/n)" (untested).
- Zip bombs: overlapped components, and data reaching into the central
  directory, which Debian's UnZip does not check, are found before any
  entry is read, so that none is extracted, nor the `-d` directory made,
  where Debian's UnZip finds them entry by entry, having extracted those
  before (`unzip.sh`, "Overlapped components", compared with a Debian
  `REF` where it finds them too; `tests-unzipcli`, `test_bomb`).
- Sizes: an entry is read by its central header's sizes, method, and
  CRC, the central directory being read whole first, rather than by its
  local header's (untested: no test sets them apart). Output stops at an
  entry's size, where UnZip writes what it decodes beyond it before
  finding the CRC wrong (`unzip.sh`, "Output beyond an entry's size";
  `test_damaged`).
- Deflate64: a match reaching before the start of an entry's data is
  invalid ("invalid compressed data to inflate", 2), as a deflated
  entry's is by zlib's rule, where UnZip's own inflate, which decodes
  Deflate64, copies from its window as an earlier entry left it and then
  usually finds a bad CRC, also 2 (`unzip.sh`, "Departure: a Deflate64
  match").
- Damaged and failed files: a bad CRC, invalid data, data beyond the
  size, or a failed write leaves no file (2, or 50), where UnZip keeps
  what it wrote, though the old file is gone either way (`unzip.sh`,
  "leaves no file" and a write past `ulimit -f`; `test_damaged` and
  `test_faults`; `unzip_windows.sh`, a bad CRC). A failed write to
  standard output (`-p`, `-c`) is reported ("write error (disk full?).")
  for the entry whose data it held, however short, its output being
  flushed as it ends, and ends the archive's processing (50), where
  UnZip, writing data there as messages, ignores it and exits 0
  (`unzip.sh`, to `/dev/full` where there is one; `test_faults`). An
  interrupted run removes the file being written and dies by the signal,
  as gzip and zip do, where UnZip exits 80 (untested here).
- Links on disk: nothing is written through a link below the `-d`
  directory, whether from the archive or there before: an entry under
  one fails with UnZip's "exists but is not directory" (2), where UnZip
  follows a link to a directory. The `-d` directory and the path to it
  may be links (`unzip.sh`, "Departure: nothing is written through a
  link"; `test_links`; `fuzz-unzip`'s planted links;
  `unzip_windows.sh`'s junction).
- Deferred links: a placeholder is an empty file, known by its identity,
  where UnZip's holds the target, known by its size and content
  (`test_links`, a link and a file of one name). A target longer than
  4096 bytes is refused, making neither link nor file, with a warning
  that ends the line naming it, where UnZip makes the link: status 1,
  though UnZip's own failures to keep or make a link leave the status
  alone (`test_links`). A link's mode is not set, where UnZip's builds
  with `lchmod`, such as macOS's, set it (untested: `unzip.sh`'s tree
  listings leave links' modes out).
- The prompt: a new name is mapped as entry names are, below the `-d`
  directory, where UnZip takes it relative to the current directory,
  ignoring `-d` (and Apple's build takes a leading `/` as absolute)
  (`test_prompt`, whose new names under a `-d` directory must stay below
  it; `unzip.sh` compares names with `..` and a leading `/` without
  `-d`, but for Apple's).
- `-f` with a `-d` directory that is missing freshens nothing, where
  UnZip, not making it, freshens the current directory (untested:
  `unzip.sh` runs it, but with nothing to freshen in either place).
- Display: control characters in comments are shown as in names (`^X`),
  where UnZip writes them raw, and bytes not UTF-8 in names and comments
  as `?`, where UnZip leaves them to the locale (`unzip.sh`,
  "comments.zip"). A name is shown in Unicode whenever a Unicode path
  field gives it, as Debian's UnZip shows it in a UTF-8 locale
  (`unzip.sh`, "names.zip"). Listings' dates are ISO, as Debian's.
- Windows: a link becomes a file holding its target, as in the port, but
  made last, as on POSIX (`unzip_windows.sh`, "Links"; `test_windows`).
  Junctions and directory links are never followed, but replaced at a
  file entry's own name, as links are on POSIX (`unzip_windows.sh`,
  which also finds an empty directory there left in place, 50).
  Trailing dots and spaces are dropped from names, and devices' names
  found by a list, rather than left to the system and asked of it
  (`test_windows`; `unzip_windows.sh`, "Names mapped for Windows"). DOS
  times are local by each year's own daylight saving rules, where the
  port applies the current year's (`unzip_windows.sh`, a date in March
  2006). Paths may exceed `MAX_PATH` (`unzip_windows.sh`, "Long paths"),
  and the console is read and written in UTF-16 (`unzip_windows.sh`, the
  prompt in a pseudo console).

## Workflow

    cmake -B build && cmake --build build -j
    ctest --test-dir build -j4 # unit, library, ZIP format, zip and
                               # unzip rules and programs, and
                               # amalgamation tests (ASan/UBSan), gzip,
                               # zip, and unzip end to end (their
                               # out-of-memory tests with the release
                               # builds, where ulimit works) (needs
                               # zlib, libdeflate, /usr/bin/gzip,
                               # Info-ZIP unzip and zipinfo; optional
                               # Python, which crafts unzip's archives)
    SLOW=1 sh test/cli.sh build/gzip   # adds a 5 GiB stream (>4 GiB offsets)
    cmake -B build-w64 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-x86_64.cmake
                               # Win32 builds (or cmake -G Ninja natively
                               # under w64devkit, which runs zip-windows
                               # and unzip-windows)
    cmake -B build-fuzz -DCMAKE_C_COMPILER=clang -DTUGZ_BUILD_FUZZ=ON
    cmake --build build-fuzz   # the seven fuzzers (LLVM clang)
    cmake --build build-fuzz --target tugz_fuzz_seeds  # fuzz/corpus/
    build-fuzz/fuzz-diff-inflate -fork=3 -max_len=65536 fuzz/corpus/diff-inflate
    build-fuzz/fuzz-zipread -jobs=6 -workers=6 -max_len=8192 fuzz/corpus/zipread
    build-fuzz/fuzz-zip -jobs=6 -workers=6 -max_len=8192 fuzz/corpus/zip
    build-fuzz/fuzz-unzip -jobs=6 -workers=6 -max_len=8192 fuzz/corpus/unzip
    cmake -B build -DTUGZ_BUILD_BENCH=ON && cmake --build build
    build/bench -l 1,6,9 bench_corpus/silesia/*
    cmake -P cmake/amalgamate.cmake    # gzip.c, zip.c, unzip.c, tugz.c (and
                                       # the build's amalgamation/ has them)
    SLOW=1 sh test/zip.sh build/zip    # adds Zip64: 4 and 5 GiB files, 70,000 entries
                                       # (needs about 10 GiB free in TMPDIR)
    sh test/zip_windows.sh ./zip.exe   # on Windows, under w64devkit
    REF=/usr/bin/unzip sh test/unzip.sh build/unzip  # against Info-ZIP's UnZip
    SLOW=1 sh test/unzip.sh build/unzip  # adds Zip64: entries of 4 GiB
                                       # (needs about 13 GiB free in TMPDIR)
    TUGZ_ZIP=./zip.exe sh test/unzip_windows.sh ./unzip.exe  # on Windows

Fuzzers:

- `fuzz-inflate`: arbitrary input to raw and gzip decoders, and to the
  Deflate64 decoder, which with input and output in fuzzer-chosen
  pieces must agree with itself given everything at once
- `fuzz-roundtrip`: deflate then inflate; output must not depend on push
  sizes or stream offset (including past 4 GiB), nor, in any format, on
  an encoder reset after other streams at the same or another level;
  zlib must agree
- `fuzz-diff-inflate`: exact accept/reject and output agreement with
  zlib, for raw DEFLATE and for multi-member gzip (GNU trailing-data
  policy); then streaming in raw, zlib, and gzip formats with
  fuzzer-chosen input and output piece sizes, which must agree with zlib
  on success, on truncation versus error, on output, and on the exact
  stream end
- `fuzz-diff-deflate`: zlib (every parameter, flush mode, mid-stream
  parameter change) and libdeflate streams must decode exactly; our
  streaming encoder, in every format with fuzzer-placed NONE/SYNC/FULL
  flushes and piece sizes, must produce piece-independent output that
  decodes under zlib, libdeflate, and our streaming decoder
- `fuzz-zipread`: arbitrary bytes as an existing archive, parsed by
  `src/zip.c` as the program reads it (the final 64 KiB, then the Zip64
  end record and the central directory), each part in an allocation of
  its exact size, so that AddressSanitizer sees reads past it; whatever
  parses is rewritten much as a merge would (but skipping bad local
  headers, which fail a merge, and dropping every data descriptor), and
  its central directory must parse back to the same entries
- `fuzz-zip`: the zip program itself (`zip_main`, in memory), with
  arbitrary bytes as the archive it adds to, refreshes, or deletes from,
  in eight modes, with POSIX or Windows conventions, and faults (the
  archive shrinking or failing to read, before it is read or while
  entries are copied, and writes failing). It must leave the archive as
  it was, or else replace it with one whose entries are the old ones in
  order, copied exactly or replaced by the files', then the files'
  (unless deleted), and which it reads back; it must leave no temporary
  file, nor claim memory once it has created one. Inputs need more than
  64 KiB (`-max_len`) for copies to read through the window again.
- `fuzz-unzip`: the unzip program itself (`unzip_main`, in memory,
  `test/unzipos.c`), with arbitrary bytes as its archive, in sixteen
  modes (extraction with `-o`, the prompt, `-n`, `-j`, `-x`, `-f`, `-u`,
  `-V`, a linked `-d`, `-KX`, `-DDC`, or from standard input; `-l`,
  `-v`, `-t`, `-p`), with POSIX or Windows conventions, the umask, the
  prompt's answers, and faults (the archive shrinking or failing to
  read, as it is opened or once extraction began, and directories,
  files, keeps, attributes, or links failing), over a tree with a
  sentinel outside the `-d` directory and links within it that lead
  there. It must exit with a status UnZip may, change nothing outside
  the `-d` directory and write over no file there, leave every file it
  extracts equal to an entry of that name as `src/zip.c`'s parser and
  zlib read it, pipe no more than the entries' sizes, leave nothing
  open, and claim no memory once it has changed the file system; then,
  run again with `-n`, change nothing.

## Cross-platform verification

- WSL Debian x86-64 (GCC 14): unit tests under ASan/UBSan/LeakSanitizer,
  `test/cli.sh` for release, sanitized, and `-m32` builds; terminal
  behavior via `script(1)`. A 32-bit build compresses and decompresses a
  3 GiB file (and fails to open it without `_FILE_OFFSET_BITS=64`).
- Windows: the 64-bit and 32-bit (`i686-w64-mingw32`) builds pass
  `test/cli.sh`; `Stop-Process -Force` mid-compression leaves the input
  and no output; console detection checked under ConPTY (`ssh -tt`).

- Windows 11, i9-12900, w64devkit GCC 16: `gzip.exe` with the real
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
  compressed and stored, an entry offset past 4 GiB, merging into a
  Zip64 archive, 70,000 entries), on aarch64 Linux, and with the
  big-endian ppc build under QEMU. Header fields match Info-ZIP 3.0 (see
  Compatibility). `test/zip_windows.sh` passes for the x86-64 build, the
  amalgamation, and the i686 build on Windows 11: Explorer's zip folder,
  `Expand-Archive`, and `tar` extract levels 1, 6, and 9 identically,
  including the literal-only input. zip.exe is about 100 KiB, imports
  only KERNEL32 and SHELL32, and has no stack frame over 4000 bytes (no
  `__chkstk`).
- unzip: `test/unzip.sh` passes on macOS against its `/usr/bin/unzip`,
  Apple's UnZip 6.0, for the sanitized build and, for the out-of-memory
  tests, the release build, and `tests-unzipcli` and `fuzz-unzip` run
  Windows conventions on any host. `unzip.exe` builds warning-free with
  mingw for x86-64 and i686, as does the amalgamation `unzip.c`; on
  Windows 11, `test/unzip_windows.sh` passes for the x86-64 build, the
  amalgamation, and the i686 build, and the amalgamation, built by its
  header's command, imports only KERNEL32 and SHELL32, with no stack
  frame over 4000 bytes.
- zip speed versus Info-ZIP 3.0 on the 267 MB benchmark corpus (Apple
  M-series): -1 2.2 s vs 1.9 s (4% smaller), -6 3.1 s vs 4.9 s, -9 6.8 s
  vs 12.6 s (smaller). 10,000 small files (64 B to 8 KiB, -6): 0.32 s vs
  0.47 s; on a Raspberry Pi 4, 1.7 s vs 2.0 s, and 20,000 files of about
  280 bytes at -9 1.2 s vs 2.0 s.
- zip memory, `-qr` over empty files unless noted, versus Info-ZIP 3.0.
  Before the reserved arena, 250K files ran out of the fixed 256 MiB.
  Apple M-series: 1M files 27 s and 371 MB peak RSS (36 s, 370 MB).
  Merging into an archive of 1M empty files in 1,000 directories (names
  of 17 bytes), in MiB: adding one file 0.15 s and 191 (0.73 s, 255),
  refreshing with `-r` 27 s and 406 (38 s, 309), and with `-ru` 6.1 s
  and 374 (10.7 s, 271); before the read window and the cuts per entry,
  0.73 s and 325, 27 s and 677, and 6.2 s and 531. 100K files of 100
  bytes 5 s and 41 MB (11 s, 40 MB), with a 1M-file subtree excluded by
  `-x` too, scanned at no cost in memory; one directory of 500K files 14
  s and 183 MB (19.5 s, 186 MB), 214 MB when its listing grew by
  doubling. Windows 11: a one-file run commits 7 MB (257 MB committed up
  front before), 100K files 46 MB, 1M files 398 MB (i686 298 MB), both
  measured while each recorded name kept a folded copy and listings were
  converted at three bytes a unit, so lower now (see Memory per entry
  and Scanning); a tree 15,000 levels deep (30K-character paths)
  archives with a peak commit of 439 MiB on x86-64 and 437 MiB on i686
  (673 and 670 MiB while each recorded name kept a folded copy; and
  before each level kept one copy of its path rather than three, i686
  ran out of its 1 GiB reservation with "zip error: Out of memory", 4),
  where the recursive build overflowed its stack by 8,000 levels. WSL:
  the `-m32` build zips 1M files in 4 s and 265 MB, and under `ulimit -v
  200000` both builds exit 4 with that message and leave no temporary
  file. Raspberry Pi 4: 100K files of 100 bytes 3.6 s and 41 MB (8.9 s,
  44 MB), 250K files 5.3 s (14.3 s). Merging there, as above, in CPU
  seconds (the shared Pi's wall times were noise) and MiB: adding one
  file 1.6 s and 190 (6.5 s, 376), `-r` 24 s and 405 (61 s, 452), and
  `-ru` 10 s and 373 (20 s, 414); before, 6.3 s and 325, 26 s and 676,
  and 12 s and 530.
- zip's lazy POSIX commit, measured in overcommit mode 0 by each
  process's charged mappings (`VmFlags` `ac` in `/proc/PID/smaps`) and
  `Committed_AS`. The old reservation without `MAP_NORESERVE`, as strict
  overcommit charges it, cost 16 GiB at startup in WSL (1 GiB for
  `-m32`, 4 GiB on the Pi, whose heuristic refused more). The
  `PROT_NONE` one costs 1 MiB at startup and then what is claimed: 129
  MiB after reading 64 MiB of `-@` names, 257 MiB after 192 MiB. Under
  `ulimit -v` and `ulimit -d` (which refuses the `mprotect`, the strict
  overcommit path), the x86-64, i386, and aarch64 builds, and the
  `/dev/zero` build forced on Linux, complete small runs, exit 4 on 100K
  or 1M files with no archive or temporary file, and leave an archive
  being updated untouched. No cost: user+sys for 100K and 1M empty files
  is unchanged on the Mac (1.71 s, 27.3 s) and in WSL (0.28 s, 2.9 s),
  and within noise on a loaded Pi (100K, mean of 8: 2.28 s before, 2.30
  s after).

## gzip and platform behavior

- Decoder strictness matches zlib exactly: incomplete codes rejected
  except a lone 1-bit code; an empty distance code is an error only when
  used; reserved header flags rejected; FHCRC verified.
- Concatenated members decode in sequence. As in GNU gzip, data after
  the last member is ignored with a warning (exit 2), unless it is only
  zero bytes (padding, as on tape), which is fine, or starts with the
  gzip magic, in which case it must be a valid member. GNU gzip reads
  the magic as a pair, so a lone byte at the end, other than zero, is a
  truncated member (exit 1).
- Also as in GNU gzip, with `-f`, decompressing to standard output
  (`zcat -f`, or from standard input) copies data that is not gzip
  through unchanged, whole or after a member, and `-tf` passes it; in
  place it is still an error. Only gzip is read, so GNU's other input
  formats (compress, pack, LZH, zip) pass through too.
- gzip's exit status: 0 success, 1 error, 2 warning; errors take
  precedence. (zip's follow Info-ZIP: see its section.)
- As in GNU gzip, a read or write error (including a failed close of an
  output) ends the run at once: nothing more of the file is read or
  written, and no later file is begun, where reading on would only waste
  work or, with a closed pipe and no SIGPIPE (Windows), never end. A
  stream that a read error cuts short is left unfinished, so that it
  cannot pass for all of its input. It is an error (1), but as there
  with SIGPIPE ignored, a closed pipe is a warning (2), silent under
  `-q`, since the default SIGPIPE would have ended the run quietly;
  Windows, which has no SIGPIPE, treats one so too.
- Messages give the system's reason for a failure, as GNU gzip's do
  (`gzip: f.gz: No space left on device`), naming the file that failed:
  the input when opening or reading, the output (`stdout` or the new
  file) when creating, writing, or closing, and for a forced output that
  could not be replaced, why it could not be removed. On Windows, every
  error is worded as its C runtime words it (`os_error` in
  `platform/windows.c`, shared with zip), and a failure with no system
  error behind it gets a description instead (`cannot open for reading`,
  `write error`).
- Other messages are worded as GNU gzip 1.14 words them, bad headers
  described from their bytes (`unknown method 7 -- not supported`, `has
  flags 0x40`, `header checksum 0xffff != computed checksum 0x77a7`). As
  there, an existing output is reported even under `-q`, and `-dq` of a
  name without a known suffix skips it with status 0. Known differences:
  a hard-linked input "has other links", where GNU counts them ("has 1
  other link"), since the platform layers report no count; a refused
  symbolic link "is a symbolic link -- ignored" on every platform, where
  GNU gives the reason its open failed ("Too many levels of symbolic
  links"); a failure to set both the mode and the times is one warning,
  where GNU gives one for each; and no message begins with GNU's blank
  line.
- Headers record no name and no time (FLG and MTIME 0), as GNU gzip's
  `-n` headers do, XFL included (4 at `-1`, 2 at `-9`, else 0, as zlib
  sets it too, and which `file` reports as "max speed" or "max
  compression"), so `-n` and `--no-name` are accepted and do nothing.
  `-N`, which asks for a name and time to be saved or restored, is
  refused.
- As in GNU gzip, the program name sets the default mode: names starting
  with `un` or `gun` decompress, and `zcat` or `gzcat` decompress to
  standard output (case-insensitive; Windows drops `.exe`). Platform
  layers pass the name in `config.name`.
- Names follow GNU gzip's default suffixes, in any case: `.gz`, `.z`,
  `-gz`, `-z`, `_z`, and `.tgz` and `.taz`, which stand for `.tar`.
  Decompressing in place drops one; compressing in place leaves a file
  that has one alone, with a notice but status 0, unless `-f`. As there,
  the file is checked (missing, a directory, a link) before its name,
  and decompressing or testing a missing name without a suffix tries
  `.gz`, `.z`, `-z`, and `.Z` after it in turn (`zcat foo` reads
  `foo.gz`), naming `foo.gz` if none exists. The gzip platform layers
  tell a missing file from other failures with `os_missing`.
- In-place operation (no `-c`/`-t`) deletes its input, so its input must
  be a regular file. As in GNU gzip, unless `-f` (then links are
  followed), a symbolic link is refused with an error (1), and a
  hard-linked file, or one with the sticky bit, is skipped with a
  warning (2); FIFOs, devices, directories, and set-user-ID and
  set-group-ID files are always skipped, the last two with a warning
  (2), compressing or decompressing. On Windows a volume, a disk file
  that has no file information to give, is skipped as a device, while a
  file that fails to give it, as on a network error, is an error (1)
  with the reason, as a failed open is. With `-c` or `-t`, anything but
  a directory may be read (e.g. `gzip -c <(cmd)`). Failing to remove the
  input afterward is a warning, as in GNU gzip, leaving both files. On
  Windows a read-only input is removed all the same, as unlinking
  ignores the mode on POSIX, just as `-f` replaces a read-only output.
- Output files are created owner-only, then given the input's ownership
  (when permitted, else its group if the user is in it), permission bits
  (`mode & 0777`, so a forced sticky input's output lacks the bit), and
  timestamps at full resolution, all as the input had them when opened,
  as in GNU gzip, before reading changed its access time. On Windows,
  timestamps are copied, and in place of the mode the read-only
  attribute, the nearest thing to a missing write bit, along with the
  hidden, system, and not-indexed attributes, as zip keeps a replaced
  archive's, while access control is inherited from the directory, as
  with GNU gzip. Also as there, failing to set the mode or times is a
  warning (2) naming the output, which is kept and its input removed,
  while failing to keep the ownership is silent, and failing to read an
  input's metadata is an error (1).
- Outputs are discarded unless explicitly kept after success
  (`os_keep`): on failure, on a failed close (which may mean lost data),
  and on interruption. POSIX uses a handler for SIGHUP, SIGINT, SIGPIPE,
  SIGQUIT, SIGTERM, SIGXCPU, and SIGXFSZ (inherited "ignore"
  dispositions are respected, as under nohup). Windows marks the file
  delete-pending at creation, so even `TerminateProcess` cleans up.
  Clearing that could fail (a file system or filter may refuse), which
  closing would follow by deleting the output, so it is an error like a
  failed close, keeping the input.
- An inherited descriptor left non-blocking (a pipe or terminal another
  program set `O_NONBLOCK` on) is read as GNU gzip reads it: a read that
  finds no input yet clears the flag and waits, rather than failing. The
  flag belongs to the open file, shared with whoever else holds it, but
  as there it is not restored. zip's `-@` reads so too. On Windows,
  likewise, a pipe left non-blocking (`PIPE_NOWAIT`), whose reads fail
  with `ERROR_NO_DATA` until input comes, is set to wait, keeping its
  read mode.
- Files are opened for reading with `O_NOCTTY`, as GNU gzip opens its
  inputs, so that on Linux (and System V) a session leader without a
  controlling terminal, such as a daemon, that reads a terminal (`gzip
  -c /dev/ttyS0`, `zip -i @/dev/tty1`) does not take it on, to be
  interrupted from it later. Info-ZIP's zip, which reads such lists
  through `fopen`, does.
- At startup, a closed standard descriptor is reopened on `/dev/null`,
  keeping output files off it; the opposite access mode makes using it
  fail as before. Without `/dev/null` (some chroots and sandboxes), the
  program instead exits before opening anything, with a message and
  status 1 (gzip) or 10 (zip, a temporary file failure).
- As in GNU gzip, decompressing in place reads the input's header before
  creating the output, so input that is not gzip, or whose header is bad
  or cut short, leaves an existing output alone even under `-f`, and
  without `-f` that is the error (1), not the existing output (2). As
  there, under `-f` a member found corrupt only after its header has
  already replaced the output, which is then removed: keeping the old
  file until success would take a temporary name and a rename, as zip
  does, and on Windows, care for read-only and delete-pending targets.
- `-f` replaces an existing output by unlinking it first, never writing
  through a link. On Windows a read-only output is replaced too, as
  unlinking ignores the mode on POSIX: the attribute is cleared just to
  delete the file, then restored, so other hard links to it keep it.
  Likewise a junction or a directory link is replaced, as a link is on
  POSIX, removing the link alone, never its target nor what is in it
  (`cli.sh`). An output it cannot remove (a directory, even an empty
  one, and on Windows a name another process holds delete-pending) is
  an error (1), as in GNU gzip, while without `-f` it is refused with a
  warning (2).
- Windows paths get the `\\?\` prefix, lifting MAX_PATH. It turns off
  Win32 parsing, so paths are first resolved as Win32 would: against the
  current directory (UNC or not), a drive's, or the root, dropping `.`,
  `..`, and doubled separators. Trailing dots and spaces are kept, so
  names from a listing round trip. A path written exactly `\\?\` passes
  through, as in Win32, but other device paths (`//?/`, `\\.\`) are
  resolved too, as Win32 resolves them, except that `..` stops at their
  volume or share, and dots and spaces stay. A resolved `\\.\` path
  becomes `\\?\`, which names the same devices, since Win32 would parse
  it again, stripping those and limiting it to MAX_PATH (so that
  `//./C:/d/name.` would read `name`). The empty path names no file, nor
  does a root missing its server, share, or device (`\\server\`,
  `//?/`). A bare DOS device name (`NUL`, `CON`, `COM1`, as
  `GetFullPathNameW` judges it) is the device, as in other Windows
  programs: zip skips it as a special file, `gzip -c NUL` reads nothing,
  and no output goes to one. With a directory, such a name is a file, so
  that names from a listing, made on other systems, still round trip.
- On Windows, whose shells pass wildcards to programs as they are, gzip
  expands each argument with `*` or `?` before reading its options, as a
  POSIX shell would, with zip's code (`src/dir.c`, `src/wild.c`): into
  the names it matches, in name order, matched as zip matches them
  (ignoring case beyond ASCII too, by DOS rules, in any component), but
  for hidden and system files, as zip leaves them out and a shell leaves
  out dotfiles, or if it matches nothing, into itself, which then is not
  found, as after a shell (`No such file or directory`, and under `-d`
  for each suffix tried), where Windows would call the name invalid
  (`Invalid argument`: no Windows name holds `*`). Unlike the C
  runtime's expansion, which w64devkit's busybox gzip gets, `*.txt`
  matches long names only (not `long.txtx` by its 8.3 name). Listings
  take three quarters of gzip's fixed 32 MiB meanwhile, and the
  arguments, packed, then made at once, the rest: that holds a directory
  of 170,000 names of 13 characters, but not one of 180,000 (on i686,
  190,000 and 200,000), where names at three bytes a UTF-16 unit, and
  listings and arguments that grew by doubling, held 65,000 but not
  67,000 (85,000 and 100,000). Beyond that gzip runs out of memory (1)
  before doing anything.
- Output to a Windows console is converted from UTF-8 (WTF-8, for file
  names) to UTF-16 for `WriteConsoleW`, since `WriteFile` would take the
  bytes in the console code page. A sequence split between writes is
  held until the next, or written as U+FFFD at exit. Input from one,
  standard input or a console opened by name (`CON`), is read with
  `ReadConsoleW` and converted the other way, since `ReadFile` would
  give it in the console's input code page (437 on biron, where `é` is
  0x82, and `а` a `?`, a wildcard to zip): names typed for zip's `-@`
  are UTF-8 like the rest. As with `ReadFile`, a read that begins with
  Ctrl+Z, a line that starts with it, ends the input. A high surrogate
  at the end of a read waits for its low one. Pipes and files get the
  bytes unchanged. Info-ZIP's port, by its sources, reads such names in
  the console's code page as ANSI ones, so it misreads them too.
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
  only at the missing end-of-block code (zlib decodes it as 1-bit
  zeros); a repeat-previous code is validated after its extra bits; gzip
  magic, method, flags, and trailer CRC are checked as soon as each is
  complete
- streaming inflate looked up a match's distance code even when input
  ran out inside the length's extra bits. With a lone 1-bit or empty
  distance code, an unread extra bit could select an invalid 1-bit
  entry, making the truncation a sticky data error, so valid files from
  Go's compress/gzip (most levels, including its default) failed `gzip
  -t` or the library depending on read boundaries. Fuzzing missed it:
  zlib never writes such codes, and the streaming check's seeds lacked
  its config byte (`test/seeds.py` now writes both; 44 of the new seeds
  each trap `fuzz-diff-inflate` on the old decoder)
- `memcpy` with a null pointer and zero length, from callers passing
  empty null buffers (UBSan under GCC)
- zip: bytes resembling a Zip64 locator before a plain end record made
  the reader insist on Zip64 (fuzzing)
- unzip, found by its in-memory tests and `fuzz-unzip`: memory claimed
  after the first change to the file system (each new name at the
  prompt), a link's target taken from the room planned for its size
  rather than its data, offsets re-compensated for entries never read, a
  Zip64 count of entries past 2^63 taken as too large for memory (4)
  rather than a damaged directory, local names in a code page compared
  undecoded (Windows), `-K` and `-X` refused only after the archive with
  Windows conventions, and standard input closed after reading an
  archive from it; and in inflate, a null input pointer with no input
  offset by zero, which C leaves undefined (UBSan under Linux clang,
  from the first `fuzz-unzip` campaign in WSL)

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
   subtables) replacing bit-at-a-time canonical decoding; 64-bit
   refills; fast loop; decode straight into a history window. ~3x.
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

- Decompression is ~83% of zlib and ~77% of libdeflate. The decode loop
  is bound by the lookup->shift->lookup dependency; multi-symbol tables
  (two literals per entry) would be the next step.
- Level 1 is half libdeflate's speed: per-position overhead (two hash
  chain insertions, chain walk) dominates, not search depth. A
  chainless, bucketed hash table for the fast levels is the likely fix.
- High levels: libdeflate's lazy2 and near-optimal parsing reach
  ~0.2-0.4 points better ratio.

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

Deflate resets: emptying the hash heads costs 5.5 us on the M4 Max and
24 us on the Pi 4 (256 KiB; 7 and 48 us with the 3-byte heads), and
rehashing costs 0.33 and 4 ns per position (0.85 and 8.5 with them), so
the old threshold of 8,192 positions sat near the crossover: raising it
slowed resets after 12-64 KiB streams by 2-7% per stream on the M4 Max,
and lowering it to 4,096 changed little. Clearing only touched slots
cannot help, as hashes spread evenly: 8,192 insertions touch 86% of the
table's cache lines. Stamps (see Library) make forgetting a long history
free, but a lookup that meets a stale entry takes a step more than one
that meets an empty slot, so short histories are still rehashed:
stamping from 256 positions on made 300-byte streams 1-9% slower on the
M4 Max, while from 1,024 on, small streams change by no more than code
placement alone moves them (about 1%). Per stream, best of seven
interleaved runs over slices of dickens and ooffice at levels 1, 6, and
9, change against the previous code:

|                | 100 B, 1 KB | 4 KiB     | 8 KiB    | 16 KiB   | 32-64 KiB          | 2 MB              |
|----------------|-------------|-----------|----------|----------|--------------------|-------------------|
| M4 Max         | -1..+2%     | -4..-7%   | -5..-9%  | -2..-4%  | 0 / -3..-8%        | 0 / -1..-2%       |
| Pi 4 (noisy)   | -1..+2%     | -2..-14%  | -6..-11% | -4..+3%  | -6..+6%            | -11..+5%          |

(M4 Max 32 KiB and up: dickens / ooffice; Pi 4: 64 KiB only. Its L2,
shared with other work, moves runs of 16 KiB and up by about 6%.) gzip
-c over many files, user+sys CPU, median of seven (M4 Max) or five (Pi
4) interleaved runs: 10,000 files of 64 B to 8 KiB took 4-10% less (Pi
4: 6-9%), 3,000 of 8-64 KiB 2-4% less (Pi 4: within 2.3% either way),
and 20,000 of 200-360 B and the Silesia files one at a time no more.
