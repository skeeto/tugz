// A pseudo console for the Windows tests (zip_windows.sh, unzip_windows.sh):
// Pty.Run runs a command in one, typing into it as a user would.
using System;
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
    static extern bool GetExitCodeProcess(IntPtr h, out int code);
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
            GetExitCodeProcess(pi.process, out code);
        } else {
            TerminateProcess(pi.process, 1);
            WaitForSingleObject(pi.process, -1);
        }
        ClosePseudoConsole(pc);
        CloseHandle(outw);
        drain.Join(5000);
        return code;
    }
}
