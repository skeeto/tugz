// Windows test helper for test/cli.sh and test/xp.sh: the steps a shell
// cannot take itself, with nothing newer than Windows XP (no PowerShell,
// no mklink, no attrib +i there). Built by the scripts at test time:
//   cc -o winhelp.exe test/winhelp.c
//
// A PATH is made absolute and taken as a \\?\ path, so that it may be
// longer than MAX_PATH, unless it is one already (taken as it is, trailing
// dots and spaces too). In a PATH, \uXXXX stands for a UTF-16 code unit,
// so that names need no code page.
//
//   attrs PATH                   its attributes, as PowerShell names them
//   attrib [+-][rhsai]... PATH   set or clear attributes, as attrib does
//                                (i is not-content-indexed, as on Vista)
//   write PATH TEXT              create or replace a file holding TEXT,
//                                with any missing parent directories
//   mkdir PATH                   a directory, and its missing parents
//   size PATH BYTES              create a file or set its size, where
//                                what it gains reads as zeros, unwritten
//   cat PATH                     a file's contents to standard output
//   rmtree PATH                  remove a file or a tree, read-only files
//                                too, junctions without what they reach
//   junction LINK TARGET         a directory junction (mklink /J)
//   pend PATH HELD DONE          create PATH and hold it delete-pending,
//                                creating HELD, until DONE exists (30 s)
//   hold PATH HELD DONE          likewise hold PATH open, sharing nothing
//   nowait CMDLINE INPUT OUT ERR MSG
//                                run CMDLINE with standard input a pipe
//                                left PIPE_NOWAIT, or (MSG 1) a pipe of
//                                messages, given INPUT after a second, as
//                                one write; print its exit status
//   ctrl c|b PATTERN SIZE OUT ERR PROGRAM [ARG]...
//                                run PROGRAM, standard output and error to
//                                OUT and ERR, until a file matching
//                                PATTERN exists and PROGRAM has written
//                                SIZE bytes (to any file), then send it
//                                Ctrl+C or Ctrl+Break; print its exit
//                                status in hex, or why there is none
//
// ctrl isolates the event: GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)
// reaches every process on the console, which would include the shell
// running the test and, over SSH, the session. So ctrl starts itself
// again in a console of its own (CREATE_NEW_CONSOLE, its window hidden),
// whose only processes are that copy and PROGRAM, and the event goes
// there. (Ctrl+Break goes to PROGRAM alone, started in a process group
// of its own, which ignores Ctrl+C, as only Ctrl+Break can be sent to a
// group.) The ctrl that the caller runs waits for that copy, and exits
// with its status.
//
// The NtSetInformationFile that pend uses is looked up at run time, and
// GetProcessIoCounters, which ctrl uses, is Windows 2000's, so this
// imports nothing XP lacks.
//
// Exit status: 0 on success, 1 on failure, 2 on bad usage.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG (WINAPI *NtSetInformationFile_t)(HANDLE, void *, void *,
                                              ULONG, int);

static void *xalloc(size_t n)
{
    void *p = calloc(1, n);
    if (!p) {
        fputs("winhelp: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

// Decode \uXXXX escapes.
static WCHAR *unescape(WCHAR *s)
{
    size_t n = wcslen(s);
    WCHAR *r = xalloc((n + 1) * sizeof(*r));
    WCHAR *d = r;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && s[i+1] == 'u') {  // (NUL ends a short one)
            int ok = 1;
            unsigned v = 0;
            for (int j = 2; j < 6; j++) {
                WCHAR c = s[i+j];
                if      (c >= '0' && c <= '9') v = v*16 + (c - '0');
                else if (c >= 'a' && c <= 'f') v = v*16 + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v = v*16 + (c - 'A' + 10);
                else { ok = 0; break; }
            }
            if (ok) {
                *d++ = (WCHAR)v;
                i += 5;
                continue;
            }
        }
        *d++ = s[i];
    }
    *d = 0;
    return r;
}

// The \\?\ path for an argument: escapes decoded, made absolute against
// the working directory, with / as \.
static WCHAR *fullpath(WCHAR *arg)
{
    WCHAR *s = unescape(arg);
    if (!wcsncmp(s, L"\\\\?\\", 4)) {
        return s;
    }
    for (WCHAR *p = s; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    WCHAR cwd[MAX_PATH*4];
    DWORD n = GetCurrentDirectoryW(MAX_PATH*4, cwd);
    if (!n || n >= MAX_PATH*4) {
        return s;
    }
    size_t len = wcslen(s) + n + 8;
    WCHAR *r = xalloc(len * sizeof(*r));
    wcscpy(r, L"\\\\?\\");
    if (s[0] && s[1] == ':' && s[2] == '\\') {
        // C:\...
    } else if (s[0] == '\\') {
        wcsncat(r, cwd, 2);  // \... on this drive
    } else {
        wcscat(r, cwd);
        if (r[wcslen(r)-1] != '\\') wcscat(r, L"\\");
    }
    wcscat(r, s);
    // Drop "." components, which a \\?\ path would keep
    for (WCHAR *p; (p = wcsstr(r + 4, L"\\.\\"));) {
        memmove(p, p + 2, (wcslen(p + 2) + 1) * sizeof(*p));
    }
    free(s);
    return r;
}

// Create each missing directory leading to a \\?\ path's last component.
static void parents(WCHAR *path)
{
    WCHAR *p = path + 4;
    if (p[0] && p[1] == ':') p += 3;   // past the drive's root
    for (; *p; p++) {
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryW(path, 0);
            *p = '\\';
        }
    }
}

static char *utf8(WCHAR *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, 0, 0, 0, 0);
    char *r = xalloc(n + 1);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, r, n, 0, 0);
    return r;
}

static void touch(WCHAR *path)
{
    CloseHandle(CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0));
}

static void await(WCHAR *path)
{
    for (int i = 0; i < 600; i++) {
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) break;
        Sleep(50);
    }
}

static int rmtree(WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        return 1;
    }
    if (a & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(path, a & ~FILE_ATTRIBUTE_READONLY);
    }
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        return !DeleteFileW(path);
    }
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        size_t n = wcslen(path);
        WCHAR *pat = xalloc((n + 4) * sizeof(*pat));
        wcscpy(pat, path);
        wcscat(pat, L"\\*");
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                WCHAR *f = fd.cFileName;
                if (!wcscmp(f, L".") || !wcscmp(f, L"..")) continue;
                WCHAR *sub = xalloc((n + wcslen(f) + 2) * sizeof(*sub));
                wcscpy(sub, path);
                wcscat(sub, L"\\");
                wcscat(sub, f);
                rmtree(sub);
                free(sub);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        free(pat);
    }
    return !RemoveDirectoryW(path);
}

// Append an argument to a command line, quoted as CommandLineToArgvW
// and the C runtime parse it.
static void quote(WCHAR *cmd, WCHAR *arg)
{
    WCHAR *d = cmd + wcslen(cmd);
    if (d != cmd) *d++ = ' ';
    *d++ = '"';
    for (WCHAR *s = arg;; s++) {
        int slashes = 0;
        for (; *s == '\\'; s++) slashes++;
        if (!*s) {
            for (int i = 0; i < slashes*2; i++) *d++ = '\\';
            break;
        }
        if (*s == '"') slashes = slashes*2 + 1;
        for (int i = 0; i < slashes; i++) *d++ = '\\';
        *d++ = *s;
    }
    *d++ = '"';
    *d = 0;
}

static WCHAR *cmdline(int argc, WCHAR **argv)
{
    size_t len = 1;
    for (int i = 0; i < argc; i++) {
        len += wcslen(argv[i])*2 + 3;
    }
    WCHAR *cmd = xalloc(len * sizeof(*cmd));
    for (int i = 0; i < argc; i++) {
        quote(cmd, argv[i]);
    }
    return cmd;
}

static HANDLE inheritable(HANDLE h)
{
    HANDLE r = INVALID_HANDLE_VALUE;
    DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &r, 0,
                    TRUE, DUPLICATE_SAME_ACCESS);
    return r;
}

// ctrl, outside: run the inside in a hidden console of its own.
static int ctrl(int argc, WCHAR **argv)
{
    WCHAR self[MAX_PATH*4];
    GetModuleFileNameW(0, self, MAX_PATH*4);
    WCHAR *inner[argc];
    inner[0] = self;
    inner[1] = L"ctrl-inside";
    for (int i = 2; i < argc; i++) inner[i] = argv[i];
    WCHAR *cmd = cmdline(argc, inner);

    STARTUPINFOW si = {0};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = inheritable(GetStdHandle(STD_INPUT_HANDLE));
    si.hStdOutput  = inheritable(GetStdHandle(STD_OUTPUT_HANDLE));
    si.hStdError   = inheritable(GetStdHandle(STD_ERROR_HANDLE));
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(self, cmd, 0, 0, TRUE, CREATE_NEW_CONSOLE, 0, 0,
                        &si, &pi)) {
        printf("cannot start: %lu\n", GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD st = 1;
    GetExitCodeProcess(pi.hProcess, &st);
    return (int)st;
}

static BOOL WINAPI ignore(DWORD e)
{
    (void)e;
    return TRUE;
}

static int matches(WCHAR *pattern)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    FindClose(h);
    return 1;
}

// Bytes a process has written so far, to any file. (An output's own
// size cannot be asked: tugz's programs hold theirs delete-pending until
// done, and a directory's record of the size lags.)
static long long written(HANDLE process)
{
    IO_COUNTERS io;
    if (!GetProcessIoCounters(process, &io)) {
        return 0;
    }
    return (long long)io.WriteTransferCount;
}

// ctrl, inside its own console
static int ctrlinside(int argc, WCHAR **argv)
{
    if (argc < 8) return 2;
    WCHAR ev = argv[2][0];
    WCHAR *pattern = fullpath(argv[3]);
    long long want = _wtoi64(argv[4]);

    // Nothing but this report reaches the caller's standard output
    SetHandleInformation(GetStdHandle(STD_OUTPUT_HANDLE),
                         HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(GetStdHandle(STD_INPUT_HANDLE),
                         HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(GetStdHandle(STD_ERROR_HANDLE),
                         HANDLE_FLAG_INHERIT, 0);
    SetConsoleCtrlHandler(0, FALSE);  // Ctrl+C not ignored, if inherited

    SECURITY_ATTRIBUTES sa = {sizeof(sa), 0, TRUE};
    HANDLE out = CreateFileW(fullpath(argv[5]), GENERIC_WRITE,
                             FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, 0);
    HANDLE err = CreateFileW(fullpath(argv[6]), GENERIC_WRITE,
                             FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, 0);
    HANDLE in  = CreateFileW(L"NUL", GENERIC_READ, 0, &sa, OPEN_EXISTING,
                             0, 0);
    if (out == INVALID_HANDLE_VALUE || err == INVALID_HANDLE_VALUE) {
        puts("cannot open the output files");
        return 1;
    }
    STARTUPINFOW si = {0};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = in;
    si.hStdOutput = out;
    si.hStdError  = err;
    PROCESS_INFORMATION pi;
    DWORD flags = ev == 'b' ? CREATE_NEW_PROCESS_GROUP : 0;
    WCHAR *cmd = cmdline(argc - 7, argv + 7);
    if (!CreateProcessW(0, cmd, 0, 0, TRUE, flags, 0, 0, &si, &pi)) {
        printf("cannot start: %lu\n", GetLastError());
        return 1;
    }
    CloseHandle(out);
    CloseHandle(err);
    SetConsoleCtrlHandler(ignore, TRUE);  // this copy is on the console too

    for (DWORD t0 = GetTickCount();;) {
        if (matches(pattern) && written(pi.hProcess) >= want) break;
        if (WaitForSingleObject(pi.hProcess, 2) == WAIT_OBJECT_0) {
            puts("finished");
            return 1;
        }
        if (GetTickCount() - t0 > 300000) {
            puts("no match");
            TerminateProcess(pi.hProcess, 9);
            return 1;
        }
    }
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
        puts("finished");
        return 1;
    }
    BOOL ok;
    if (ev == 'b') {
        ok = GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pi.dwProcessId);
    } else {
        ok = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
    }
    if (!ok) {
        printf("no event: %lu\n", GetLastError());
        TerminateProcess(pi.hProcess, 9);
        return 1;
    }
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) {
        puts("hung");
        TerminateProcess(pi.hProcess, 9);
        return 1;
    }
    DWORD st;
    GetExitCodeProcess(pi.hProcess, &st);
    printf("%08lX\n", st);
    return 0;
}

static int nowait(WCHAR **argv)
{
    HANDLE r, w;
    if (argv[6][0] == '1') {
        WCHAR name[64];
        _snwprintf(name, 64, L"\\\\.\\pipe\\tugz-winhelp-%lu",
                 GetCurrentProcessId());
        r = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND,
                             PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE,
                             1, 0, 0, 0, 0);
        w = CreateFileW(name, GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
    } else {
        CreatePipe(&r, &w, 0, 0);
        DWORD mode = PIPE_NOWAIT;
        SetNamedPipeHandleState(r, &mode, 0, 0);
    }
    SECURITY_ATTRIBUTES sa = {sizeof(sa), 0, TRUE};
    HANDLE o = CreateFileW(fullpath(argv[4]), GENERIC_WRITE, 0, &sa,
                           CREATE_ALWAYS, 0, 0);
    HANDLE e = CreateFileW(fullpath(argv[5]), GENERIC_WRITE, 0, &sa,
                           CREATE_ALWAYS, 0, 0);
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    STARTUPINFOW si = {0};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = r;
    si.hStdOutput = o;
    si.hStdError  = e;
    PROCESS_INFORMATION pi;
    WCHAR *cmd = xalloc((wcslen(argv[2]) + 1) * sizeof(*cmd));
    wcscpy(cmd, argv[2]);
    if (!CreateProcessW(0, cmd, 0, 0, TRUE, 0, 0, 0, &si, &pi)) {
        puts("-1");
        return 0;
    }
    CloseHandle(r);
    CloseHandle(o);
    CloseHandle(e);
    Sleep(1000);
    HANDLE in = CreateFileW(fullpath(argv[3]), GENERIC_READ,
                            FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    DWORD size = GetFileSize(in, 0), n = 0, done;
    char *data = xalloc(size + 1);
    ReadFile(in, data, size, &n, 0);
    WriteFile(w, data, n, &done, 0);
    CloseHandle(w);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("%lu\n", code);
    return 0;
}

static int junction(WCHAR *linkarg, WCHAR *targetarg)
{
    WCHAR *link = fullpath(linkarg);
    WCHAR *target = fullpath(targetarg);
    target[1] = '?';  // \\?\ to \??\, the NT form a junction holds
    if (!CreateDirectoryW(link, 0)) {
        return 1;
    }
    HANDLE h = CreateFileW(link, GENERIC_WRITE, 0, 0, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS |
                           FILE_FLAG_OPEN_REPARSE_POINT, 0);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link);
        return 1;
    }
    // REPARSE_DATA_BUFFER, mount point: substitute name, then an empty
    // print name, each terminated
    USHORT slen = (USHORT)(wcslen(target) * 2);
    static BYTE buf[16 + 2*MAX_PATH*4 + 4];
    if (slen > sizeof(buf) - 20) return 1;
    *(DWORD  *)(buf+0)  = IO_REPARSE_TAG_MOUNT_POINT;
    *(USHORT *)(buf+4)  = (USHORT)(8 + slen + 2 + 2);  // data length
    *(USHORT *)(buf+8)  = 0;                            // substitute offset
    *(USHORT *)(buf+10) = slen;
    *(USHORT *)(buf+12) = (USHORT)(slen + 2);           // print offset
    *(USHORT *)(buf+14) = 0;
    memcpy(buf+16, target, slen);
    DWORD n;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf,
                              8 + *(USHORT *)(buf+4), 0, 0, &n, 0);
    CloseHandle(h);
    if (!ok) {
        fprintf(stderr, "winhelp: junction: error %lu\n", GetLastError());
        RemoveDirectoryW(link);
    }
    return !ok;
}

static int attrs(WCHAR *arg)
{
    static const struct { DWORD bit; char *name; } names[] = {
        {0x0001, "ReadOnly"},  {0x0002, "Hidden"},     {0x0004, "System"},
        {0x0010, "Directory"}, {0x0020, "Archive"},    {0x0040, "Device"},
        {0x0080, "Normal"},    {0x0100, "Temporary"},  {0x0200, "SparseFile"},
        {0x0400, "ReparsePoint"}, {0x0800, "Compressed"},
        {0x1000, "Offline"},   {0x2000, "NotContentIndexed"},
        {0x4000, "Encrypted"},
    };
    DWORD a = GetFileAttributesW(fullpath(arg));
    if (a == INVALID_FILE_ATTRIBUTES) {
        return 1;
    }
    char *sep = "";
    for (int i = 0; i < (int)(sizeof(names)/sizeof(*names)); i++) {
        if (a & names[i].bit) {
            printf("%s%s", sep, names[i].name);
            sep = ", ";
        }
    }
    puts("");
    return 0;
}

static int attrib(int argc, WCHAR **argv)
{
    WCHAR *path = fullpath(argv[argc-1]);
    DWORD a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        return 1;
    }
    for (int i = 2; i < argc-1; i++) {
        WCHAR *s = argv[i];
        DWORD bit = 0;
        switch (s[1] | 0x20) {
        case 'r': bit = FILE_ATTRIBUTE_READONLY; break;
        case 'h': bit = FILE_ATTRIBUTE_HIDDEN; break;
        case 's': bit = FILE_ATTRIBUTE_SYSTEM; break;
        case 'a': bit = FILE_ATTRIBUTE_ARCHIVE; break;
        case 'i': bit = FILE_ATTRIBUTE_NOT_CONTENT_INDEXED; break;
        }
        if (!bit || (s[0] != '+' && s[0] != '-') || s[2]) return 2;
        a = s[0] == '+' ? a | bit : a & ~bit;
    }
    a &= ~FILE_ATTRIBUTE_DIRECTORY;
    return !SetFileAttributesW(path, a ? a : FILE_ATTRIBUTE_NORMAL);
}

static int cat(WCHAR *arg)
{
    HANDLE h = CreateFileW(fullpath(arg), GENERIC_READ, FILE_SHARE_READ, 0,
                           OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    static char buf[1<<16];
    for (DWORD n, w; ReadFile(h, buf, sizeof(buf), &n, 0) && n;) {
        if (!WriteFile(out, buf, n, &w, 0) || w != n) return 1;
    }
    CloseHandle(h);
    return 0;
}

static int writefile(WCHAR *arg, WCHAR *text)
{
    WCHAR *path = fullpath(arg);
    parents(path);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    char *s = utf8(text);
    DWORD n;
    BOOL ok = WriteFile(h, s, (DWORD)strlen(s), &n, 0);
    return !(CloseHandle(h) && ok);
}

static int hold(WCHAR **argv, int pend)
{
    WCHAR *path = fullpath(argv[2]);
    HANDLE h;
    if (pend) {
        NtSetInformationFile_t set = (NtSetInformationFile_t)(void *)
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                           "NtSetInformationFile");
        h = CreateFileW(path, GENERIC_WRITE|DELETE, 0, 0, CREATE_NEW, 0, 0);
        if (h == INVALID_HANDLE_VALUE || !set) {
            return 1;
        }
        ULONG_PTR iosb[2];
        BOOLEAN discard = TRUE;
        if (set(h, iosb, &discard, sizeof(discard),
                13)) {  // FileDispositionInformation
            return 1;
        }
    } else {
        h = CreateFileW(path, GENERIC_READ|GENERIC_WRITE, 0, 0,
                        OPEN_EXISTING, 0, 0);
        if (h == INVALID_HANDLE_VALUE) {
            return 1;
        }
    }
    touch(fullpath(argv[3]));
    await(fullpath(argv[4]));
    CloseHandle(h);
    return 0;
}

int main(void)
{
    int argc;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) {
        fputs("usage: winhelp COMMAND ARG... (see winhelp.c)\n", stderr);
        return 2;
    }
    WCHAR *c = argv[1];
    if (!wcscmp(c, L"attrs")) {
        return attrs(argv[2]);
    } else if (!wcscmp(c, L"attrib") && argc >= 4) {
        return attrib(argc, argv);
    } else if (!wcscmp(c, L"write") && argc == 4) {
        return writefile(argv[2], argv[3]);
    } else if (!wcscmp(c, L"mkdir")) {
        WCHAR *path = fullpath(argv[2]);
        parents(path);
        return !CreateDirectoryW(path, 0);
    } else if (!wcscmp(c, L"size") && argc == 4) {
        HANDLE h = CreateFileW(fullpath(argv[2]), GENERIC_WRITE, 0, 0,
                               OPEN_ALWAYS, 0, 0);
        LARGE_INTEGER size;
        size.QuadPart = _wtoi64(argv[3]);
        BOOL ok = h != INVALID_HANDLE_VALUE &&
                  SetFilePointerEx(h, size, 0, FILE_BEGIN) && SetEndOfFile(h);
        return !(ok && CloseHandle(h));
    } else if (!wcscmp(c, L"cat")) {
        return cat(argv[2]);
    } else if (!wcscmp(c, L"rmtree")) {
        return rmtree(fullpath(argv[2]));
    } else if (!wcscmp(c, L"junction") && argc == 4) {
        return junction(argv[2], argv[3]);
    } else if (!wcscmp(c, L"pend") && argc == 5) {
        return hold(argv, 1);
    } else if (!wcscmp(c, L"hold") && argc == 5) {
        return hold(argv, 0);
    } else if (!wcscmp(c, L"nowait") && argc == 7) {
        return nowait(argv);
    } else if (!wcscmp(c, L"ctrl") && argc >= 8) {
        return ctrl(argc, argv);
    } else if (!wcscmp(c, L"ctrl-inside")) {
        return ctrlinside(argc, argv);
    }
    fputs("winhelp: bad usage (see winhelp.c)\n", stderr);
    return 2;
}
