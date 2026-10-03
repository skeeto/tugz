CC ?= gcc
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Wno-unused-parameter -fno-builtin \
          -fno-stack-protector -fno-asynchronous-unwind-tables -fno-ident
LDFLAGS ?= -nostartfiles -Wl,--gc-sections -s
LDLIBS ?= -lmemory -lshell32 -lkernel32
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

build/gzip.exe: $(SRC) $(HDR)
	mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

test: build/gzip.exe
	python run_tests.py --gzip-bin build/gzip.exe

clean:
	rm -rf build

.PHONY: test clean