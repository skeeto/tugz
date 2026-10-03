#ifndef GZ_GZIP_H
#define GZ_GZIP_H

#include "common.h"
#include "win32.h"

b32 gzip_compress_handle(iptr in, iptr out, i32 level);
b32 gzip_decompress_handle(iptr in, iptr out, b32 verify_only);

#endif