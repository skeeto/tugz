#include "common.h"
#include "win32.h"
#include "gzip.h"
#include "crc32.h"
#include "deflate.h"

typedef struct {
    i32 level;
    b32 decompress, test, to_stdout, force, keep;
    c16 **files;
    i32 nfiles;
} options;

static b32 c16_is(const c16 *a, const char *b)
{
    while (*b) {
        if (*a++ != (c16)(u8)*b++) {
            return 0;
        }
    }
    return *a == 0;
}

static void usage(iptr h)
{
    out_str(h,
        "usage: gzip [-cfkdt123456789] [FILE]...\n"
        "Compress FILEs (or stdin)\n"
        "  -1..9  Compression level\n"
        "  -d     Decompress\n"
        "  -c     Write to stdout\n"
        "  -f     Force\n"
        "  -k     Keep input files\n"
        "  -t     Test integrity\n");
}

static b32 parse_args(i32 argc, c16 **argv, options *o)
{
    o->level = 6;
    o->files = arena_alloc((iz)argc * (iz)sizeof(c16 *), 8);
    o->nfiles = 0;
    b32 endopts = 0;

    for (i32 i = 1; i < argc; i++) {
        c16 *a = argv[i];
        if (!endopts && a[0] == '-' && a[1] != 0) {
            if (a[1] == '-') {
                if (a[2] == 0) {
                    endopts = 1;
                    continue;
                }
                if (c16_is(a, "--help")) {
                    usage(std_handle(1));
                    return 0;
                }
                if (c16_is(a, "--version")) {
                    out_str(std_handle(1), "gzip (crgzip) 1.0\n");
                    return 0;
                }
                err_msg("unknown option");
                usage(std_handle(2));
                return 0;
            }
            for (i32 j = 1; a[j]; j++) {
                c16 c = a[j];
                if (c >= '1' && c <= '9') {
                    o->level = (i32)(c - '0');
                } else if (c == 'd') {
                    o->decompress = 1;
                } else if (c == 't') {
                    o->test = 1;
                } else if (c == 'c') {
                    o->to_stdout = 1;
                } else if (c == 'f') {
                    o->force = 1;
                } else if (c == 'k') {
                    o->keep = 1;
                } else {
                    err_msg("unknown option");
                    usage(std_handle(2));
                    return 0;
                }
            }
        } else {
            o->files[o->nfiles++] = a;
        }
    }
    return 1;
}

static i32 process_file(const c16 *file, options *o)
{
    arena_mark m = arena_save();

    c16 *in_abs = path_absolute(file);
    if (!in_abs) {
        err_path(file, "invalid path");
        arena_restore(m);
        return 1;
    }

    iptr in = file_open_read(in_abs);
    if (in == INVALID_HANDLE) {
        err_path(file, "cannot open");
        arena_restore(m);
        return 1;
    }

    if (o->test) {
        b32 ok = gzip_decompress_handle(in, 0, 1);
        CloseHandle(in);
        if (!ok) {
            err_path(file, "invalid compressed data");
        }
        arena_restore(m);
        return ok ? 0 : 1;
    }

    if (o->to_stdout) {
        iptr out = std_handle(1);
        b32 ok = o->decompress
            ? gzip_decompress_handle(in, out, 0)
            : gzip_compress_handle(in, out, o->level);
        CloseHandle(in);
        if (!ok) {
            err_path(file, "failed");
        }
        arena_restore(m);
        return ok ? 0 : 1;
    }

    c16 *out_rel = o->decompress ? path_strip_gz(file) : path_append_gz(file);
    if (!out_rel) {
        err_path(file, "unknown suffix -- ignored");
        CloseHandle(in);
        arena_restore(m);
        return 1;
    }
    c16 *out_abs = path_absolute(out_rel);
    if (!out_abs) {
        err_path(out_rel, "invalid path");
        CloseHandle(in);
        arena_restore(m);
        return 1;
    }

    if (!o->force && file_exists(out_abs)) {
        err_path(out_rel, "already exists; not overwritten");
        CloseHandle(in);
        arena_restore(m);
        return 1;
    }

    iptr out = file_open_write(out_abs);
    if (out == INVALID_HANDLE) {
        err_path(out_rel, "cannot create");
        CloseHandle(in);
        arena_restore(m);
        return 1;
    }

    b32 ok = o->decompress
        ? gzip_decompress_handle(in, out, 0)
        : gzip_compress_handle(in, out, o->level);

    CloseHandle(in);
    CloseHandle(out);

    if (ok) {
        if (!o->keep) {
            file_delete(in_abs);
        }
    } else {
        file_delete(out_abs);
        err_path(file, "failed");
    }

    arena_restore(m);
    return ok ? 0 : 1;
}

static i32 run(options *o)
{
    i32 status = 0;

    if (o->nfiles == 0) {
        if (o->test) {
            status = gzip_decompress_handle(std_handle(0), 0, 1) ? 0 : 1;
        } else if (o->decompress) {
            status = gzip_decompress_handle(std_handle(0), std_handle(1), 0) ? 0 : 1;
        } else {
            status = gzip_compress_handle(std_handle(0), std_handle(1), o->level) ? 0 : 1;
        }
        if (status) {
            err_msg("stdin: invalid or unsupported data");
        }
        return status;
    }

    for (i32 i = 0; i < o->nfiles; i++) {
        status |= process_file(o->files[i], o);
    }
    return status;
}

__attribute((force_align_arg_pointer))
void mainCRTStartup(void)
{
    arena_init();
    crc32_init();
    deflate_init_tables();

    i32 argc = 0;
    c16 **argv = win_command_line(&argc);

    options o = {0};
    if (!parse_args(argc, argv, &o)) {
        ExitProcess(1);
    }

    i32 status = run(&o);
    ExitProcess((i32)status);
}