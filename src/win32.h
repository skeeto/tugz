#ifndef GZ_WIN32_H
#define GZ_WIN32_H

#include "common.h"

typedef unsigned short c16;
typedef uz iptr;
typedef uz uptr;

#define INVALID_HANDLE ((iptr)-1)

#define W32(r) __declspec(dllimport) r __stdcall
W32(b32) CloseHandle(iptr);
W32(void) ExitProcess(i32) __attribute((noreturn));

typedef struct {
    iptr h;
    u8 *buf;
    iz cap, pos, len;
    b32 eof, err;
} reader;

typedef struct {
    iptr h;
    u8 *buf;
    iz cap, len;
    b32 err, null;
} writer;

void reader_init(reader *r, iptr h, u8 *buf, iz cap);
iz reader_read(reader *r, void *dst, iz n);
i32 reader_byte(reader *r);

void writer_init(writer *w, iptr h, u8 *buf, iz cap, b32 null);
b32 writer_flush(writer *w);
b32 writer_write(writer *w, const void *p, iz n);
void writer_byte(writer *w, u8 b);

iptr file_open_read(const c16 *path);
iptr file_open_write(const c16 *path);
b32 file_exists(const c16 *path);
b32 file_delete(const c16 *path);

c16 *path_absolute(const c16 *path);
c16 *path_append_gz(const c16 *path);
c16 *path_strip_gz(const c16 *path);
iz c16_len(const c16 *s);

iptr std_handle(i32 which);
c16 **win_command_line(i32 *argc);
void out_str(iptr h, const char *s);
void err_msg(const char *msg);
void err_path(const c16 *path, const char *msg);

#endif