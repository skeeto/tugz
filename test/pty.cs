// A pseudo console for the Windows tests (zip_windows.sh, unzip_windows.sh):
// Pty.Run runs a command in one, typing into it as a user would, and
// Pty.Interrupt one whose output goes to files, interrupting it.
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
public static class Pty {
    [StructLayout(LayoutKind.Sequential)]
    struct Coord { public short x, y; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct StartupInfoEx {
        public int cb;
        public string reserved, desktop, title;
        public int x, y, w, h, cols, rows, fill, flags;
        public short show, reserved2;
        public IntPtr reserved3, stdin, stdout, stderr, attributes;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInfo {
        public IntPtr process, thread;
        public int pid, tid;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct SecurityAttributes {
        public int len;
        public IntPtr descriptor;
        public bool inherit;
    }
    [DllImport("kernel32.dll")]
    static extern bool CreatePipe(out IntPtr r, out IntPtr w, IntPtr sa, int n);
    [DllImport("kernel32.dll")]
    static extern int CreatePseudoConsole(Coord size, IntPtr input,
                                          IntPtr output, int flags,
                                          out IntPtr pc);
    [DllImport("kernel32.dll")]
    static extern void ClosePseudoConsole(IntPtr pc);
    [DllImport("kernel32.dll")]
    static extern bool InitializeProcThreadAttributeList(IntPtr list, int n,
                                                         int flags,
                                                         ref IntPtr size);
    [DllImport("kernel32.dll")]
    static extern bool UpdateProcThreadAttribute(IntPtr list, int flags,
                                                 IntPtr attr, IntPtr value,
                                                 IntPtr size, IntPtr prev,
                                                 IntPtr retsize);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern bool CreateProcessW(string app, string cmd, IntPtr pa,
                                      IntPtr ta, bool inherit, int flags,
                                      IntPtr env, string dir,
                                      ref StartupInfoEx si,
                                      out ProcessInfo pi);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, int share,
                                     ref SecurityAttributes sa, int disp,
                                     int flags, IntPtr template);
    [DllImport("kernel32.dll")]
    static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll")]
    static extern bool ReadFile(IntPtr h, byte[] b, int n, out int done,
                                IntPtr o);
    [DllImport("kernel32.dll")]
    static extern bool WriteFile(IntPtr h, byte[] b, int n, out int done,
                                 IntPtr o);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")]
    static extern int WaitForSingleObject(IntPtr h, int ms);
    [DllImport("kernel32.dll")]
    static extern bool TerminateProcess(IntPtr h, int code);
    [DllImport("kernel32.dll")]
    static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    static IntPtr output;
    static void Drain() {
        byte[] b = new byte[4096];
        int n;
        while (ReadFile(output, b, b.Length, out n, IntPtr.Zero) && n > 0) {}
    }
    // Run a command in a new pseudo console, typing each string into it
    // in turn ("\r" is Enter), and return its status: -1 if it ran past
    // 20 seconds, or -2 without pseudo consoles.
    public static int Run(string cmd, string[] typed) {
        IntPtr inr, inw, outw, pc;
        CreatePipe(out inr, out inw, IntPtr.Zero, 0);
        CreatePipe(out output, out outw, IntPtr.Zero, 0);
        Coord size = new Coord();
        size.x = 120;
        size.y = 30;
        try {
            if (CreatePseudoConsole(size, inr, outw, 0, out pc) != 0) {
                return -2;
            }
        } catch (EntryPointNotFoundException) {
            return -2;
        }
        Thread drain = new Thread(Drain);
        drain.Start();
        IntPtr len = IntPtr.Zero;
        InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref len);
        StartupInfoEx si = new StartupInfoEx();
        si.cb = Marshal.SizeOf(si);
        si.flags = 0x100;  // STARTF_USESTDHANDLES, null: the console's
        si.attributes = Marshal.AllocHGlobal(len);
        InitializeProcThreadAttributeList(si.attributes, 1, 0, ref len);
        UpdateProcThreadAttribute(si.attributes, 0, (IntPtr)0x20016, pc,
                                  (IntPtr)IntPtr.Size, IntPtr.Zero,
                                  IntPtr.Zero);  // the pseudo console
        ProcessInfo pi;
        if (!CreateProcessW(null, cmd, IntPtr.Zero, IntPtr.Zero, false,
                            0x80000, IntPtr.Zero, null, ref si, out pi)) {
            return -3;
        }
        foreach (string s in typed) {
            Thread.Sleep(300);
            byte[] b = Encoding.UTF8.GetBytes(s);
            int done;
            WriteFile(inw, b, b.Length, out done, IntPtr.Zero);
        }
        int code = -1;
        if (WaitForSingleObject(pi.process, 20000) == 0) {
            uint status;
            GetExitCodeProcess(pi.process, out status);
            code = (int)status;
        } else {
            TerminateProcess(pi.process, 1);
            WaitForSingleObject(pi.process, -1);
        }
        ClosePseudoConsole(pc);
        CloseHandle(outw);
        drain.Join(5000);
        return code;
    }

    // A file to inherit, created (replaced) for writing.
    static IntPtr Create(string path) {
        SecurityAttributes sa = new SecurityAttributes();
        sa.len = Marshal.SizeOf(sa);
        sa.inherit = true;
        return CreateFileW(path, 0x40000000, 7, ref sa, 2, 0x80, IntPtr.Zero);
    }

    // Run a command in directory dir in a new pseudo console, its standard
    // output and error to files, and once a file matching pattern is in
    // directory watch (as listed, so a delete-pending one too), and delay
    // milliseconds later, interrupt it as a user would: typing Ctrl+C
    // ('c') or Ctrl+Break ('b', as Windows Terminal sends that key), or
    // closing its console ('x').
    // Returns its status in hexadecimal, or "early" and its status if it
    // ended before the file was seen, "late" if it ran past 20 seconds,
    // or "none" without pseudo consoles.
    public static string Interrupt(string cmd, string dir, string output,
                                   string error, string watch,
                                   string pattern, int delay, char how) {
        IntPtr inr, inw, outw, pc;
        CreatePipe(out inr, out inw, IntPtr.Zero, 0);
        CreatePipe(out Pty.output, out outw, IntPtr.Zero, 0);
        Coord size = new Coord();
        size.x = 120;
        size.y = 30;
        try {
            if (CreatePseudoConsole(size, inr, outw, 0, out pc) != 0) {
                return "none";
            }
        } catch (EntryPointNotFoundException) {
            return "none";
        }
        Thread drain = new Thread(Drain);
        drain.Start();
        IntPtr len = IntPtr.Zero;
        InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref len);
        StartupInfoEx si = new StartupInfoEx();
        si.cb = Marshal.SizeOf(si);
        si.flags = 0x100;  // STARTF_USESTDHANDLES, input the console's
        si.stdout = Create(output);
        si.stderr = Create(error);
        si.attributes = Marshal.AllocHGlobal(len);
        InitializeProcThreadAttributeList(si.attributes, 1, 0, ref len);
        UpdateProcThreadAttribute(si.attributes, 0, (IntPtr)0x20016, pc,
                                  (IntPtr)IntPtr.Size, IntPtr.Zero,
                                  IntPtr.Zero);  // the pseudo console
        // Ctrl+C is ignored by a process started so, as by sshd, and by
        // its children, which inherit that: not by this one's
        SetConsoleCtrlHandler(IntPtr.Zero, false);
        ProcessInfo pi;
        if (!CreateProcessW(null, cmd, IntPtr.Zero, IntPtr.Zero, true,
                            0x80000, IntPtr.Zero, dir, ref si, out pi)) {
            return "failed";
        }
        CloseHandle(si.stdout);
        CloseHandle(si.stderr);
        bool seen = false;
        for (int i = 0; i < 4000 && !seen; i++) {
            if (WaitForSingleObject(pi.process, 5) == 0) {
                break;
            }
            seen = Directory.Exists(watch) &&  // (it may be made later)
                   Directory.GetFiles(watch, pattern).Length > 0;
        }
        if (seen) {
            Thread.Sleep(delay);
        }
        if (seen && how == 'x') {
            ClosePseudoConsole(pc);
            pc = IntPtr.Zero;
        } else if (seen) {
            // Ctrl+Break as a key event (win32-input-mode): VK_CANCEL,
            // pressed and released, with the left Ctrl key down
            string key = how == 'c' ? "\x03" :
                         "\x1b[3;70;0;1;8;1_\x1b[3;70;0;0;8;1_";
            byte[] b = Encoding.ASCII.GetBytes(key);
            int done;
            WriteFile(inw, b, b.Length, out done, IntPtr.Zero);
        }
        string r = "late";
        if (WaitForSingleObject(pi.process, 20000) == 0) {
            uint code;
            GetExitCodeProcess(pi.process, out code);
            r = (seen ? "" : "early ") + code.ToString("X");
        } else {
            TerminateProcess(pi.process, 1);
            WaitForSingleObject(pi.process, -1);
        }
        if (pc != IntPtr.Zero) {
            ClosePseudoConsole(pc);
        }
        CloseHandle(outw);
        drain.Join(5000);
        return r;
    }
}
