# Development Makefile. Each platform/*_{posix,windows}.c, and each
# test/*.c with an entry point, is a complete unity build, so any program
# can also be built by invoking a compiler directly on it.

CC       = cc
CROSS    = x86_64-w64-mingw32-
OPT      = -O2
WARN     = -Wall -Wextra -Wconversion -Wno-sign-conversion
DEBUG    = -g3 -O1 $(WARN) -fsanitize=address,undefined \
           -fno-sanitize-recover=all
FUZZCC   = clang
FUZZ     = -g3 -O1 $(WARN) -Wno-unused-function \
           -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all
PREFIX   = /opt/homebrew
REFLIBS  = -I$(PREFIX)/include -L$(PREFIX)/lib -ldeflate -lz

WIN32_CFLAGS = -fno-builtin -fno-asynchronous-unwind-tables
WIN32_LIBS   = -nostartfiles -s -Wl,--gc-sections -lmemory -lshell32 -lkernel32

CORE = src/base.c src/crc32.c src/adler32.c src/inflate.c src/deflate.c \
       src/gzip.c
SRC  = $(CORE) src/io.c src/gzipio.c src/cli.c
LIB  = platform/libtugz.c tugz.h $(CORE)
ZIPSRC = src/base.c src/crc32.c src/deflate.c src/io.c src/zip.c src/zipcli.c
POSIX   = platform/posix.c
WINDOWS = platform/windows.c

gzip: platform/gzip_posix.c $(POSIX) $(SRC)
	$(CC) $(OPT) $(WARN) -o $@ platform/gzip_posix.c

gzip-debug: platform/gzip_posix.c $(POSIX) $(SRC)
	$(CC) $(DEBUG) -o $@ platform/gzip_posix.c

gzip.exe: platform/gzip_windows.c $(WINDOWS) $(SRC)
	$(CROSS)gcc $(OPT) $(WARN) $(WIN32_CFLAGS) -o $@ platform/gzip_windows.c $(WIN32_LIBS)

zip: platform/zip_posix.c $(POSIX) $(ZIPSRC)
	$(CC) $(OPT) $(WARN) -o $@ platform/zip_posix.c

zip-debug: platform/zip_posix.c $(POSIX) $(ZIPSRC)
	$(CC) $(DEBUG) -o $@ platform/zip_posix.c

zip.exe: platform/zip_windows.c $(WINDOWS) $(ZIPSRC)
	$(CROSS)gcc $(OPT) $(WARN) $(WIN32_CFLAGS) -o $@ platform/zip_windows.c $(WIN32_LIBS)

# Single-file Windows source, e.g. for w64devkit. The header carries the
# version and build command; local includes are dropped.
amalgamation: gzip.c zip.c
gzip.c: platform/gzip_windows.c $(WINDOWS) $(SRC)
	v=$$(sed -n 's/.*gzip (tugz) \([0-9.]*\).*/\1/p' src/cli.c); \
	{ echo "// tugz $$v: tiny unity gzip, a drop-in gzip for Windows"; \
	  echo "// Single-file amalgamation of the tugz sources. Build:"; \
	  echo "//   \$$ cc -O2 -nostartfiles -o gzip.exe gzip.c -lmemory"; \
	  echo "// Copies named gunzip.exe or zcat.exe decompress by default."; \
	  echo; \
	  awk 'FNR==1 && NR>1 {print ""} !/^#include "/ && !/^\/\/ +\$$ cc/' \
	      $(SRC) $(WINDOWS) platform/gzip_windows.c; } >$@

zip.c: platform/zip_windows.c $(WINDOWS) $(ZIPSRC)
	v=$$(sed -n 's/.*tugz zip \([0-9][0-9.]*\).*/\1/p' src/zipcli.c); \
	{ echo "// tugz zip $$v: an Info-ZIP compatible zip for Windows"; \
	  echo "// Single-file amalgamation of the tugz sources. Build:"; \
	  echo "//   \$$ cc -O2 -nostartfiles -o zip.exe zip.c -lmemory"; \
	  echo; \
	  awk 'FNR==1 && NR>1 {print ""} !/^#include "/ && !/^\/\/ +\$$ cc/' \
	      $(ZIPSRC) $(WINDOWS) platform/zip_windows.c; } >$@

tests: test/tests.c $(SRC)
	$(CC) $(DEBUG) -o $@ test/tests.c $(REFLIBS)

tests-lib: test/libtests.c $(LIB)
	$(CC) $(DEBUG) -o $@ test/libtests.c $(REFLIBS)

tests-zip: test/ziptests.c src/base.c src/zip.c
	$(CC) $(DEBUG) -o $@ test/ziptests.c

check: tests tests-lib tests-zip gzip zip-debug
	./tests
	./tests-lib
	./tests-zip
	sh test/cli.sh ./gzip
	sh test/zip.sh ./zip-debug

# The library: an object exporting only the tugz.h interface
libtugz.o: $(LIB)
	$(CC) -c $(OPT) $(WARN) -o $@ platform/libtugz.c

# Single-file library source with its header inlined. Define TUGZ_API as
# static before including it to embed the library in another program.
tugz.c: $(LIB)
	v=$$(sed -n 's/.*gzip (tugz) \([0-9.]*\).*/\1/p' src/cli.c); \
	{ echo "// tugz $$v: streaming DEFLATE, zlib, and gzip library"; \
	  echo "// Single-file amalgamation of the tugz sources. Build:"; \
	  echo "//   \$$ cc -c -O2 tugz.c"; \
	  echo "// The interface documentation follows."; \
	  echo; \
	  awk 'FNR==1 && NR>1 {print ""} !/^#include "/ && !/^\/\/ +\$$ cc/' \
	      tugz.h $(CORE) platform/libtugz.c; } >$@

fuzz-inflate: test/fuzz_inflate.c test/fuzzos.c $(SRC)
	$(FUZZCC) $(FUZZ) -o $@ test/fuzz_inflate.c

fuzz-roundtrip: test/fuzz_roundtrip.c test/fuzzos.c $(SRC)
	$(FUZZCC) $(FUZZ) -o $@ test/fuzz_roundtrip.c $(REFLIBS)

fuzz-diff-inflate: test/fuzz_diff_inflate.c test/fuzzos.c $(SRC)
	$(FUZZCC) $(FUZZ) -o $@ test/fuzz_diff_inflate.c $(REFLIBS)

fuzz-diff-deflate: test/fuzz_diff_deflate.c test/fuzzos.c $(SRC)
	$(FUZZCC) $(FUZZ) -o $@ test/fuzz_diff_deflate.c $(REFLIBS)

fuzz-zipread: test/fuzz_zipread.c src/base.c src/zip.c
	$(FUZZCC) $(FUZZ) -o $@ test/fuzz_zipread.c

fuzz: fuzz-inflate fuzz-roundtrip fuzz-diff-inflate fuzz-diff-deflate \
      fuzz-zipread

fuzz-seeds:
	uv run --no-project python test/seeds.py

bench: test/bench.c $(SRC)
	$(CC) -O2 $(WARN) -Wno-unused-function -o $@ test/bench.c $(REFLIBS)

clean:
	rm -rf gzip gzip-debug gzip.exe gzip.c zip zip-debug zip.exe zip.c tests tests-zip tests-lib bench *.dSYM \
	       libtugz.o tugz.c \
	       fuzz-inflate fuzz-roundtrip fuzz-diff-inflate fuzz-diff-deflate

.PHONY: amalgamation check fuzz fuzz-seeds clean
