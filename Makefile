# Development Makefile. Each main_*.c is a complete unity build, so any
# program can also be built by invoking a compiler directly on it.

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

SRC = src/base.c src/crc32.c src/inflate.c src/deflate.c src/gzip.c src/cli.c

gzip: main_posix.c $(SRC)
	$(CC) $(OPT) $(WARN) -o $@ main_posix.c

gzip-debug: main_posix.c $(SRC)
	$(CC) $(DEBUG) -o $@ main_posix.c

gzip.exe: main_windows.c $(SRC)
	$(CROSS)gcc $(OPT) $(WARN) $(WIN32_CFLAGS) -o $@ main_windows.c $(WIN32_LIBS)

# Single-file Windows source, e.g. for w64devkit. The header carries the
# version and build command; local includes are dropped.
amalgamation: gzip.c
gzip.c: main_windows.c $(SRC)
	v=$$(sed -n 's/.*gzip (tugz) \([0-9.]*\).*/\1/p' src/cli.c); \
	{ echo "// tugz $$v: tiny unity gzip, a drop-in gzip for Windows"; \
	  echo "// Single-file amalgamation of the tugz sources. Build:"; \
	  echo "//   \$$ cc -O2 -nostartfiles -o gzip.exe gzip.c -lmemory"; \
	  echo "// Copies named gunzip.exe or zcat.exe decompress by default."; \
	  echo; \
	  awk 'FNR==1 && NR>1 {print ""} !/^#include "/ && !/^\/\/ +\$$ cc/' \
	      $(SRC) main_windows.c; } >$@

tests: main_test.c $(SRC)
	$(CC) $(DEBUG) -o $@ main_test.c $(REFLIBS)

check: tests gzip
	./tests
	sh test/cli.sh ./gzip

fuzz-inflate: main_fuzz_inflate.c test/fuzzos.c $(SRC)
	$(FUZZCC) $(FUZZ) -o $@ main_fuzz_inflate.c

fuzz-roundtrip: main_fuzz_roundtrip.c test/fuzzos.c $(SRC)
	$(FUZZCC) $(FUZZ) -o $@ main_fuzz_roundtrip.c $(REFLIBS)

fuzz-diff-inflate: main_fuzz_diff_inflate.c test/fuzzos.c $(SRC)
	$(FUZZCC) $(FUZZ) -o $@ main_fuzz_diff_inflate.c $(REFLIBS)

fuzz-diff-deflate: main_fuzz_diff_deflate.c test/fuzzos.c $(SRC)
	$(FUZZCC) $(FUZZ) -o $@ main_fuzz_diff_deflate.c $(REFLIBS)

fuzz: fuzz-inflate fuzz-roundtrip fuzz-diff-inflate fuzz-diff-deflate

fuzz-seeds:
	uv run --no-project python test/seeds.py

bench: main_bench.c $(SRC)
	$(CC) -O2 $(WARN) -Wno-unused-function -o $@ main_bench.c $(REFLIBS)

clean:
	rm -rf gzip gzip-debug gzip.exe gzip.c tests bench *.dSYM \
	       fuzz-inflate fuzz-roundtrip fuzz-diff-inflate fuzz-diff-deflate

.PHONY: amalgamation check fuzz fuzz-seeds clean
