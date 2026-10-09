# Cross-compiling for 32-bit big-endian PowerPC Linux, the tests run
# under QEMU user mode (see README.md for zlib):
#   $ cmake -B build-ppc -DCMAKE_TOOLCHAIN_FILE=cmake/linux-powerpc.cmake
#           -DTUGZ_BUILD_TESTS=ON -DCMAKE_FIND_ROOT_PATH=/path/to/ppc/zlib
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR powerpc)
set(CMAKE_C_COMPILER powerpc-linux-gnu-gcc)
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-ppc -L /usr/powerpc-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
