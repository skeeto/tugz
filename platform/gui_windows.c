// tugz: a Windows GUI for zip archives, through tugz's zip and unzip
// $ cc -O2 -fno-builtin -nostartfiles -mwindows -o tugz.exe platform/gui_windows.c -lmemory -lcomctl32 -lole32
// For XP (i686), as there, and to address more memory, add:
//   -Wl,--large-address-aware
// Run from the source's root, which the icon's .incbin names.
//
// A folder tree beside a list of an archive's entries, zipinfo's fields
// for columns, with the work of zip and unzip themselves, through their
// callers' interfaces (zip_call, unzip_call), on a worker thread as a
// host in this process (os_call): listing, adding, extracting, moving,
// deleting, renaming, testing, and editing entries, and dragging them in
// and out. Windows XP and later, Unicode throughout, sharp at any DPI.
#include "../src/base.c"
#include "../src/crc32.c"
#include "../src/inflate.c"
#include "../src/deflate.c"
#include "../src/io.c"
#include "../src/zip.c"
#include "../src/wild.c"
#include "../src/dir.c"
#include "../src/zipin.c"
#include "../src/unzip.c"
#include "../src/zipinfo.c"

// Both programs, each with its own message for running out of memory,
// which the GUI's os_oom gives as the program running gives it
#define os_oom zip_os_oom
#include "../src/zipcli.c"
#undef os_oom
#define os_oom unzip_os_oom
#include "../src/unzipcli.c"
#undef os_oom

#include "windows.c"
#include "zipfs_windows.c"
#include "zipos_windows.c"
#include "unzipos_windows.c"


// Resources: the program's icon (platform/tugz.ico, painted by the same
// code as the toolbar's, below: BMPs to 64 for XP, then a 256 PNG), its
// group, and the manifest: version 6 of the common controls, per-monitor
// DPI awareness (as the GUI also asks at run time), and the Windows
// versions it was tested on. It asks for no activeCodePage, which would
// change how names in the archive's code pages are decoded (os_fromcp).
// The icon is read from the source's root (CMake passes -Wa,-I); its
// entries' offsets are as the file was laid out, and must follow it.
// GCC emits top-level asm first, or without -ftoplevel-reorder (-O0) in
// place, here after functions, in .text, to which it returns.
__asm__(
    "    .section .rsrc,\"dr\"\n"
    "    .macro dir n\n"                     // a directory of n IDs
    "    .long 0, 0\n"
    "    .short 0, 0, 0, \\n\n"
    "    .endm\n"
    "    .macro data label\n"                // a data entry, by its labels
    "    .rva \\label\n"
    "    .long \\label\\()end - \\label, 0, 0\n"
    "    .endm\n"
    "    .p2align 2\n"
    ".Lroot: dir 3\n"
    "    .long 3, 0x80000000 + (.Licons - .Lroot)\n"    // RT_ICON
    "    .long 14, 0x80000000 + (.Lgroups - .Lroot)\n"  // RT_GROUP_ICON
    "    .long 24, 0x80000000 + (.Lmans - .Lroot)\n"    // RT_MANIFEST
    ".Licons: dir 8\n"
    "    .irp i, 1, 2, 3, 4, 5, 6, 7, 8\n"
    "    .long \\i, 0x80000000 + (.Licon\\i - .Lroot)\n"
    "    .endr\n"
    "    .irp i, 1, 2, 3, 4, 5, 6, 7, 8\n"
    ".Licon\\i: dir 1\n"
    "    .long 0, .Licondata\\i - .Lroot\n"            // language neutral
    "    .endr\n"
    ".Lgroups: dir 1\n"
    "    .long 1, 0x80000000 + (.Lgroup - .Lroot)\n"
    ".Lgroup: dir 1\n"
    "    .long 0, .Lgroupdata - .Lroot\n"
    ".Lmans: dir 1\n"
    "    .long 1, 0x80000000 + (.Lmanid - .Lroot)\n"
    ".Lmanid: dir 1\n"
    "    .long 0x409, .Lmandata - .Lroot\n"
    "    .irp i, 1, 2, 3, 4, 5, 6, 7, 8\n"
    ".Licondata\\i: data .Lico\\i\n"
    "    .endr\n"
    ".Lgroupdata: data .Lgrp\n"
    ".Lmandata: data .Lman\n"

    // The group: each icon's entry in the file but for its offset, then
    // its ID; then the icons, at their offsets in the file, the PNG last
    ".Lgrp: .short 0, 1, 8\n"
    "    .irp i, 1, 2, 3, 4, 5, 6, 7, 8\n"
    "    .incbin \"platform/tugz.ico\", 16*\\i - 10, 12\n"
    "    .short \\i\n"
    "    .endr\n"
    ".Lgrpend:\n"
    "    .macro icon i, offset, len\n"
    "    .p2align 3\n"
    ".Lico\\i: .incbin \"platform/tugz.ico\", \\offset, \\len\n"
    ".Lico\\i\\()end:\n"
    "    .endm\n"
    "    icon 1, 134, 1128\n"                // 16x16
    "    icon 2, 1262, 1720\n"               // 20x20
    "    icon 3, 2982, 2440\n"               // 24x24
    "    icon 4, 5422, 4264\n"               // 32x32
    "    icon 5, 9686, 6760\n"               // 40x40
    "    icon 6, 16446, 9640\n"              // 48x48
    "    icon 7, 26086, 16936\n"             // 64x64
    "    .p2align 3\n"
    ".Lico8: .incbin \"platform/tugz.ico\", 43022\n"  // 256x256, PNG
    ".Lico8end:\n"

    "    .p2align 3\n"
    ".Lman: .ascii \"<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\\n"
    "<assembly xmlns='urn:schemas-microsoft-com:asm.v1' "
    "manifestVersion='1.0'>\\n"
    "<assemblyIdentity type='win32' name='tugz' version='1.1.0.0' "
    "processorArchitecture='*'/>\\n"
    "<dependency><dependentAssembly><assemblyIdentity type='win32' "
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "
    "language='*'/></dependentAssembly></dependency>\\n"
    "<trustInfo xmlns='urn:schemas-microsoft-com:asm.v3'><security>"
    "<requestedPrivileges><requestedExecutionLevel level='asInvoker' "
    "uiAccess='false'/></requestedPrivileges></security></trustInfo>\\n"
    "<compatibility xmlns='urn:schemas-microsoft-com:compatibility.v1'>"
    "<application>"
    "<supportedOS Id='{e2011457-1546-43c5-a5fe-008deee3d3f0}'/>"
    "<supportedOS Id='{35138b9a-5d96-4fbd-8e2d-a2440225f93a}'/>"
    "<supportedOS Id='{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}'/>"
    "<supportedOS Id='{1f676c76-80e1-4239-95bb-83d0f6d0da78}'/>"
    "<supportedOS Id='{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}'/>"
    "</application></compatibility>\\n"
    "<application xmlns='urn:schemas-microsoft-com:asm.v3'>"
    "<windowsSettings>"
    "<dpiAware xmlns='http://schemas.microsoft.com/SMI/2005/"
    "WindowsSettings'>true/pm</dpiAware>"
    "<dpiAwareness xmlns='http://schemas.microsoft.com/SMI/2016/"
    "WindowsSettings'>PerMonitorV2,PerMonitor</dpiAwareness>"
    "</windowsSettings></application>\\n"
    "</assembly>\\n\"\n"
    ".Lmanend:\n"
    "    .purgem dir\n"
    "    .purgem data\n"
    "    .purgem icon\n"
    "    .text\n"
);


// Win32: what the GUI uses beyond windows.c, declared as it declares it.
// Only what Windows XP has is imported; newer functions are looked up
// (load_guiapi), as are those of DLLs that are not KnownDLLs (uxtheme,
// shlwapi), by their full path, lest one beside the program be loaded.

typedef struct { i32 x, y; } point;
typedef struct { i32 left, top, right, bottom; } rect;

typedef iz (__stdcall *wndproc)(iptr, u32, uptr, iz);

typedef struct {
    u32        size;
    u32        style;
    wndproc    proc;
    i32        clsextra, wndextra;
    iptr       instance, icon, cursor, background;
    c16 const *menuname, *classname;
    iptr       iconsm;
} wndclassex;

typedef struct {
    iptr  hwnd;
    u32   message;
    uptr  wparam;
    iz    lparam;
    u32   time;
    point pt;
} winmsg;

typedef struct {
    iptr hdc;
    b32  erase;
    rect paint;
    b32  restore, incupdate;
    u8   reserved[32];
} paintstruct;

typedef struct {
    iptr from;
    uptr id;
    u32  code;
} nmhdr;

typedef struct {
    u32  mask;
    i32  item, subitem;
    u32  state, statemask;
    c16 *text;
    i32  textmax;
    i32  image;
    iz   lparam;
    i32  indent;
} lvitem;  // the fields of XP's, without Vista's groups

typedef struct {
    nmhdr  hdr;
    lvitem item;
} nmlvdispinfo;

typedef struct {
    nmhdr hdr;
    i32   item, subitem;
    u32   newstate, oldstate, changed;
    point action;
    iz    lparam;
    u32   keyflags;  // NMITEMACTIVATE's, not NMLISTVIEW's
} nmlistview;

typedef struct __attribute((packed)) {
    nmhdr hdr;
    u16   vkey;
    u32   flags;
} nmkeydown;  // NMLVKEYDOWN and NMTVKEYDOWN

typedef struct {
    u32  mask;
    i32  fmt;
    i32  cx;
    c16 *text;
    i32  textmax;
    i32  subitem;
    i32  image, order;
} lvcolumn;

typedef struct {
    nmhdr hdr;
    i32   from, to;
    u32   newstate, oldstate;
} nmlvodstatechange;

typedef struct {
    nmhdr hdr;
    u32   stage;
    iptr  hdc;
    rect  rc;
    uptr  itemspec;
    u32   itemstate;
    iz    itemlparam;
} nmcustomdraw;

typedef struct {
    nmcustomdraw nmcd;
    u32          clrtext, clrtextbk;
    i32          subitem;
} nmlvcustomdraw;  // its first fields

typedef struct {
    point pt;
    u32   flags;
    i32   item, subitem, group;
} lvhittestinfo;

typedef struct {
    u32  mask;
    iptr item;
    u32  state, statemask;
    c16 *text;
    i32  textmax;
    i32  image, selimage;
    i32  children;
    iz   lparam;
} tvitem;

typedef struct {
    tvitem base;
    i32    integral;
    u32    stateex;
    iptr   hwnd;
    i32    expandedimage;
    i32    reserved;
} tvitemex;

typedef struct {
    iptr     parent;
    iptr     after;
    tvitemex item;
} tvinsertstruct;

typedef struct {
    nmhdr  hdr;
    u32    action;
    tvitem old;
    tvitem new;
    point  drag;
} nmtreeview;

typedef struct {
    nmhdr  hdr;
    tvitem item;
} nmtvdispinfo;

typedef struct {
    point pt;
    u32   flags;
    iptr  item;
} tvhittestinfo;

typedef struct {
    i32  bitmap;
    i32  command;
    u8   state;
    u8   style;
    u8   reserved[sizeof(void *)-2];
    uptr data;
    iz   string;
} tbbutton;

typedef struct {
    nmhdr    hdr;
    i32      item;
    tbbutton button;
    i32      textlen;
    c16     *text;
    rect     rc;
} nmtoolbar;

typedef struct {
    nmhdr hdr;
    c16  *text;
    c16   sztext[80];
    iptr  instance;
    u32   flags;
    iz    lparam;
} nmttdispinfo;

typedef struct {
    i32 height, width, escapement, orientation, weight;
    u8  italic, underline, strikeout, charset;
    u8  outprecision, clipprecision, quality, pitchandfamily;
    c16 facename[32];
} logfont;

typedef struct {
    u32     size;
    i32     borderwidth, scrollwidth, scrollheight;
    i32     captionwidth, captionheight;
    logfont captionfont;
    i32     smcaptionwidth, smcaptionheight;
    logfont smcaptionfont;
    i32     menuwidth, menuheight;
    logfont menufont, statusfont, messagefont;
    i32     paddedborderwidth;  // Vista's: leave out for XP (size 500)
} nonclientmetrics;

typedef struct {
    i32 height, ascent, descent, internalleading, externalleading;
    i32 avecharwidth, maxcharwidth, weight, overhang;
    i32 digitizedaspectx, digitizedaspecty;
    c16 firstchar, lastchar, defaultchar, breakchar;
    u8  italic, underlined, struckout, pitchandfamily, charset;
} textmetric;

typedef struct {
    u32 size;
    i32 width, height;
    u16 planes, bitcount;
    u32 compression, sizeimage;
    i32 xpelspermeter, ypelspermeter;
    u32 clrused, clrimportant;
} bitmapinfoheader;

typedef struct {
    b32  icon;
    u32  xhotspot, yhotspot;
    iptr mask, color;
} iconinfo;

typedef struct {
    iptr icon;
    i32  iicon;
    u32  attributes;
    c16  displayname[260];
    c16  typename[80];
} shfileinfo;

typedef struct {
    u32        size;
    u32        mask;
    iptr       hwnd;
    c16 const *verb, *file, *params, *dir;
    i32        show;
    iptr       instapp;
    void      *idlist;
    c16 const *class;
    iptr       hkeyclass;
    u32        hotkey;
    iptr       icon;
    iptr       process;
} shellexecuteinfo;

typedef struct {
    u32        size;
    iptr       owner, instance;
    c16 const *filter;
    c16       *customfilter;
    u32        maxcustfilter, filterindex;
    c16       *file;
    u32        maxfile;
    c16       *filetitle;
    u32        maxfiletitle;
    c16 const *initialdir, *title;
    u32        flags;
    u16        fileoffset, fileextension;
    c16 const *defext;
    iz         custdata;
    void      *hook;
    c16 const *templatename;
    void      *reserved;
    u32        reserved2, flagsex;
} openfilename;

typedef struct {
    iptr       owner;
    void      *root;
    c16       *displayname;
    c16 const *title;
    u32        flags;
    void      *callback;
    iz         lparam;
    i32        image;
} browseinfo;

typedef struct {
    u32  cb;
    c16 *reserved, *desktop, *title;
    u32  x, y, xsize, ysize, xcount, ycount, fill, flags;
    u16  show, reserved2;
    u8  *reserved2p;
    iptr input, output, error;
} startupinfo;

typedef struct {
    iptr process, thread;
    u32  pid, tid;
} processinfo;

typedef struct {
    u32  size;
    rect monitor, work;
    u32  flags;
} monitorinfo;

typedef struct {
    u32   length, flags, showcmd;
    point min, max;
    rect  normal;
} windowplacement;

typedef struct {
    u32 mask;
    i32 cxy;
    c16 *text;
    iptr bm;
    i32 textmax;
    i32 fmt;
    iz  lparam;
    i32 image, order;
    u32 type;
    void *filter;
    u32 state;
} hditem;

typedef struct {
    point reserved, maxsize, maxposition, mintracksize, maxtracksize;
} minmaxinfo;

typedef struct {
    u16   format;
    void *ptd;
    u32   aspect;
    i32   index;
    u32   tymed;
} formatetc;

typedef struct {
    u32   tymed;
    iptr  hglobal;
    void *unkforrelease;
} stgmedium;

typedef struct {
    u32   files;
    point pt;
    b32   nc;
    b32   wide;
} dropfiles;

_Static_assert(sizeof(wndclassex) == (sizeof(void *)==8 ? 80 : 48), "");
_Static_assert(sizeof(winmsg) == (sizeof(void *)==8 ? 48 : 28), "");
_Static_assert(sizeof(paintstruct) == (sizeof(void *)==8 ? 72 : 64), "");
_Static_assert(sizeof(nmhdr) == (sizeof(void *)==8 ? 24 : 12), "");
_Static_assert(sizeof(nmlistview) == (sizeof(void *)==8 ? 72 : 48), "");
_Static_assert(sizeof(nmkeydown) == (sizeof(void *)==8 ? 30 : 18), "");
_Static_assert(sizeof(nmlvodstatechange) == (sizeof(void *)==8 ? 40 : 28),
               "");
_Static_assert(sizeof(nmcustomdraw) == (sizeof(void *)==8 ? 80 : 48), "");
_Static_assert(sizeof(lvhittestinfo) == 24, "");
_Static_assert(sizeof(tvitem) == (sizeof(void *)==8 ? 56 : 40), "");
_Static_assert(sizeof(tvinsertstruct) == (sizeof(void *)==8 ? 96 : 68), "");
_Static_assert(sizeof(nmtreeview) == (sizeof(void *)==8 ? 152 : 104), "");
_Static_assert(sizeof(tvhittestinfo) == (sizeof(void *)==8 ? 24 : 16), "");
_Static_assert(sizeof(tbbutton) == (sizeof(void *)==8 ? 32 : 20), "");
_Static_assert(sizeof(nmtoolbar) == (sizeof(void *)==8 ? 96 : 60), "");
_Static_assert(sizeof(nmttdispinfo) == (sizeof(void *)==8 ? 216 : 188), "");
_Static_assert(sizeof(logfont) == 92, "");
_Static_assert(sizeof(nonclientmetrics) == 504, "");
_Static_assert(sizeof(textmetric) == 60, "");
_Static_assert(sizeof(iconinfo) == (sizeof(void *)==8 ? 32 : 20), "");
_Static_assert(sizeof(shfileinfo) == (sizeof(void *)==8 ? 696 : 692), "");
_Static_assert(sizeof(shellexecuteinfo) == (sizeof(void *)==8 ? 112 : 60),
               "");
_Static_assert(sizeof(openfilename) == (sizeof(void *)==8 ? 152 : 88), "");
_Static_assert(sizeof(browseinfo) == (sizeof(void *)==8 ? 64 : 32), "");
_Static_assert(sizeof(startupinfo) == (sizeof(void *)==8 ? 104 : 68), "");
_Static_assert(sizeof(processinfo) == (sizeof(void *)==8 ? 24 : 16), "");
_Static_assert(sizeof(hditem) == (sizeof(void *)==8 ? 72 : 48), "");
_Static_assert(sizeof(formatetc) == (sizeof(void *)==8 ? 32 : 20), "");
_Static_assert(sizeof(stgmedium) == (sizeof(void *)==8 ? 24 : 12), "");
_Static_assert(sizeof(dropfiles) == 20, "");

// kernel32
W32(iptr)   CreateEventW(uptr, b32, b32, c16 const *);
W32(b32)    CreateProcessW(c16 const *, c16 *, uptr, uptr, b32, u32, uptr,
                           c16 const *, startupinfo *, processinfo *);
W32(iptr)   CreateThread(uptr, iz, u32 (__stdcall *)(void *), void *, u32,
                         u32 *);
W32(u32)    GetCurrentProcessId(void);
W32(b32)    GetExitCodeProcess(iptr, u32 *);
W32(u32)    GetModuleFileNameW(iptr, c16 *, u32);
W32(u32)    GetSystemDirectoryW(c16 *, u32);
W32(u32)    GetTempPathW(u32, c16 *);
W32(u32)    GetVersion(void);
W32(iptr)   GlobalAlloc(u32, iz);
W32(iptr)   GlobalFree(iptr);
W32(void *) GlobalLock(iptr);
W32(iz)     GlobalSize(iptr);
W32(b32)    GlobalUnlock(iptr);
W32(iptr)   LoadLibraryW(c16 const *);
W32(i32)    MulDiv(i32, i32, i32);
W32(b32)    RemoveDirectoryW(c16 const *);
W32(b32)    SetEvent(iptr);
W32(u32)    SetErrorMode(u32);
W32(b32)    SetFileAttributesW(c16 const *, u32);
W32(i32)    lstrcmpiW(c16 const *, c16 const *);
W32(b32)    VirtualFree(void *, iz, u32);
W32(u32)    WaitForSingleObject(iptr, u32);
W32(i32)    GetDateFormatW(u32, u32, systemtime const *, c16 const *, c16 *,
                           i32);
W32(i32)    GetTimeFormatW(u32, u32, systemtime const *, c16 const *, c16 *,
                           i32);
W32(i32)    GetLocaleInfoW(u32, u32, c16 *, i32);
W32(b32)    GetFileAttributesExW(c16 const *, i32, void *);
W32(iptr)   CreateFileMappingW(iptr, uptr, u32, u32, u32, c16 const *);

// user32
W32(b32)    AdjustWindowRectEx(rect *, u32, b32, u32);
W32(b32)    AppendMenuW(iptr, u32, uptr, c16 const *);
W32(iz)     CallWindowProcW(wndproc, iptr, u32, uptr, iz);
W32(b32)    CheckMenuItem(iptr, u32, u32);
W32(b32)    ClientToScreen(iptr, point *);
W32(iptr)   CreateAcceleratorTableW(void *, i32);
W32(iptr)   CreateDialogIndirectParamW(iptr, void *, iptr, void *, iz);
W32(iptr)   CreateIconIndirect(iconinfo *);
W32(iptr)   CreateMenu(void);
W32(iptr)   CreatePopupMenu(void);
W32(iptr)   CreateWindowExW(u32, c16 const *, c16 const *, u32, i32, i32,
                            i32, i32, iptr, iptr, iptr, void *);
W32(iz)     DefWindowProcW(iptr, u32, uptr, iz);
W32(b32)    DestroyIcon(iptr);
W32(b32)    DestroyMenu(iptr);
W32(b32)    DestroyWindow(iptr);
W32(iz)     DialogBoxIndirectParamW(iptr, void *, iptr, void *, iz);
W32(iz)     DispatchMessageW(winmsg *);
W32(b32)    DrawIconEx(iptr, i32, i32, iptr, i32, i32, u32, iptr, u32);
W32(i32)    DrawTextW(iptr, c16 const *, i32, rect *, u32);
W32(b32)    EnableMenuItem(iptr, u32, u32);
W32(b32)    EnableWindow(iptr, b32);
W32(b32)    EndDialog(iptr, iz);
W32(b32)    EndPaint(iptr, paintstruct *);
W32(iptr)   BeginPaint(iptr, paintstruct *);
W32(i32)    FillRect(iptr, rect *, iptr);
W32(b32)    GetClientRect(iptr, rect *);
W32(b32)    GetCursorPos(point *);
W32(iptr)   GetDC(iptr);
W32(iptr)   GetDlgItem(iptr, i32);
W32(u32)    GetDlgItemTextW(iptr, i32, c16 *, i32);
W32(iptr)   GetFocus(void);
W32(short)  GetKeyState(i32);
W32(b32)    GetMessageW(winmsg *, iptr, u32, u32);
W32(b32)    GetMonitorInfoW(iptr, monitorinfo *);
W32(iptr)   GetParent(iptr);
W32(u32)    GetSysColor(i32);
W32(iptr)   GetSysColorBrush(i32);
W32(i32)    GetSystemMetrics(i32);
W32(b32)    GetWindowPlacement(iptr, windowplacement *);
W32(b32)    GetWindowRect(iptr, rect *);
W32(i32)    GetWindowTextLengthW(iptr);
W32(i32)    GetWindowTextW(iptr, c16 *, i32);
W32(b32)    InvalidateRect(iptr, rect *, b32);
W32(b32)    IsDialogMessageW(iptr, winmsg *);
W32(b32)    IsWindowEnabled(iptr);
W32(b32)    IsWindowVisible(iptr);
W32(b32)    IsChild(iptr, iptr);
W32(b32)    KillTimer(iptr, uptr);
W32(iptr)   LoadCursorW(iptr, uptr);
W32(i32)    MessageBoxW(iptr, c16 const *, c16 const *, u32);
W32(iptr)   MonitorFromRect(rect const *, u32);
W32(iptr)   MonitorFromWindow(iptr, u32);
W32(b32)    MoveWindow(iptr, i32, i32, i32, i32, b32);
W32(u32)    MsgWaitForMultipleObjects(u32, iptr *, b32, u32, u32);
W32(b32)    PostMessageW(iptr, u32, uptr, iz);
W32(void)   PostQuitMessage(i32);
W32(u16)    RegisterClassExW(wndclassex *);
W32(u32)    RegisterClipboardFormatW(c16 const *);
W32(b32)    ReleaseCapture(void);
W32(i32)    ReleaseDC(iptr, iptr);
W32(b32)    ScreenToClient(iptr, point *);
W32(iz)     SendMessageW(iptr, u32, uptr, iz);
W32(iptr)   SetCapture(iptr);
W32(iptr)   SetCursor(iptr);
W32(b32)    SetDlgItemTextW(iptr, i32, c16 const *);
W32(iptr)   SetFocus(iptr);
W32(b32)    SetForegroundWindow(iptr);
W32(b32)    SetMenu(iptr, iptr);
W32(uptr)   SetTimer(iptr, uptr, u32, uptr);
W32(b32)    SetWindowPlacement(iptr, windowplacement *);
W32(b32)    SetWindowPos(iptr, iptr, i32, i32, i32, i32, u32);
W32(b32)    SetWindowTextW(iptr, c16 const *);
W32(b32)    ShowWindow(iptr, i32);
W32(b32)    SystemParametersInfoW(u32, u32, void *, u32);
W32(b32)    TrackPopupMenu(iptr, u32, i32, i32, i32, iptr, rect *);
W32(i32)    TranslateAcceleratorW(iptr, iptr, winmsg *);
W32(b32)    TranslateMessage(winmsg *);
W32(b32)    UpdateWindow(iptr);
#if __SIZEOF_POINTER__ == 8
W32(iz)     GetWindowLongPtrW(iptr, i32);
W32(iz)     SetWindowLongPtrW(iptr, i32, iz);
#else
W32(iz)     GetWindowLongW(iptr, i32);
W32(iz)     SetWindowLongW(iptr, i32, iz);
#  define GetWindowLongPtrW GetWindowLongW
#  define SetWindowLongPtrW SetWindowLongW
#endif

// gdi32
W32(iptr)   CreateCompatibleDC(iptr);
W32(iptr)   CreateDIBSection(iptr, void *, u32, void **, iptr, u32);
W32(iptr)   CreateBitmap(i32, i32, u32, u32, void const *);
W32(iptr)   CreateFontIndirectW(logfont *);
W32(iptr)   CreateSolidBrush(u32);
W32(b32)    DeleteDC(iptr);
W32(b32)    DeleteObject(iptr);
W32(i32)    GetDeviceCaps(iptr, i32);
W32(b32)    GetTextMetricsW(iptr, textmetric *);
W32(iptr)   SelectObject(iptr, iptr);
W32(i32)    SetBkMode(iptr, i32);
W32(u32)    SetTextColor(iptr, u32);

// comctl32
W32(b32)    InitCommonControlsEx(void *);
W32(i32)    ImageList_Add(iptr, iptr, iptr);
W32(iptr)   ImageList_Create(i32, i32, u32, i32, i32);
W32(b32)    ImageList_Destroy(iptr);
W32(i32)    ImageList_ReplaceIcon(iptr, i32, iptr);
W32(b32)    ImageList_SetOverlayImage(iptr, i32, i32);

// shell32
W32(u32)    DragQueryFileW(iptr, u32, c16 *, u32);
W32(b32)    ShellExecuteExW(shellexecuteinfo *);
W32(void *) SHBrowseForFolderW(browseinfo *);
W32(uptr)   SHGetFileInfoW(c16 const *, u32, shfileinfo *, u32, u32);
W32(b32)    SHGetPathFromIDListW(void *, c16 *);

// ole32
W32(i32)    CoCreateInstance(void const *, void *, u32, void const *,
                             void **);
W32(void)   CoTaskMemFree(void *);
W32(i32)    DoDragDrop(void *, void *, u32, u32 *);
W32(i32)    OleInitialize(void *);
W32(void)   OleUninitialize(void);
W32(i32)    RegisterDragDrop(iptr, void *);
W32(void)   ReleaseStgMedium(stgmedium *);
W32(i32)    RevokeDragDrop(iptr);

// comdlg32
W32(b32)    GetOpenFileNameW(openfilename *);
W32(b32)    GetSaveFileNameW(openfilename *);

// advapi32
W32(i32)    RegCloseKey(iptr);
W32(i32)    RegCreateKeyExW(iptr, c16 const *, u32, c16 *, u32, u32, uptr,
                            iptr *, u32 *);
W32(i32)    RegQueryValueExW(iptr, c16 const *, u32 *, u32 *, void *, u32 *);
W32(i32)    RegSetValueExW(iptr, c16 const *, u32, u32, void const *, u32);

#define CW_USEDEFAULT       ((i32)0x80000000)
#define WS_OVERLAPPEDWINDOW 0x00cf0000u
#define WS_CHILD            0x40000000u
#define WS_VISIBLE          0x10000000u
#define WS_CLIPCHILDREN     0x02000000u
#define WS_CLIPSIBLINGS     0x04000000u
#define WS_BORDER           0x00800000u
#define WS_TABSTOP          0x00010000u
#define WS_GROUP            0x00020000u
#define WS_VSCROLL          0x00200000u
#define WS_HSCROLL          0x00100000u
#define WS_POPUP            0x80000000u
#define WS_CAPTION          0x00c00000u
#define WS_SYSMENU          0x00080000u
#define WS_THICKFRAME       0x00040000u
#define WS_EX_CLIENTEDGE    0x00000200u
#define WS_EX_DLGMODALFRAME 0x00000001u
#define WS_EX_ACCEPTFILES   0x00000010u
#define WS_EX_CONTROLPARENT 0x00010000u

#define SW_SHOW             5
#define SW_SHOWNORMAL       1
#define SW_HIDE             0
#define SWP_NOZORDER        0x0004u
#define SWP_NOACTIVATE      0x0010u
#define SWP_NOMOVE          0x0002u
#define SWP_NOSIZE          0x0001u

#define WM_CREATE           0x0001
#define WM_DESTROY          0x0002
#define WM_SIZE             0x0005
#define WM_ACTIVATE         0x0006
#define WM_SETFOCUS         0x0007
#define WM_PAINT            0x000f
#define WM_CLOSE            0x0010
#define WM_ERASEBKGND       0x0014
#define WM_SETTINGCHANGE    0x001a
#define WM_ACTIVATEAPP      0x001c
#define WM_SETCURSOR        0x0020
#define WM_GETMINMAXINFO    0x0024
#define WM_SETFONT          0x0030
#define WM_NOTIFY           0x004e
#define WM_CONTEXTMENU      0x007b
#define WM_SETICON          0x0080
#define WM_KEYDOWN          0x0100
#define WM_CHAR             0x0102
#define WM_SYSKEYDOWN       0x0104
#define WM_INITDIALOG       0x0110
#define WM_COMMAND          0x0111
#define WM_TIMER            0x0113
#define WM_INITMENUPOPUP    0x0117
#define WM_CTLCOLORSTATIC   0x0138
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_CAPTURECHANGED   0x0215
#define WM_DPICHANGED       0x02e0
#define WM_APP              0x8000

#define BS_PUSHBUTTON       0x00000000u
#define BS_DEFPUSHBUTTON    0x00000001u
#define BS_AUTOCHECKBOX     0x00000003u
#define BS_AUTORADIOBUTTON  0x00000009u
#define BS_GROUPBOX         0x00000007u
#define BM_GETCHECK         0x00f0
#define BM_SETCHECK         0x00f1
#define BST_CHECKED         1
#define ES_AUTOHSCROLL      0x0080u
#define ES_MULTILINE        0x0004u
#define ES_READONLY         0x0800u
#define ES_AUTOVSCROLL      0x0040u
#define EM_SETSEL           0x00b1
#define EM_LIMITTEXT        0x00c5
#define SS_LEFT             0x00000000u
#define SS_ICON             0x00000003u
#define SS_NOPREFIX         0x00000080u
#define SS_PATHELLIPSIS     0x00008000u
#define SS_ENDELLIPSIS      0x00004000u
#define CBS_DROPDOWNLIST    0x0003u
#define CB_ADDSTRING        0x0143
#define CB_GETCURSEL        0x0147
#define CB_SETCURSEL        0x014e
#define STM_SETICON         0x0170
#define DS_SETFONT          0x40u
#define DS_MODALFRAME       0x80u
#define DS_FIXEDSYS         0x0008u
#define DS_CENTER           0x0800u
#define DS_SHELLFONT        (DS_SETFONT|DS_FIXEDSYS)
#define IDOK                1
#define IDCANCEL            2

#define MF_STRING           0x0000u
#define MF_GRAYED           0x0001u
#define MF_POPUP            0x0010u
#define MF_SEPARATOR        0x0800u
#define MF_CHECKED          0x0008u
#define MF_BYCOMMAND        0x0000u
#define MF_ENABLED          0x0000u
#define TPM_RIGHTBUTTON     0x0002u
#define TPM_RETURNCMD       0x0100u
#define TPM_NONOTIFY        0x0080u

#define MB_OK               0x00000000u
#define MB_OKCANCEL         0x00000001u
#define MB_YESNO            0x00000004u
#define MB_YESNOCANCEL      0x00000003u
#define MB_ICONERROR        0x00000010u
#define MB_ICONWARNING      0x00000030u
#define MB_ICONINFORMATION  0x00000040u
#define MB_ICONQUESTION     0x00000020u
#define MB_DEFBUTTON2       0x00000100u
#define IDYES               6
#define IDNO                7

#define VK_BACK             0x08
#define VK_RETURN           0x0d
#define VK_SHIFT            0x10
#define VK_CONTROL          0x11
#define VK_MENU             0x12
#define VK_ESCAPE           0x1b
#define VK_UP               0x26
#define VK_INSERT           0x2d
#define VK_DELETE           0x2e
#define VK_F2               0x71
#define VK_F4               0x73
#define VK_F5               0x74
#define FVIRTKEY            0x01
#define FSHIFT              0x04
#define FCONTROL            0x08
#define FALT                0x10

#define IDC_ARROW           32512
#define IDC_SIZEWE          32644
#define IDC_WAIT            32514
#define IDC_APPSTARTING     32650
#define COLOR_WINDOW        5
#define COLOR_BTNFACE       15
#define COLOR_GRAYTEXT      17
#define COLOR_INFOBK        24
#define COLOR_INFOTEXT      23
#define COLOR_WINDOWTEXT    8
#define SM_CXSMICON         49
#define SM_CYSMICON         50
#define SM_CXICON           11
#define SM_CYICON           12
#define SPI_GETNONCLIENTMETRICS 0x0029
#define LOGPIXELSX          88
#define MONITOR_DEFAULTTONULL    0
#define MONITOR_DEFAULTTONEAREST 2
#define SW_SHOWMAXIMIZED         3
#define DT_CENTER           0x0001u
#define DT_VCENTER          0x0004u
#define DT_SINGLELINE       0x0020u
#define DT_NOPREFIX         0x0800u
#define DT_WORDBREAK        0x0010u
#define DT_END_ELLIPSIS     0x8000u
#define DI_NORMAL           0x0003u
#define TRANSPARENT         1
#define GWLP_USERDATA       (-21)
#define GWLP_WNDPROC        (-4)
#define ICON_SMALL          0
#define ICON_BIG            1
#define BM_SETIMAGE         0x00f7
#define IMAGE_ICON          1
#define BS_ICON             0x00000040u
#define WAIT_OBJECT_0       0
#define WAIT_TIMEOUT        258
#define INFINITE            0xffffffffu
#define QS_ALLINPUT         0x04ffu
#define SEM_FAILCRITICALERRORS 0x0001u
#define SEM_NOOPENFILEERRORBOX 0x8000u
#define FILE_ATTRIBUTE_TEMPORARY 0x100u
#define MEM_RELEASE         0x8000u
#define MEM_DECOMMIT        0x4000u
#define GMEM_MOVEABLE       0x0002u
#define GMEM_ZEROINIT       0x0040u
#define LOCALE_USER_DEFAULT 0x0400u
#define DATE_SHORTDATE      0x00000001u
#define TIME_NOSECONDS      0x00000002u
#define LOCALE_STHOUSAND    0x0000000fu
#define HKEY_CURRENT_USER   ((iptr)(iz)(i32)0x80000001)
#define KEY_ALL_ACCESS      0x000f003fu
#define REG_SZ              1u
#define REG_BINARY          3u
#define REG_DWORD           4u

// Common controls
#define ICC_WIN95_CLASSES   0x000000ffu
#define ICC_PROGRESS_CLASS  0x00000020u
#define ICC_BAR_CLASSES     0x00000004u
#define ICC_LISTVIEW_CLASSES 0x00000001u
#define ICC_TREEVIEW_CLASSES 0x00000002u
#define ILC_COLOR32         0x0020u
#define ILC_MASK            0x0001u
#define CCS_NODIVIDER       0x0040u
#define CCS_NORESIZE        0x0004u
#define CCS_NOPARENTALIGN   0x0008u
#define CCS_TOP             0x0001u
#define NM_FIRST            0u
#define NM_CLICK            (NM_FIRST-2)
#define NM_DBLCLK           (NM_FIRST-3)
#define NM_RETURN           (NM_FIRST-4)
#define NM_RCLICK           (NM_FIRST-5)
#define NM_SETFOCUS         (NM_FIRST-7)
#define NM_CUSTOMDRAW       (NM_FIRST-12)
#define CDDS_PREPAINT       0x00000001u
#define CDDS_POSTPAINT      0x00000002u
#define CDRF_DODEFAULT      0x00000000u
#define CDRF_NOTIFYPOSTPAINT 0x00000010u

#define LVS_REPORT          0x0001u
#define LVS_SHOWSELALWAYS   0x0008u
#define LVS_EDITLABELS      0x0200u
#define LVS_OWNERDATA       0x1000u
#define LVS_SHAREIMAGELISTS 0x0040u
#define LVS_EX_FULLROWSELECT 0x00000020u
#define LVS_EX_HEADERDRAGDROP 0x00000010u
#define LVS_EX_LABELTIP     0x00004000u
#define LVS_EX_DOUBLEBUFFER 0x00010000u
#define LVSIL_SMALL         1
#define LVCF_FMT            0x0001u
#define LVCF_WIDTH          0x0002u
#define LVCF_TEXT           0x0004u
#define LVCF_SUBITEM        0x0008u
#define LVCFMT_LEFT         0x0000
#define LVCFMT_RIGHT        0x0001
#define LVIF_TEXT           0x0001u
#define LVIF_IMAGE          0x0002u
#define LVIF_PARAM          0x0004u
#define LVIF_STATE          0x0008u
#define LVIS_FOCUSED        0x0001u
#define LVIS_SELECTED       0x0002u
#define LVIS_DROPHILITED    0x0008u
#define LVIS_OVERLAYMASK    0x0f00u
#define LVNI_SELECTED       0x0002u
#define LVNI_FOCUSED        0x0001u
#define LVHT_ONITEM         0x000eu
#define LVM_FIRST           0x1000
#define LVM_SETIMAGELIST    (LVM_FIRST+3)
#define LVM_GETITEMCOUNT    (LVM_FIRST+4)
#define LVM_GETNEXTITEM     (LVM_FIRST+12)
#define LVM_HITTEST         (LVM_FIRST+18)
#define LVM_ENSUREVISIBLE   (LVM_FIRST+19)
#define LVM_REDRAWITEMS     (LVM_FIRST+21)
#define LVM_GETCOLUMNWIDTH  (LVM_FIRST+29)
#define LVM_SETCOLUMNWIDTH  (LVM_FIRST+30)
#define LVM_GETHEADER       (LVM_FIRST+31)
#define LVM_SETITEMSTATE    (LVM_FIRST+43)
#define LVM_GETSELECTEDCOUNT (LVM_FIRST+50)
#define LVM_SETITEMCOUNT    (LVM_FIRST+47)
#define LVM_SETCALLBACKMASK (LVM_FIRST+11)
#define LVM_SETEXTENDEDLISTVIEWSTYLE (LVM_FIRST+54)
#define LVM_INSERTCOLUMNW   (LVM_FIRST+97)
#define LVM_EDITLABELW      (LVM_FIRST+118)
#define LVM_GETITEMRECT     (LVM_FIRST+14)
#define LVIR_LABEL          2
#define LVM_GETEDITCONTROL  (LVM_FIRST+24)
#define LVM_GETCOLUMNORDERARRAY (LVM_FIRST+59)
#define LVM_SETCOLUMNORDERARRAY (LVM_FIRST+58)
#define LVN_FIRST           (0u-100u)
#define LVN_ITEMCHANGED     (LVN_FIRST-1)
#define LVN_COLUMNCLICK     (LVN_FIRST-8)
#define LVN_BEGINDRAG       (LVN_FIRST-9)
#define LVN_ODSTATECHANGED  (LVN_FIRST-15)
#define LVN_ITEMACTIVATE    (LVN_FIRST-14)
#define LVN_KEYDOWN         (LVN_FIRST-55)
#define LVN_GETDISPINFOW    (LVN_FIRST-77)
#define LVN_BEGINLABELEDITW (LVN_FIRST-75)
#define LVN_ENDLABELEDITW   (LVN_FIRST-76)
#define LVN_ODFINDITEMW     (LVN_FIRST-79)
#define HDM_FIRST           0x1200
#define HDM_GETITEMW        (HDM_FIRST+11)
#define HDM_SETITEMW        (HDM_FIRST+12)
#define HDI_FORMAT          0x0004u
#define HDF_SORTUP          0x0400
#define HDF_SORTDOWN        0x0200

#define TVS_HASBUTTONS      0x0001u
#define TVS_HASLINES        0x0002u
#define TVS_LINESATROOT     0x0004u
#define TVS_EDITLABELS      0x0008u
#define TVS_SHOWSELALWAYS   0x0020u
#define TVS_TRACKSELECT     0x0200u
#define TVS_FULLROWSELECT   0x1000u
#define TVS_EX_DOUBLEBUFFER 0x0004u
#define TVIF_TEXT           0x0001u
#define TVIF_IMAGE          0x0002u
#define TVIF_PARAM          0x0004u
#define TVIF_STATE          0x0008u
#define TVIF_SELECTEDIMAGE  0x0020u
#define TVIF_CHILDREN       0x0040u
#define TVIS_SELECTED       0x0002u
#define TVIS_DROPHILITED    0x0008u
#define TVIS_EXPANDED       0x0020u
#define TVM_GETITEMSTATE    (TV_FIRST+39)
#define TVI_ROOT            ((iptr)(iz)-0x10000)
#define TVI_LAST            ((iptr)(iz)-0x0fffe)
#define TVGN_ROOT           0x0000u
#define TVGN_NEXT           0x0001u
#define TVGN_CHILD          0x0004u
#define TVGN_CARET          0x0009u
#define TVGN_DROPHILITE     0x0008u
#define TVE_EXPAND          0x0002u
#define TVE_COLLAPSE        0x0001u
#define TVSIL_NORMAL        0
#define I_CHILDRENCALLBACK  (-1)
#define TV_FIRST            0x1100
#define TVM_INSERTITEMW     (TV_FIRST+50)
#define TVM_DELETEITEM      (TV_FIRST+1)
#define TVM_EXPAND          (TV_FIRST+2)
#define TVM_SETIMAGELIST    (TV_FIRST+9)
#define TVM_GETITEMRECT     (TV_FIRST+4)
#define TVM_GETNEXTITEM     (TV_FIRST+10)
#define TVM_SELECTITEM      (TV_FIRST+11)
#define TVM_HITTEST         (TV_FIRST+17)
#define TVM_ENSUREVISIBLE   (TV_FIRST+20)
#define TVM_EDITLABELW      (TV_FIRST+65)
#define TVM_GETITEMW        (TV_FIRST+62)
#define TVM_SETITEMW        (TV_FIRST+63)
#define TVM_SETITEMHEIGHT   (TV_FIRST+27)
#define TVM_SETEXTENDEDSTYLE (TV_FIRST+44)
#define TVN_FIRST           (0u-400u)
#define TVN_SELCHANGEDW     (TVN_FIRST-51)
#define TVN_ITEMEXPANDINGW  (TVN_FIRST-54)
#define TVN_BEGINDRAGW      (TVN_FIRST-56)
#define TVN_BEGINLABELEDITW (TVN_FIRST-59)
#define TVN_ENDLABELEDITW   (TVN_FIRST-60)
#define TVN_KEYDOWN         (TVN_FIRST-12)
#define TVHT_ONITEM         0x0046u

#define TBSTYLE_TOOLTIPS    0x0100u
#define TBSTYLE_FLAT        0x0800u
#define TBSTYLE_LIST        0x1000u
#define TBSTYLE_EX_DRAWDDARROWS 0x00000001u
#define TBSTYLE_EX_MIXEDBUTTONS 0x00000008u
#define TBSTYLE_EX_DOUBLEBUFFER 0x00000080u
#define BTNS_BUTTON         0x00u
#define BTNS_SEP            0x01u
#define BTNS_DROPDOWN       0x08u
#define BTNS_AUTOSIZE       0x10u
#define BTNS_SHOWTEXT       0x40u
#define TBSTATE_ENABLED     0x04u
#define WM_USER             0x0400
#define TB_ENABLEBUTTON     (WM_USER+1)
#define TB_BUTTONSTRUCTSIZE (WM_USER+30)
#define TB_ADDBUTTONSW      (WM_USER+68)
#define TB_SETIMAGELIST     (WM_USER+48)
#define TB_SETDISABLEDIMAGELIST (WM_USER+54)
#define TB_AUTOSIZE         (WM_USER+33)
#define TB_SETEXTENDEDSTYLE (WM_USER+84)
#define TB_GETRECT          (WM_USER+51)
#define TB_SETMAXTEXTROWS   (WM_USER+60)
#define TB_GETBUTTONSIZE    (WM_USER+58)
#define TB_DELETEBUTTON     (WM_USER+22)
#define TB_BUTTONCOUNT      (WM_USER+24)
#define TBN_FIRST           (0u-700u)
#define TBN_DROPDOWN        (TBN_FIRST-10)
#define TBDDRET_DEFAULT     0
#define TTN_GETDISPINFOW    (0u-530u)
#define SB_SETPARTS         (WM_USER+4)
#define SB_SETTEXTW         (WM_USER+11)
#define SB_GETRECT          (WM_USER+10)
#define SBARS_SIZEGRIP      0x0100u
#define PBM_SETRANGE32      (WM_USER+6)
#define PBM_SETPOS          (WM_USER+2)
#define PBM_SETMARQUEE      (WM_USER+10)
#define PBS_SMOOTH          0x01u
#define PBS_MARQUEE         0x08u

// Shell
#define SHGFI_ICON              0x000000100u
#define SHGFI_SYSICONINDEX      0x000004000u
#define SHGFI_LARGEICON         0x000000000u
#define SHGFI_SMALLICON         0x000000001u
#define SHGFI_OPENICON          0x000000002u
#define SHGFI_USEFILEATTRIBUTES 0x000000010u
#define SEE_MASK_NOCLOSEPROCESS 0x00000040u
#define SEE_MASK_FLAG_DDEWAIT   0x00000100u
#define SEE_MASK_NOASYNC        0x00000100u
#define SEE_MASK_INVOKEIDLIST   0x0000000cu
#define BIF_RETURNONLYFSDIRS    0x0001u
#define BIF_NEWDIALOGSTYLE      0x0040u
#define BIF_EDITBOX             0x0010u
#define OFN_HIDEREADONLY        0x00000004u
#define OFN_NOCHANGEDIR         0x00000008u
#define OFN_ALLOWMULTISELECT    0x00000200u
#define OFN_PATHMUSTEXIST       0x00000800u
#define OFN_FILEMUSTEXIST       0x00001000u
#define OFN_OVERWRITEPROMPT     0x00000002u
#define OFN_EXPLORER            0x00080000u
#define OFN_NODEREFERENCELINKS  0x00100000u
#define OFN_DONTADDTORECENT     0x02000000u

// OLE
#define S_OK                0
#define S_FALSE             1
#define E_NOINTERFACE       ((i32)0x80004002u)
#define E_NOTIMPL           ((i32)0x80004001u)
#define E_UNEXPECTED        ((i32)0x8000ffffu)
#define E_OUTOFMEMORY       ((i32)0x8007000eu)
#define DV_E_FORMATETC      ((i32)0x80040064u)
#define DV_E_TYMED          ((i32)0x80040069u)
#define OLE_E_ADVISENOTSUPPORTED ((i32)0x80040003u)
#define DRAGDROP_S_DROP     0x00040100
#define DRAGDROP_S_CANCEL   0x00040101
#define DRAGDROP_S_USEDEFAULTCURSORS 0x00040102
#define DROPEFFECT_NONE     0u
#define DROPEFFECT_COPY     1u
#define DROPEFFECT_MOVE     2u
#define MK_LBUTTON          0x0001u
#define MK_RBUTTON          0x0002u
#define CF_HDROP            15
#define TYMED_HGLOBAL       1u
#define DVASPECT_CONTENT    1u
#define CLSCTX_INPROC_SERVER 1u
#define DATADIR_GET         1


// Newer functions, looked up at run time, each null where it is missing:
// Vista's DPI awareness, Windows 8.1's and 10's per-monitor DPI, and
// what DLGs that are not KnownDLLs hold, loaded from the system's own
// directory
typedef struct {
    b32  (__stdcall *setdpiaware)(void);                       // Vista
    i32  (__stdcall *setdpiawareness)(i32);                     // 8.1
    b32  (__stdcall *setdpicontext)(iz);                        // 10 1703
    u32  (__stdcall *dpiforwindow)(iptr);                       // 10 1607
    b32  (__stdcall *spiwithdpi)(u32, u32, void *, u32, u32);   // 10 1607
    i32  (__stdcall *metricsfordpi)(i32, u32);                  // 10 1607
    b32  (__stdcall *adjustfordpi)(rect *, u32, b32, u32, u32); // 10 1607
    i32  (__stdcall *setwindowtheme)(iptr, c16 const *, c16 const *);
    i32  (__stdcall *strcmplogical)(c16 const *, c16 const *);
    b32  (__stdcall *setdlldirectory)(c16 const *);             // XP SP1
    b32  (__stdcall *setdefaultdlldirs)(u32);                   // 8
} guiapi;

// A DLL from the system's directory, by its full path.
static iptr system_dll(c16 const *name)
{
    c16 path[300];
    u32 len = GetSystemDirectoryW(path, 260);
    if (!len || len>=260) {
        return 0;
    }
    path[len++] = '\\';
    for (iz i = 0; name[i] && len<countof(path)-1; i++) {
        path[len++] = name[i];
    }
    path[len] = 0;
    return LoadLibraryW(path);
}

static void load_guiapi(guiapi *g)
{
    iptr k32 = GetModuleHandleW(L"kernel32.dll");
    iptr user = GetModuleHandleW(L"user32.dll");
    g->setdlldirectory   = (b32 (__stdcall *)(c16 const *))
                           GetProcAddress(k32, "SetDllDirectoryW");
    g->setdefaultdlldirs = (b32 (__stdcall *)(u32))
                           GetProcAddress(k32, "SetDefaultDllDirectories");
    if (g->setdlldirectory) {
        g->setdlldirectory(L"");  // nothing from the current directory
    }
    if (g->setdefaultdlldirs) {
        g->setdefaultdlldirs(0x800);  // LOAD_LIBRARY_SEARCH_SYSTEM32
    }
    g->setdpiaware   = (b32 (__stdcall *)(void))
                       GetProcAddress(user, "SetProcessDPIAware");
    g->setdpicontext = (b32 (__stdcall *)(iz))
                       GetProcAddress(user, "SetProcessDpiAwarenessContext");
    g->dpiforwindow  = (u32 (__stdcall *)(iptr))
                       GetProcAddress(user, "GetDpiForWindow");
    g->spiwithdpi    = (b32 (__stdcall *)(u32, u32, void *, u32, u32))
                       GetProcAddress(user, "SystemParametersInfoForDpi");
    g->metricsfordpi = (i32 (__stdcall *)(i32, u32))
                       GetProcAddress(user, "GetSystemMetricsForDpi");
    g->adjustfordpi  = (b32 (__stdcall *)(rect *, u32, b32, u32, u32))
                       GetProcAddress(user, "AdjustWindowRectExForDpi");
    iptr shcore = g->setdpicontext ? 0 : system_dll(L"shcore.dll");
    if (shcore) {
        g->setdpiawareness = (i32 (__stdcall *)(i32))
                             GetProcAddress(shcore, "SetProcessDpiAwareness");
    }
    iptr ux = system_dll(L"uxtheme.dll");
    if (ux) {
        g->setwindowtheme = (i32 (__stdcall *)(iptr, c16 const *,
                                               c16 const *))
                            GetProcAddress(ux, "SetWindowTheme");
    }
    iptr shlwapi = system_dll(L"shlwapi.dll");
    if (shlwapi) {
        g->strcmplogical = (i32 (__stdcall *)(c16 const *, c16 const *))
                           GetProcAddress(shlwapi, "StrCmpLogicalW");
    }
}


// Text: UTF-16 for Win32, from the WTF-8 of everything else

// A string as UTF-16, null-terminated, its '/' left be (fromwtf8, for
// paths, makes them '\').
static c16 *towide(arena *a, s8 s)
{
    c16 *r = new(a, s.len+1, c16);
    iz   n = 0;
    for (iz i = 0; i < s.len;) {
        n = utf16_put(r, n, wtf8_decode(s.s, s.len, &i));
    }
    r[n] = 0;
    return r;
}

static iz wlen(c16 const *s)
{
    iz n = 0;
    for (; s[n]; n++) {}
    return n;
}

// A bounded UTF-16 line being built, for messages and the list's text,
// cut short where it would overflow.
typedef struct {
    c16 *s;
    iz   len;
    iz   cap;  // including the null
} wbuf;

static wbuf wbuf_of(c16 *s, iz cap)
{
    wbuf b = {s, 0, cap};
    s[0] = 0;
    return b;
}

static void wput(wbuf *b, c16 const *s)
{
    for (; *s && b->len<b->cap-1; s++) {
        b->s[b->len++] = *s;
    }
    b->s[b->len] = 0;
}

static void wput8(wbuf *b, s8 s)
{
    for (iz i = 0; i<s.len && b->len<b->cap-2;) {
        b->len = utf16_put(b->s, b->len, wtf8_decode(s.s, s.len, &i));
    }
    b->s[b->len] = 0;
}

static void wputc(wbuf *b, c16 c)
{
    if (b->len < b->cap-1) {
        b->s[b->len++] = c;
    }
    b->s[b->len] = 0;
}

// A number, its digits grouped by sep (if any), as Explorer shows them.
static void wputnum(wbuf *b, u64 v, c16 sep)
{
    c16 t[32];
    iz  n = 0;
    for (i32 k = 0;; k++) {
        if (k && k%3==0 && sep) {
            t[n++] = sep;
        }
        t[n++] = (c16)('0' + v%10);
        v /= 10;
        if (!v) {
            break;
        }
    }
    while (n) {
        wputc(b, t[--n]);
    }
}

static void wputhex(wbuf *b, u32 v, i32 digits)
{
    for (i32 i = digits-1; i >= 0; i--) {
        wputc(b, (c16)"0123456789ABCDEF"[v>>(4*i) & 15]);
    }
}

// A size as Explorer abbreviates it ("1.5 MB"), to three digits.
static void wputsize(wbuf *b, u64 v)
{
    static c16 const *units[] = {L" KB", L" MB", L" GB", L" TB"};
    if (v < 1000) {
        wputnum(b, v, 0);
        wput(b, v==1 ? L" byte" : L" bytes");
        return;
    }
    i32 u = 0;
    u64 t = v/1024*10 + v%1024*10/1024;  // tenths of a unit
    for (; t>=9995 && u<3; u++) {
        t /= 1024;
    }
    if (t < 1000) {
        wputnum(b, t/10, 0);
        wputc(b, '.');
        wputnum(b, t%10, 0);
    } else {
        wputnum(b, (t+5)/10, 0);
    }
    wput(b, units[u]);
}


// Memory: a region of address space for each job, and for each archive's
// model, the job's that listed it, reserved whole and committed by
// os_extend as its arenas grow, then released whole. The host, at the
// region's head, embeds the os, and says which program runs, should it
// run out of memory.

enum { WHO_UI, WHO_ZIP, WHO_UNZIP };

typedef struct {
    os    ctx;
    i32   who;
    byte *base;  // the region
    iz    cap;
} host;

// Reserve a region for a struct of size head, which begins it, committed,
// and its arenas, from ctx->lo to ctx->hi: as much as the system lends up
// to cap, halving on refusal. Returns null if it lends too little.
static void *region_new(iz cap, iz head)
{
    iz page = 1 << 16;
    head = (head + page - 1) & -page;
    for (; cap >= (iz)1<<24; cap /= 2) {
        byte *p = VirtualAlloc(0, cap, MEM_RESERVE, PAGE_READWRITE);
        if (p) {
            if (!VirtualAlloc((uptr)p, head, MEM_COMMIT, PAGE_READWRITE)) {
                VirtualFree(p, 0, MEM_RELEASE);
                return 0;
            }
            host *h = (host *)p;
            bytefill(h, 0, head);
            h->base   = p;
            h->cap    = cap;
            h->ctx.lo = p + head;
            h->ctx.hi = p + cap;
            return p;
        }
    }
    return 0;
}

static void region_free(host *h)
{
    if (h) {
        VirtualFree(h->base, 0, MEM_RELEASE);
    }
}

// The arenas of a host's region: perm from the bottom of what is free,
// scratch from the top, as the programs' platforms make them.
static arena host_perm(host *h)
{
    return (arena){h->ctx.lo, h->ctx.lo, &h->ctx, 0};
}

static arena host_scratch(host *h)
{
    return (arena){h->ctx.hi, h->ctx.hi, &h->ctx, 1};
}

// Out of memory: the program running says so, as it would on its own,
// and ends its work (os_exit's escape), or for the GUI's own memory, the
// end.
static void os_oom(os *ctx)
{
    host *h = (host *)ctx;
    switch (h->who) {
    case WHO_ZIP:
        zip_os_oom(ctx);
        break;
    case WHO_UNZIP:
        unzip_os_oom(ctx);
        break;
    }
    MessageBoxW(0, L"Out of memory.", L"tugz", MB_OK|MB_ICONERROR);
    ExitProcess(4);
}


// The model of an archive: its entries as unzip lists them, in a tree of
// folders by the paths that unzip would extract them to (uz_mapname),
// built by unzip's visits on the worker thread, then the UI's alone.

typedef struct entry entry;
struct entry {
    i64    at;       // its central header's offset, unzip's picks by it
    iz     ordinal;  // in the central directory, zip's changes by it
    s8     stored;   // its name as stored
    s8     name;     // decoded, as unzip shows it
    i64    usize;
    i64    csize;
    i64    when;     // its local time, for sorting
    i32    tm[6];    // its time, local, normalized
    u32    crc;
    u32    extattr;
    u16    made;
    u16    method;
    u16    flags;
    u16    intattr;
    entry *same;     // a folder's next entry of the same path
};

typedef struct node node;
struct node {
    node   *parent;
    node   *child[4];  // a folder's folders, by name, a hash trie
    node   *first;     // its children as listed, linked
    node   *last;
    node   *next;
    node   *nextdir;   // the next of all folders, to visit each
    node  **kids;      // its children, folders first, once listed
    iz      nkids;
    iz      nfolders;  // the first of kids
    i32     sortkey;   // that kids are sorted by, or -1
    c16    *text;      // its name, as shown
    s8      name;      // its name, a component of a path
    entry  *e;         // its entry, or null for a folder without one
    i64     usize;     // a folder's totals, of everything within it
    i64     csize;
    i64     nfiles;
    iptr    item;      // its tree view item, once inserted
    i32     icon;      // its image in the image list, or -1
    b32     dir;
    b32     treed;     // its folders are in the tree view
};

typedef struct {
    host   *region;    // the listing job's, which holds it
    s8      path;      // the archive's
    c16    *wpath;
    node    root;
    node   *lastdir;
    iz      nentries;
    s8      comment;
    b32     readonly;  // read, but as zip could not change it
    b32     damaged;
    i64     size;      // the archive's, as listed, to notice a change
    u64     written;
} model;

static u64 node_hash(s8 s)
{
    u64 h = 0x100;
    for (iz i = 0; i < s.len; i++) {
        h ^= s.s[i];
        h *= 1111111111111111111u;
    }
    return h;
}

static node *node_new(model *m, node *parent, s8 name, b32 dir, arena *perm,
                      arena scratch)
{
    node *n = new(perm, 1, node);
    n->parent  = parent;
    n->name    = JOIN(perm, name);
    n->text    = towide(perm, uz_filter(name, &scratch));
    n->dir     = dir;
    n->icon    = -1;
    n->sortkey = -1;
    if (parent->last) {
        parent->last->next = n;
    } else {
        parent->first = n;
    }
    parent->last = n;
    parent->nkids++;
    parent->nfolders += dir;
    if (dir) {
        m->lastdir->nextdir = n;
        m->lastdir = n;
    }
    return n;
}

// A folder's folder of a name, made if new.
static node *folder_of(model *m, node *dir, s8 name, arena *perm,
                       arena scratch)
{
    node **slot = dir->child;
    for (u64 h = node_hash(name);; h <<= 2) {
        node *n = slot[h>>62];
        if (!n) {
            n = node_new(m, dir, name, 1, perm, scratch);
            slot[h>>62] = n;
            return n;
        } else if (s8equals(n->name, name)) {
            return n;
        }
        slot = n->child;
    }
}

// Each entry as unzip lists it, then, without one, the archive.
static void list_visit(void *data, unzipvisit *v)
{
    model *m    = data;
    arena *perm = v->perm;
    if (!v->e) {
        m->comment  = JOIN(perm, v->ar->end.comment);
        m->damaged  = v->cderr != CD_OK;
        m->readonly = m->damaged || v->ar->shift || v->ar->wrapped ||
                      v->ar->moved64 || v->ar->end.cut;
        for (node *d = &m->root; d; d = d->nextdir) {
            d->kids = new(perm, d->nkids, node *);
            iz f = 0, k = d->nfolders;
            for (node *n = d->first; n; n = n->next) {
                d->kids[n->dir ? f++ : k++] = n;
            }
        }
        return;
    }

    zentry *z = v->e;
    entry  *e = new(perm, 1, entry);
    e->at      = v->at;
    e->ordinal = (iz)v->ordinal;
    e->stored  = JOIN(perm, z->name);
    e->name    = JOIN(perm, v->name);
    e->usize   = z->usize;
    e->csize   = z->csize;
    e->crc     = z->crc;
    e->extattr = z->extattr;
    e->made    = z->made;
    e->method  = z->method;
    e->flags   = z->flags;
    e->intattr = z->intattr;
    uzizux x = uz_extra_izux(z->cextra, 1, z->dostime);
    if (x.flags & UZ_MTIME) {
        os_localtime(&m->region->ctx, x.mtime, e->tm);
    } else {
        uz_dosdate(z->dostime, e->tm);
        zip_gmtime(uz_timegm(e->tm), e->tm);  // normalized
    }
    e->when = uz_timegm(e->tm);
    m->nentries++;

    // Its place in the tree: the path unzip would make, its folders made
    // as needed, each named once, though entries' names may repeat
    uzpath mp   = uz_mapname(v->name, z->made, UZ_KEEPVER, v->scratch);
    b32    isdir = (mp.flags & UZ_DIR) != 0;
    node  *dir   = &m->root;
    s8     path  = mp.path;
    if (!path.len) {
        if (isdir) {
            return;  // the root itself, "/"
        }
        path = uz_filter(v->name, v->scratch);
        path = path.len ? path : S("?");
    }
    for (iz beg = 0, i = 0; i <= path.len; i++) {
        if (i<path.len && path.s[i]!='/') {
            continue;
        }
        s8  name = {path.s+beg, i-beg};
        b32 last = i == path.len;
        beg = i + 1;
        if (!last || isdir) {
            dir = folder_of(m, dir, name, perm, *v->scratch);
            if (last) {
                // Its own entry, or another of its path
                entry **p = &dir->e;
                for (; *p; p = &(*p)->same) {}
                *p = e;
            }
        } else {
            node *n = node_new(m, dir, name, 0, perm, *v->scratch);
            n->e = e;
            for (node *d = dir; d; d = d->parent) {
                d->usize  += e->usize;
                d->csize  += e->csize;
                d->nfiles += 1;
            }
        }
    }
}


// Jobs: the work of zip or unzip, one at a time, on a worker thread in a
// region of its own, run as a host (os_call). Its meter, on that thread,
// counts what is read, names what is opened, and ends the work once the
// UI asks it to stop; questions go to the UI by SendMessage.

enum { STATUS_STOPPED = 0x7fff };  // the work was cancelled

enum { THEN_OPEN, THEN_OPENWITH, THEN_EDIT };

enum {
    WM_APP_JOBDONE = WM_APP + 1,  // lparam: the job
    WM_APP_ASK,                   // lparam: an asking
    WM_APP_DROP,                  // lparam: a dropping
    WM_APP_ACTIVATED,             // the window, active again
};

typedef struct app app;
typedef struct job job;

struct job {
    host       h;            // first: the meter finds its job from its os
    app       *a;
    i32      (*work)(job *); // on the worker thread
    void     (*done)(app *, job *);  // on the UI thread, once it ends
    c16 const *title;        // of its progress
    i32        status;
    i32        stop;         // set by the UI: end the work
    i64        read;         // bytes read so far, by the meter
    i64        total;        // to read, if known, else zero
    c16        named[2][96]; // the name of the last path opened
    i32        current;      // which of named holds it
    iptr       thread;
    iptr       log;          // its standard output and error
    c16       *logtext;      // read once it ends, CRLF lines
    arena      perm;         // its own, for its calls and results
    u32        began;        // GetTickCount's

    // What it works on, by kind
    model     *model;        // a listing's
    zipcall    zc;
    unzipcall  uc;
    i32        replaceall;   // zip's replace: 1 all, 0 none, -1 ask
    s8         dest;         // an extraction's directory
    b32        kept;         // its region kept, as the model's
    b32        reload;       // a reload of what is shown
    i32        then;         // after extracting to a temporary: THEN_*
    entry    **pentries;     // uc.picks' entries, alike in order
    s8         zone;         // the archive's mark of the web, if any
    b32        move;         // extracting, then deleting what it did
    b32        openafter;    // the folder extracted to
    entry     *entry;
};

static void job_meter(os *ctx, s8 path, iz len)
{
    job *j = (job *)ctx;
    if (len) {
        __atomic_add_fetch(&j->read, (i64)len, __ATOMIC_RELAXED);
    }
    if (path.len) {
        iz cut = path.len;
        for (; cut>0 && path.s[cut-1]!='/' && path.s[cut-1]!='\\'; cut--) {}
        s8   name = {path.s+cut, path.len-cut};
        i32  next = !__atomic_load_n(&j->current, __ATOMIC_RELAXED);
        wbuf b    = wbuf_of(j->named[next], countof(j->named[next]));
        wput8(&b, name);
        __atomic_store_n(&j->current, next, __ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&j->stop, __ATOMIC_RELAXED)) {
        os_exit(ctx, STATUS_STOPPED);
    }
}

static i32 job_run(os *ctx, void *arg)
{
    (void)ctx;
    job *j = arg;
    return j->work(j);
}


// The application: one window, its controls, the archive shown, and
// the job running, if any. Its memory is a region of its own: perm for
// the run, and scratch for each message, a stack: each window procedure
// call takes it from where it is, and gives it back as it returns, so
// that calls within (modal loops, dialogs) take only what is free.

enum {
    ID_TREE = 1, ID_LIST, ID_TOOLBAR, ID_STATUS, ID_ADDRESS, ID_UP, ID_INFO,
    ID_EDITBAR, ID_EDITUPDATE, ID_EDITSTOP,
};

enum {
    CMD_NEW = 100, CMD_OPEN, CMD_CLOSE, CMD_PROPERTIES, CMD_EXIT,
    CMD_SELECTALL, CMD_RENAME, CMD_DELETE,
    CMD_ADD, CMD_ADDFILES, CMD_ADDFOLDER, CMD_EXTRACT, CMD_MOVE, CMD_TEST,
    CMD_EDIT, CMD_OPENENTRY, CMD_OPENWITH,
    CMD_UP, CMD_REFRESH, CMD_OPTIONS, CMD_ABOUT,
};

enum {
    COL_NAME, COL_SIZE, COL_PACKED, COL_RATIO, COL_METHOD, COL_MODIFIED,
    COL_ATTRIBS, COL_CRC, COL_VERSION, NCOLS
};

enum { TIMER_JOB = 1, TIMER_EDIT };

// The fixed images of the tree's and list's image list
enum { IMG_FOLDER, IMG_OPENFOLDER, IMG_ARCHIVE, IMG_FILE, IMG_FIXED };

typedef struct {
    i32 level;       // 0 to 9
    i32 mode;        // MODE_ADD, MODE_UPDATE, or MODE_FRESHEN
    b32 hidden;      // -S
    b32 noextra;     // -X
    b32 nodirs;      // -D
    b32 askreplace;  // before a file replaces an entry
    i32 overwrite;   // extracting over a file: OVERWRT_*
    b32 flat;        // extracting without folders
    b32 openafter;   // the folder extracted to
    c16 edit[300];   // the command Edit runs, %1 its file
    c16 dest[300];   // where it last extracted
} options;

typedef struct iconmap iconmap;
struct iconmap {
    iconmap *child[4];
    s8       ext;
    i32      image;
};

struct app {
    host     h;          // the UI's own memory
    arena    perm;
    arena    scratch;
    iptr     instance;
    guiapi   api;
    b32      xp;         // Windows XP (or 2003)
    iptr     hwnd, toolbar, address, upbutton, info, tree, list, status;
    iptr     editbar, editupdate, editstop;  // files being edited
    struct edit *edits;  // the latest first
    b32      ticking;    // watching the editors
    iptr     lastfocus;  // the control focused, while the window is not
    b32      popup;      // a context menu, enabled as it was made, is up
    rect     editrow;    // the edit bar's, painted as a note
    u32      dpi;
    iptr     font, monofont;
    i32      fontheight;
    i32      iconsize;   // of the tree's and list's
    i32      tbsize;     // of the toolbar's
    iptr     images;
    iptr     tbimages, tbdimmed;
    iconmap *icons;      // the list's images, by extension
    i32      split;      // the tree's width, at 96 DPI
    b32      splitting;
    b32      quitting;   // once the job ends
    i32      colwidth[NCOLS];  // at 96 DPI
    c16      sep;        // between groups of digits
    model   *m;
    node    *folder;     // shown in the list
    i32      sortcol;
    b32      sortdesc;
    b32      syncing;    // the tree's selection follows the list
    job     *job;        // running, if any
    iptr     progress;   // its dialog, once shown
    i32      shown;      // its percent, as last shown
    b32      asking;     // its question is open
    options  opt;
    iptr     accel;
    s8       temp;       // this session's temporary directory, once made
    i32      temps;      // its directories made
    b32      dragging;   // our own drag is under way
    struct dropsource *drag;  // its source, once it extracts
};

static i32 scale(app *a, i32 v)
{
    return MulDiv(v, (i32)a->dpi, 96);
}

static u32 window_dpi(app *a, iptr hwnd)
{
    if (a->api.dpiforwindow && hwnd) {
        u32 d = a->api.dpiforwindow(hwnd);
        if (d) {
            return d;
        }
    }
    iptr dc = GetDC(0);
    i32  d  = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(0, dc);
    return d>0 ? (u32)d : 96;
}

static i32 metric(app *a, i32 index)
{
    return a->api.metricsfordpi ? a->api.metricsfordpi(index, a->dpi)
                                : GetSystemMetrics(index);
}

// The message font, as the system gives it for the window's DPI, or as
// it gives it for its own, scaled.
static void make_fonts(app *a)
{
    nonclientmetrics m = {0};
    m.size = a->xp ? 500 : (u32)sizeof(m);
    b32 ok = 0;
    if (a->api.spiwithdpi) {
        ok = a->api.spiwithdpi(SPI_GETNONCLIENTMETRICS, m.size, &m, 0,
                               a->dpi);
    }
    if (!ok) {
        ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, m.size, &m, 0);
        u32 sys = window_dpi(a, 0);
        m.messagefont.height = MulDiv(m.messagefont.height, (i32)a->dpi,
                                      (i32)sys);
    }
    if (!ok) {
        m.messagefont = (logfont){0};
        m.messagefont.height = -scale(a, 12);
        bytecopy(m.messagefont.facename, L"Tahoma", 14);
    }
    if (a->font) {
        DeleteObject(a->font);
    }
    a->font = CreateFontIndirectW(&m.messagefont);

    logfont mono = {0};
    mono.height = m.messagefont.height;
    mono.weight = 400;
    c16 const *face = a->xp ? L"Lucida Console" : L"Consolas";
    bytecopy(mono.facename, face, (wlen(face)+1)*(iz)sizeof(c16));
    if (a->monofont) {
        DeleteObject(a->monofont);
    }
    a->monofont = CreateFontIndirectW(&mono);

    iptr dc  = GetDC(a->hwnd);
    iptr old = SelectObject(dc, a->font);
    textmetric tm = {0};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(a->hwnd, dc);
    a->fontheight = tm.height ? tm.height : scale(a, 16);
}


// Images: the system's icons for the types of files, as Explorer shows
// them, drawn at the list's size

// The system's icon for a file of a name (it need not exist), or 0.
static iptr shell_icon(app *a, c16 const *name, u32 attr, u32 flags)
{
    shfileinfo fi = {0};
    u32 size = a->iconsize<=GetSystemMetrics(SM_CXSMICON) ? SHGFI_SMALLICON
                                                          : SHGFI_LARGEICON;
    flags |= SHGFI_ICON | SHGFI_USEFILEATTRIBUTES | size;
    if (!SHGetFileInfoW(name, attr, &fi, (u32)sizeof(fi), flags)) {
        return 0;
    }
    return fi.icon;
}

static i32 add_icon(app *a, iptr icon)
{
    if (!icon) {
        return -1;
    }
    i32 i = ImageList_ReplaceIcon(a->images, -1, icon);
    DestroyIcon(icon);
    return i;
}

static void make_images(app *a)
{
    a->iconsize = metric(a, SM_CXSMICON);
    if (a->images) {
        ImageList_Destroy(a->images);
    }
    a->images = ImageList_Create(a->iconsize, a->iconsize,
                                 ILC_COLOR32|ILC_MASK, 16, 64);
    add_icon(a, shell_icon(a, L"folder", FILE_ATTRIBUTE_DIRECTORY, 0));
    add_icon(a, shell_icon(a, L"folder", FILE_ATTRIBUTE_DIRECTORY,
                           SHGFI_OPENICON));
    add_icon(a, shell_icon(a, L"a.zip", FILE_ATTRIBUTE_NORMAL, 0));
    add_icon(a, shell_icon(a, L"a", FILE_ATTRIBUTE_NORMAL, 0));
    a->icons = 0;
    SendMessageW(a->list, LVM_SETIMAGELIST, LVSIL_SMALL, (iz)a->images);
    SendMessageW(a->tree, TVM_SETIMAGELIST, TVSIL_NORMAL, (iz)a->images);
}

// A file's image, by its extension, found once for each.
static i32 node_image(app *a, node *n)
{
    if (n->dir) {
        return IMG_FOLDER;
    }
    s8 name = n->name;
    iz dot  = name.len;
    for (; dot>0 && name.s[dot-1]!='.'; dot--) {}
    if (!dot || name.len-dot>16) {
        return IMG_FILE;
    }
    u8 lower[16];
    s8 ext = {lower, name.len-dot};
    for (iz i = 0; i < ext.len; i++) {
        u8 c = name.s[dot+i];
        lower[i] = c>='A' && c<='Z' ? c+32 : c;
    }
    iconmap **m = &a->icons;
    for (u64 h = node_hash(ext); *m; h <<= 2) {
        if (s8equals((*m)->ext, ext)) {
            return (*m)->image;
        }
        m = &(*m)->child[h>>62];
    }
    *m = new(&a->perm, 1, iconmap);
    (*m)->ext = JOIN(&a->perm, ext);
    arena tmp  = a->scratch;
    c16  *file = towide(&tmp, JOIN(&tmp, S("a."), ext));
    i32   i    = add_icon(a, shell_icon(a, file, FILE_ATTRIBUTE_NORMAL, 0));
    (*m)->image = i<0 ? IMG_FILE : i;
    return (*m)->image;
}


// The list: an owner-data report view of the folder shown

static c16 const *column_names[NCOLS] = {
    L"Name", L"Size", L"Packed", L"Ratio", L"Method", L"Modified",
    L"Attributes", L"CRC-32", L"Version",
};

static i32 const column_widths[NCOLS] = {
    260, 96, 96, 60, 64, 130, 96, 76, 70,
};

static b32 column_right(i32 c)
{
    return c==COL_SIZE || c==COL_PACKED || c==COL_RATIO;
}

static i64 node_usize(node *n)
{
    return n->dir ? n->usize : n->e->usize;
}

static i64 node_csize(node *n)
{
    return n->dir ? n->csize : n->e->csize;
}

static i32 node_ratio(node *n)
{
    i64 c = node_csize(n);
    if (!n->dir && (n->e->flags & ZIP_FLAG_ENCRYPTED)) {
        c -= MIN(c, 12);  // as zipinfo, less the encryption header
    }
    return uz_ratio((u64)node_usize(n), (u64)c);
}

static void put_ratio(wbuf *b, i32 r)
{
    if (r < 0) {
        wputc(b, '-');
        r = -r;
    }
    wputnum(b, (u64)r/10, 0);
    wputc(b, '.');
    wputnum(b, (u64)r%10, 0);
    wputc(b, '%');
}

static void put_when(wbuf *b, i32 const tm[6])
{
    systemtime st = {0};
    st.year   = (u16)tm[0];
    st.month  = (u16)tm[1];
    st.day    = (u16)tm[2];
    st.hour   = (u16)tm[3];
    st.minute = (u16)tm[4];
    st.second = (u16)tm[5];
    c16 t[64];
    if (GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, 0, t,
                       countof(t))) {
        wput(b, t);
        wputc(b, ' ');
    }
    if (GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, 0, t,
                       countof(t))) {
        wput(b, t);
    }
}

// A cell of the list, as zipinfo would give its field.
static void put_cell(app *a, wbuf *b, node *n, i32 col)
{
    entry *e = n->e;
    switch (col) {
    case COL_NAME:
        wput(b, n->text);
        break;
    case COL_SIZE:
        wputnum(b, (u64)node_usize(n), a->sep);
        break;
    case COL_PACKED:
        wputnum(b, (u64)node_csize(n), a->sep);
        break;
    case COL_RATIO:
        if (node_usize(n)) {
            put_ratio(b, node_ratio(n));
        }
        break;
    case COL_METHOD:
        if (e && !n->dir) {
            zistr z = zi_method(e->method, e->flags);
            wput8(b, zi_s8(&z));
        }
        break;
    case COL_MODIFIED:
        if (e) {
            put_when(b, e->tm);
        }
        break;
    case COL_ATTRIBS:
        if (e) {
            zistr z = zi_attribs(e->made, e->extattr, e->name);
            s8    s = zi_s8(&z);
            s.len = MIN(s.len, 10);  // without the version after it
            for (; s.len && s.s[s.len-1]==' '; s.len--) {}
            wput8(b, s);
        }
        break;
    case COL_CRC:
        if (e && !n->dir) {
            wputhex(b, e->crc, 8);
        }
        break;
    case COL_VERSION:
        if (e) {
            u32 v = e->made & 0xff;
            wputnum(b, v/10, 0);
            wputc(b, '.');
            wputnum(b, v%10, 0);
            wputc(b, ' ');
            wput8(b, zi_host(e->made));
        }
        break;
    }
}

// Compare two of a folder's children by a column, as Explorer does,
// names by their numbers ("2" before "10"), ties by name.
static i32 node_cmp(app *a, node *x, node *y, i32 col)
{
    i64 d = 0;
    switch (col) {
    case COL_SIZE:
        d = node_usize(x) - node_usize(y);
        break;
    case COL_PACKED:
        d = node_csize(x) - node_csize(y);
        break;
    case COL_RATIO:
        d = node_ratio(x) - node_ratio(y);
        break;
    case COL_MODIFIED:
        d = (x->e ? x->e->when : 0) - (y->e ? y->e->when : 0);
        break;
    case COL_CRC:
        d = (i64)(x->e ? x->e->crc : 0) - (i64)(y->e ? y->e->crc : 0);
        break;
    case COL_METHOD:
    case COL_ATTRIBS:
    case COL_VERSION: {
        c16  bx[64], by[64];
        wbuf wx = wbuf_of(bx, countof(bx));
        wbuf wy = wbuf_of(by, countof(by));
        put_cell(a, &wx, x, col);
        put_cell(a, &wy, y, col);
        d = a->api.strcmplogical ? a->api.strcmplogical(bx, by) : 0;
        break;
    }
    }
    if (!d) {
        d = a->api.strcmplogical ? a->api.strcmplogical(x->text, y->text)
                                 : 0;
    }
    return d<0 ? -1 : d>0;
}

// Sort nodes by a column, a merge sort through scratch.
static void sort_nodes(app *a, node **v, iz n, i32 col, b32 desc,
                       arena scratch)
{
    node **t = new(&scratch, n, node *);
    for (iz w = 1; w < n; w *= 2) {
        for (iz lo = 0; lo < n; lo += 2*w) {
            iz mid = MIN(lo+w, n);
            iz hi  = MIN(lo+2*w, n);
            iz i = lo, j = mid, k = lo;
            while (i<mid || j<hi) {
                b32 right = i == mid;
                if (!right && j<hi) {
                    i32 c = node_cmp(a, v[i], v[j], col);
                    right = desc ? c<0 : c>0;
                }
                t[k++] = right ? v[j++] : v[i++];
            }
        }
        bytecopy(v, t, n*(iz)sizeof(*v));
    }
}

static void sort_folder(app *a, node *d)
{
    i32 key = a->sortcol*2 + !!a->sortdesc;
    if (d->sortkey != key) {
        sort_nodes(a, d->kids, d->nfolders, a->sortcol, a->sortdesc,
                   a->scratch);
        sort_nodes(a, d->kids+d->nfolders, d->nkids-d->nfolders,
                   a->sortcol, a->sortdesc, a->scratch);
        d->sortkey = key;
    }
}

static void show_sort(app *a)
{
    iptr header = (iptr)SendMessageW(a->list, LVM_GETHEADER, 0, 0);
    for (i32 c = 0; c < NCOLS; c++) {
        hditem h = {0};
        h.mask = HDI_FORMAT;
        SendMessageW(header, HDM_GETITEMW, (uptr)c, (iz)&h);
        h.fmt &= ~(HDF_SORTUP|HDF_SORTDOWN);
        if (c == a->sortcol) {
            h.fmt |= a->sortdesc ? HDF_SORTDOWN : HDF_SORTUP;
        }
        SendMessageW(header, HDM_SETITEMW, (uptr)c, (iz)&h);
    }
}


// The selection: the nodes chosen in the list, or in the tree

typedef struct {
    node **v;
    iz     n;
} nodes;

static nodes selection(app *a, arena *perm)
{
    nodes r = {0};
    if (!a->folder) {
        return r;
    }
    iz count = SendMessageW(a->list, LVM_GETSELECTEDCOUNT, 0, 0);
    r.v = new(perm, MAX(count, 0), node *);
    for (iz i = -1; r.n < count;) {
        i = SendMessageW(a->list, LVM_GETNEXTITEM, (uptr)i, LVNI_SELECTED);
        if (i<0 || i>=a->folder->nkids) {
            break;
        }
        r.v[r.n++] = a->folder->kids[i];
    }
    return r;
}

// The path of a node from a folder above it, '/' between names, as the
// archive's names would join them.
static s8 node_path(node *n, node *from, arena *perm)
{
    iz len = 0;
    for (node *p = n; p && p!=from; p = p->parent) {
        len += p->name.len + (p != n);
    }
    s8 r = {newstr(perm, len), len};
    for (node *p = n; p && p!=from; p = p->parent) {
        len -= p->name.len;
        bytecopy(r.s+len, p->name.s, p->name.len);
        if (len) {
            r.s[--len] = '/';
        }
    }
    return r;
}


// The window

static void set_status(app *a)
{
    arena tmp = a->scratch;
    c16  buf[256];
    wbuf b = wbuf_of(buf, countof(buf));
    if (a->m && a->folder) {
        iz   count = SendMessageW(a->list, LVM_GETSELECTEDCOUNT, 0, 0);
        u64  bytes = 0;
        nodes s    = selection(a, &tmp);
        for (iz i = 0; i < s.n; i++) {
            bytes += (u64)node_usize(s.v[i]);
        }
        if (count) {
            wputnum(&b, (u64)count, a->sep);
            wput(&b, count==1 ? L" item selected, " : L" items selected, ");
            wputsize(&b, bytes);
        } else {
            wputnum(&b, (u64)a->folder->nkids, a->sep);
            wput(&b, a->folder->nkids==1 ? L" item" : L" items");
        }
    }
    SendMessageW(a->status, SB_SETTEXTW, 0, (iz)buf);

    b = wbuf_of(buf, countof(buf));
    if (a->m) {
        node *r = &a->m->root;
        wputnum(&b, (u64)r->nfiles, a->sep);
        wput(&b, r->nfiles==1 ? L" file, " : L" files, ");
        wputsize(&b, (u64)r->usize);
        wput(&b, L", ");
        wputsize(&b, (u64)r->csize);
        wput(&b, L" packed");
        if (r->usize) {
            wput(&b, L" (");
            put_ratio(&b, uz_ratio((u64)r->usize, (u64)r->csize));
            wputc(&b, ')');
        }
    }
    SendMessageW(a->status, SB_SETTEXTW, 1, (iz)buf);
}

static void set_address(app *a)
{
    arena tmp = a->scratch;
    c16   buf[1024];
    wbuf  b = wbuf_of(buf, countof(buf));
    if (a->m) {
        wput(&b, a->m->root.text);
        if (a->folder && a->folder!=&a->m->root) {
            wputc(&b, '\\');
            s8 path = node_path(a->folder, &a->m->root, &tmp);
            for (iz i = 0; i < path.len; i++) {
                path.s[i] = path.s[i]=='/' ? '\\' : path.s[i];
            }
            wput8(&b, path);
        }
    }
    SetWindowTextW(a->address, buf);
    EnableWindow(a->upbutton, a->folder && a->folder->parent);
}

static void set_title(app *a)
{
    c16  buf[400];
    wbuf b = wbuf_of(buf, countof(buf));
    if (a->m) {
        wput(&b, a->m->root.text);
        wput(&b, L" - ");
    }
    wput(&b, L"tugz");
    SetWindowTextW(a->hwnd, buf);
}

// Insert a folder's folders in the tree, in order of name, once.
static void tree_fill(app *a, node *d)
{
    if (d->treed) {
        return;
    }
    d->treed = 1;
    arena  tmp = a->scratch;
    node **v   = new(&tmp, d->nfolders, node *);
    bytecopy(v, d->kids, d->nfolders*(iz)sizeof(*v));
    sort_nodes(a, v, d->nfolders, COL_NAME, 0, tmp);
    for (iz i = 0; i < d->nfolders; i++) {
        node *k = v[i];
        tvinsertstruct t = {0};
        t.parent = d->item;
        t.after  = TVI_LAST;
        t.item.base.mask     = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE |
                               TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
        t.item.base.text     = k->text;
        t.item.base.image    = IMG_FOLDER;
        t.item.base.selimage = IMG_OPENFOLDER;
        t.item.base.children = k->nfolders > 0;
        t.item.base.lparam   = (iz)k;
        k->item = (iptr)SendMessageW(a->tree, TVM_INSERTITEMW, 0, (iz)&t);
    }
}

// Select a folder's item in the tree, its folders above it expanded.
static void tree_select(app *a, node *d)
{
    if (!d->item && d->parent) {
        tree_select(a, d->parent);
        tree_fill(a, d->parent);
        SendMessageW(a->tree, TVM_EXPAND, TVE_EXPAND, (iz)d->parent->item);
    }
    a->syncing = 1;
    SendMessageW(a->tree, TVM_SELECTITEM, TVGN_CARET, (iz)d->item);
    a->syncing = 0;
}

// Show a folder in the list, and select a child in it, if any.
static void navigate(app *a, node *d, node *select)
{
    a->folder = d;
    sort_folder(a, d);
    SendMessageW(a->list, LVM_SETITEMCOUNT, (uptr)d->nkids, 0);
    lvitem none = {0};
    none.statemask = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(a->list, LVM_SETITEMSTATE, (uptr)-1, (iz)&none);
    for (iz i = 0; select && i<d->nkids; i++) {
        if (d->kids[i] == select) {
            lvitem it = {0};
            it.state = it.statemask = LVIS_SELECTED | LVIS_FOCUSED;
            SendMessageW(a->list, LVM_SETITEMSTATE, (uptr)i, (iz)&it);
            SendMessageW(a->list, LVM_ENSUREVISIBLE, (uptr)i, 0);
        }
    }
    InvalidateRect(a->list, 0, 1);
    tree_select(a, d);
    set_address(a);
    set_status(a);
}

static void layout(app *a);
static void update_commands(app *a);

// The folder at a path, '/' between names, or as far as it goes if
// partial, else null.
static node *folder_at(model *m, s8 at, b32 partial)
{
    node *d = &m->root;
    for (iz beg = 0, i = 0; i <= at.len; i++) {
        if (i<at.len && at.s[i]!='/') {
            continue;
        }
        s8    name = {at.s+beg, i-beg};
        node *next = 0;
        for (iz k = 0; name.len && k<d->nfolders && !next; k++) {
            next = s8equals(d->kids[k]->name, name) ? d->kids[k] : 0;
        }
        if (!next) {
            return name.len && !partial ? 0 : d;
        }
        d = next;
        beg = i + 1;
    }
    return d;
}

// Expand a folder in the tree, and those above it.
static void tree_expand(app *a, node *d)
{
    if (!d->item && d->parent) {
        tree_expand(a, d->parent);
    }
    if (d->item) {
        tree_fill(a, d);
        SendMessageW(a->tree, TVM_EXPAND, TVE_EXPAND, (iz)d->item);
    }
}

// Show an archive's model, the old one's let go, at a folder of the same
// path, if it has one, else at the root. Shown again, the same archive's
// folders expanded in the tree stay expanded.
static void show_model(app *a, model *m, s8 at)
{
    arena *tmp      = &a->scratch;
    s8    *expanded = 0;
    iz     nexpanded = 0;
    if (a->m && m && s8equals(a->m->path, m->path)) {
        iz cap = 0;
        for (node *d = &a->m->root; d; d = d->nextdir) {
            cap += d->item != 0;
        }
        expanded = new(tmp, cap, s8);
        for (node *d = &a->m->root; d; d = d->nextdir) {
            if (d->item && (SendMessageW(a->tree, TVM_GETITEMSTATE,
                                         (uptr)d->item, TVIS_EXPANDED) &
                            TVIS_EXPANDED)) {
                expanded[nexpanded++] = node_path(d, &a->m->root, tmp);
            }
        }
    }

    SendMessageW(a->list, LVM_SETITEMCOUNT, 0, 0);
    SendMessageW(a->tree, TVM_DELETEITEM, 0, (iz)TVI_ROOT);
    a->folder = 0;
    if (a->m) {
        region_free(a->m->region);
    }
    a->m = m;
    if (m) {
        tvinsertstruct t = {0};
        t.parent = TVI_ROOT;
        t.after  = TVI_LAST;
        t.item.base.mask     = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE |
                               TVIF_SELECTEDIMAGE | TVIF_CHILDREN;
        t.item.base.text     = m->root.text;
        t.item.base.image    = IMG_ARCHIVE;
        t.item.base.selimage = IMG_ARCHIVE;
        t.item.base.children = m->root.nfolders > 0;
        t.item.base.lparam   = (iz)&m->root;
        m->root.item = (iptr)SendMessageW(a->tree, TVM_INSERTITEMW, 0,
                                          (iz)&t);
        tree_fill(a, &m->root);
        SendMessageW(a->tree, TVM_EXPAND, TVE_EXPAND, (iz)m->root.item);
        for (iz i = 0; i < nexpanded; i++) {
            node *d = folder_at(m, expanded[i], 0);
            if (d) {
                tree_expand(a, d);
            }
        }
        navigate(a, folder_at(m, at, 1), 0);
    } else {
        set_address(a);
        set_status(a);
    }
    set_title(a);
    ShowWindow(a->info, m && (m->readonly || m->damaged) ? SW_SHOW : SW_HIDE);
    if (m && (m->readonly || m->damaged)) {
        SetWindowTextW(a->info, m->damaged
            ? L"  This archive is damaged: what could be read is shown, "
              L"but it cannot be changed."
            : L"  This archive can be read, but not changed: its offsets "
              L"do not account for the data before it.");
    }
    layout(a);
    update_commands(a);
}

static void layout(app *a)
{
    rect rc = {0};
    GetClientRect(a->hwnd, &rc);
    i32 w = rc.right;
    i32 h = rc.bottom;

    SendMessageW(a->toolbar, TB_AUTOSIZE, 0, 0);
    rect tr = {0};
    GetWindowRect(a->toolbar, &tr);
    i32 y = tr.bottom - tr.top;

    SendMessageW(a->status, WM_SIZE, 0, 0);
    rect sr = {0};
    GetWindowRect(a->status, &sr);
    i32 sh = sr.bottom - sr.top;
    i32 parts[2] = {w/2, -1};
    SendMessageW(a->status, SB_SETPARTS, 2, (iz)parts);

    i32 pad  = scale(a, 3);
    i32 rowh = a->fontheight + 2*pad + scale(a, 4);
    MoveWindow(a->upbutton, pad, y+pad, rowh, rowh-2*pad, 1);
    MoveWindow(a->address, rowh+2*pad, y+pad, w-rowh-3*pad, rowh-2*pad, 1);
    y += rowh;
    if (a->m && (a->m->readonly || a->m->damaged)) {
        i32 ih = a->fontheight + scale(a, 8);
        MoveWindow(a->info, 0, y, w, ih, 1);
        y += ih;
    }
    if (a->edits) {
        i32 bw = scale(a, 90);
        i32 bh = rowh - 2*pad;
        b32 up = IsWindowVisible(a->editupdate);
        i32 x  = w - pad - bw;
        MoveWindow(a->editstop, x, y+pad, bw, bh, 1);
        if (up) {
            x -= pad + bw;
            MoveWindow(a->editupdate, x, y+pad, bw, bh, 1);
        }
        MoveWindow(a->editbar, 2*pad, y+pad+(bh-a->fontheight)/2,
                   x-3*pad, a->fontheight, 1);
        a->editrow = (rect){0, y, w, y+rowh};
        InvalidateRect(a->hwnd, &a->editrow, 1);
        y += rowh;
    }

    i32 gap   = scale(a, 5);
    i32 split = MAX(MIN(scale(a, a->split), w - scale(a, 160)),
                    scale(a, 100));
    MoveWindow(a->tree, 0, y, split, h-sh-y, 1);
    MoveWindow(a->list, split+gap, y, w-split-gap, h-sh-y, 1);
}

// Whether a command applies, to the archive shown and the selection.
static b32 command_on(app *a, i32 cmd, nodes s)
{
    b32 open = a->m != 0;
    b32 can  = open && !a->m->readonly && !a->m->damaged;
    b32 file = s.n==1 && !s.v[0]->dir;
    switch (cmd) {
    case CMD_EXTRACT:
    case CMD_TEST:        return open && a->m->nentries;
    case CMD_CLOSE:
    case CMD_SELECTALL:
    case CMD_REFRESH:     return open;
    case CMD_PROPERTIES:  return open && s.n<=1;
    case CMD_ADD:
    case CMD_ADDFILES:
    case CMD_ADDFOLDER:   return !open || can;
    case CMD_MOVE:
    case CMD_DELETE:      return can && s.n;
    case CMD_RENAME:      return can && s.n==1;
    case CMD_EDIT:        return can && file;
    case CMD_OPENENTRY:   return open && s.n==1;
    case CMD_OPENWITH:    return open && file;
    case CMD_UP:          return a->folder && a->folder->parent;
    }
    return 1;
}

// The toolbar's buttons enabled as their commands apply (the menus' are
// as they open: WM_INITMENUPOPUP).
static void update_commands(app *a)
{
    arena tmp = a->scratch;
    nodes s   = selection(a, &tmp);
    i32   cmds[] = {
        CMD_ADD, CMD_EXTRACT, CMD_MOVE, CMD_DELETE, CMD_RENAME, CMD_EDIT,
        CMD_TEST,
    };
    for (i32 i = 0; i < countof(cmds); i++) {
        SendMessageW(a->toolbar, TB_ENABLEBUTTON, (uptr)cmds[i],
                     command_on(a, cmds[i], s));
    }
    EnableWindow(a->upbutton, command_on(a, CMD_UP, s));
}


// Dialogs, from templates built in memory (DLGTEMPLATEEX) in the message
// font, laid out in dialog units, which the system scales to the font,
// and under per-monitor DPI awareness, to each monitor

enum {
    DLG_BUTTON = 0x80, DLG_EDIT, DLG_STATIC, DLG_LISTBOX, DLG_SCROLLBAR,
    DLG_COMBOBOX,
};

typedef struct {
    u8 *s;
    iz  len;
    iz  cap;
    iz  count;  // where its count of items is
} dlgt;

static void dt_u16(dlgt *t, u32 v)
{
    assert(t->len+2 <= t->cap);
    t->s[t->len++] = (u8)v;
    t->s[t->len++] = (u8)(v >> 8);
}

static void dt_u32(dlgt *t, u32 v)
{
    dt_u16(t, v & 0xffff);
    dt_u16(t, v >> 16);
}

static void dt_text(dlgt *t, c16 const *s)
{
    for (; *s; s++) {
        dt_u16(t, *s);
    }
    dt_u16(t, 0);
}

static dlgt dlg_begin(app *a, arena *perm, c16 const *title, i32 w, i32 h,
                      u32 style)
{
    dlgt t = {0};
    t.cap = 1 << 14;
    t.s   = newbytes(perm, t.cap);
    dt_u16(&t, 1);       // its version
    dt_u16(&t, 0xffff);  // DLGTEMPLATEEX
    dt_u32(&t, 0);       // help ID
    dt_u32(&t, 0);       // extended style
    dt_u32(&t, WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
               DS_SETFONT | DS_CENTER | style);
    t.count = t.len;
    dt_u16(&t, 0);
    dt_u16(&t, 0);
    dt_u16(&t, 0);
    dt_u16(&t, (u32)w);
    dt_u16(&t, (u32)h);
    dt_u16(&t, 0);  // no menu
    dt_u16(&t, 0);  // the dialog class
    dt_text(&t, title);

    // The message font, in points at the window's DPI
    nonclientmetrics m = {0};
    m.size = a->xp ? 500 : (u32)sizeof(m);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, m.size, &m, 0);
    i32 points = MulDiv(-m.messagefont.height, 72, (i32)window_dpi(a, 0));
    points = points>0 ? points : 8;
    dt_u16(&t, (u32)points);
    dt_u16(&t, 400);
    dt_u16(&t, 0x0100);  // not italic, the default character set
    dt_text(&t, m.messagefont.facename[0] ? m.messagefont.facename
                                          : L"MS Shell Dlg");
    return t;
}

static void dlg_item(dlgt *t, u32 class, c16 const *classname, u32 style,
                     i32 x, i32 y, i32 w, i32 h, i32 id, c16 const *text)
{
    while (t->len & 3) {
        t->s[t->len++] = 0;
    }
    dt_u32(t, 0);  // help ID
    dt_u32(t, 0);  // extended style
    dt_u32(t, WS_CHILD | WS_VISIBLE | style);
    dt_u16(t, (u32)x);
    dt_u16(t, (u32)y);
    dt_u16(t, (u32)w);
    dt_u16(t, (u32)h);
    dt_u32(t, (u32)id);
    if (classname) {
        dt_text(t, classname);
    } else {
        dt_u16(t, 0xffff);
        dt_u16(t, class);
    }
    dt_text(t, text ? text : L"");
    dt_u16(t, 0);  // no creation data
    u32 n = t->s[t->count] | (u32)t->s[t->count+1]<<8;
    n++;
    t->s[t->count]   = (u8)n;
    t->s[t->count+1] = (u8)(n >> 8);
}

static void dlg_button(dlgt *t, i32 x, i32 y, i32 w, i32 id, c16 const *text,
                       b32 def)
{
    dlg_item(t, DLG_BUTTON, 0, WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : 0),
             x, y, w, 14, id, text);
}

// A label, its & marking the next control's key.
static void dlg_label(dlgt *t, i32 x, i32 y, i32 w, i32 h, i32 id,
                      c16 const *text)
{
    dlg_item(t, DLG_STATIC, 0, SS_LEFT, x, y, w, h, id, text);
}

// Text shown as it is, such as a name.
static void dlg_note(dlgt *t, i32 x, i32 y, i32 w, i32 h, i32 id,
                     c16 const *text)
{
    dlg_item(t, DLG_STATIC, 0, SS_LEFT|SS_NOPREFIX, x, y, w, h, id, text);
}

static iz __stdcall dlg_proc(iptr, u32, uptr, iz);

typedef struct dialog dialog;
struct dialog {
    app  *a;
    iptr  hwnd;
    b32 (*init)(dialog *);
    b32 (*command)(dialog *, i32 id, i32 code);  // true: handled
    void *data;
    iz    result;
};

static iz run_dialog(app *a, dlgt *t, dialog *d)
{
    d->a = a;
    iptr owner = a->progress ? a->progress : a->hwnd;
    return DialogBoxIndirectParamW(a->instance, t->s, owner, (void *)dlg_proc,
                                   (iz)d);
}

static iz __stdcall dlg_proc(iptr hwnd, u32 message, uptr wp, iz lp)
{
    dialog *d = (dialog *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (message) {
    case WM_INITDIALOG:
        d = (dialog *)lp;
        d->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (iz)d);
        return d->init ? d->init(d) : 1;
    case WM_COMMAND:
        if (d && d->command && d->command(d, (i32)(wp & 0xffff),
                                          (i32)(wp >> 16 & 0xffff))) {
            return 1;
        } else if ((wp&0xffff)==IDOK || (wp&0xffff)==IDCANCEL) {
            EndDialog(hwnd, (iz)(wp & 0xffff));
            return 1;
        }
        break;
    }
    return 0;
}

static s8 dlg_text(dialog *d, i32 id, arena *perm)
{
    iptr  w   = GetDlgItem(d->hwnd, id);
    i32   n   = GetWindowTextLengthW(w);
    c16  *buf = new(perm, n+1, c16);  // (garbage beside the result)
    GetWindowTextW(w, buf, n+1);
    return towtf8(perm, buf);
}

static b32 dlg_checked(dialog *d, i32 id)
{
    return SendMessageW(GetDlgItem(d->hwnd, id), BM_GETCHECK, 0, 0) ==
           BST_CHECKED;
}

static void dlg_check(dialog *d, i32 id, b32 on)
{
    SendMessageW(GetDlgItem(d->hwnd, id), BM_SETCHECK, on ? BST_CHECKED : 0,
                 0);
}


// A report: a summary, then text, such as a job's log, in a monospace
// box to read, scroll, and copy.

typedef struct {
    c16 const *summary;
    c16 const *text;
} report_data;

enum { ID_SUMMARY = 10, ID_TEXT };

static b32 report_init(dialog *d)
{
    report_data *r = d->data;
    SetDlgItemTextW(d->hwnd, ID_SUMMARY, r->summary);
    iptr text = GetDlgItem(d->hwnd, ID_TEXT);
    SendMessageW(text, WM_SETFONT, (uptr)d->a->monofont, 0);
    SendMessageW(text, EM_LIMITTEXT, 0, 0);
    SetWindowTextW(text, r->text);
    SetFocus(GetDlgItem(d->hwnd, IDOK));
    return 0;
}

static void show_report(app *a, c16 const *title, c16 const *summary,
                        c16 const *text)
{
    arena *tmp = &a->scratch;
    dlgt   t   = dlg_begin(a, tmp, title, 340, 214, 0);
    dlg_note(&t, 7, 7, 326, 24, ID_SUMMARY, 0);
    dlg_item(&t, DLG_EDIT, 0, WS_BORDER | WS_VSCROLL | WS_HSCROLL |
             WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
             ES_AUTOHSCROLL, 7, 34, 326, 154, ID_TEXT, 0);
    dlg_button(&t, 283, 193, 50, IDOK, L"OK", 1);
    report_data r = {summary, text};
    dialog d = {0};
    d.init = report_init;
    d.data = &r;
    run_dialog(a, &t, &d);
}

static void message(app *a, c16 const *text, u32 icon)
{
    iptr owner = a->progress ? a->progress : a->hwnd;
    MessageBoxW(owner, text, L"tugz", MB_OK | icon);
}


// Focus: the control that had it, kept while the window is inactive or
// disabled (a dialog, a job), and given back, else the list.

static void save_focus(app *a)
{
    iptr f = GetFocus();
    if (f && IsChild(a->hwnd, f)) {
        a->lastfocus = f;
    }
}

static void restore_focus(app *a)
{
    iptr f = a->lastfocus;
    if (!f || !IsChild(a->hwnd, f) || !IsWindowVisible(f) ||
        !IsWindowEnabled(f)) {
        f = a->list;
    }
    SetFocus(f);
}


// Jobs: started, watched, and ended on the UI thread

static u32 __stdcall worker(void *arg)
{
    job *j = arg;
    j->status = os_call(&j->h.ctx, job_run, j);
    oom_out = 0;  // as zip leaves it should the work end there
    PostMessageW(j->a->hwnd, WM_APP_JOBDONE, 0, (iz)j);
    return 0;
}

// A job in a region of its own, for a program, null if no memory.
static job *job_new(app *a, i32 who)
{
    iz   cap = sizeof(void *)==8 ? (iz)1<<36 : (iz)1<<29;
    job *j   = region_new(cap, (iz)sizeof(job));
    if (!j) {
        message(a, L"Not enough memory.", MB_ICONERROR);
        return 0;
    }
    j->h.who         = who;
    j->h.ctx.api     = a->h.ctx.api;
    j->h.ctx.meter   = job_meter;
    j->a             = a;
    j->perm          = host_perm(&j->h);
    j->replaceall    = -1;
    return j;
}

W32(u32) SetFilePointer(iptr, i32, i32 *, u32);

// Begin a job on the worker thread, its standard output and error going
// to a log, a temporary file read as it ends.
static b32 job_start(app *a, job *j)
{
    c16  dir[300];
    wbuf p = wbuf_of(dir, countof(dir));
    p.len  = GetTempPathW(260, dir);
    c16 name[64];
    wbuf b = wbuf_of(name, countof(name));
    wput(&b, L"tugz-");
    wputnum(&b, GetCurrentProcessId(), 0);
    wput(&b, L"-");
    wputnum(&b, GetTickCount(), 0);
    wput(&b, L".log");
    wput(&p, name);
    j->log = CreateFileW(dir, GENERIC_READ|GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,
                         0);
    if (j->log == INVALID_HANDLE_VALUE) {
        j->log = 0;
    }
    j->h.ctx.handles[0] = 0;
    j->h.ctx.handles[1] = j->log;
    j->h.ctx.handles[2] = j->log;
    j->began = GetTickCount();
    a->job = j;
    a->shown = -1;
    j->thread = CreateThread(0, 0, worker, j, 0, 0);
    if (!j->thread) {
        a->job = 0;
        if (j->log) {
            CloseHandle(j->log);
        }
        region_free(&j->h);
        message(a, L"Could not start the work.", MB_ICONERROR);
        return 0;
    }
    save_focus(a);
    EnableWindow(a->hwnd, 0);
    SetTimer(a->hwnd, TIMER_JOB, 100, 0);
    return 1;
}

// The log as text to show: UTF-16, lines ending CRLF, its first MiB.
static c16 *read_log(job *j, arena *perm)
{
    if (!j->log) {
        return L"";
    }
    u32 size = SetFilePointer(j->log, 0, 0, 2);  // FILE_END
    SetFilePointer(j->log, 0, 0, 0);             // FILE_BEGIN
    size = size==0xffffffff ? 0 : MIN(size, 1u<<20);
    u8 *buf = newbytes(perm, size);
    u32 got = 0;
    ReadFile(j->log, buf, size, &got, 0);
    s8   text = {buf, got};
    c16 *r    = new(perm, 2*text.len+1, c16);
    iz   n    = 0;
    for (iz i = 0; i < text.len;) {
        u32 c = wtf8_decode(text.s, text.len, &i);
        if (c=='\n' && (!n || r[n-1]!='\r')) {
            r[n++] = '\r';
        }
        n = utf16_put(r, n, c);
    }
    r[n] = 0;
    return r;
}

enum { ID_PNAME = 10, ID_PBAR, ID_PBYTES };

static b32 progress_command(dialog *d, i32 id, i32 code)
{
    (void)code;
    if (id == IDCANCEL) {
        job *j = d->data;
        __atomic_store_n(&j->stop, 1, __ATOMIC_RELAXED);
        SetDlgItemTextW(d->hwnd, IDCANCEL, L"Stopping...");
        EnableWindow(GetDlgItem(d->hwnd, IDCANCEL), 0);
        return 1;
    }
    return 0;
}

static b32 progress_init(dialog *d)
{
    iptr bar = GetDlgItem(d->hwnd, ID_PBAR);
    SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
    return 1;
}

static dialog progress_dialog;

static void show_progress(app *a, job *j)
{
    arena *tmp = &a->scratch;
    dlgt   t   = dlg_begin(a, tmp, j->title, 260, 74, 0);
    dlg_item(&t, DLG_STATIC, 0, SS_LEFT|SS_NOPREFIX|SS_PATHELLIPSIS, 7, 7,
             246, 9, ID_PNAME, 0);
    dlg_item(&t, 0, L"msctls_progress32", PBS_SMOOTH, 7, 20, 246, 11,
             ID_PBAR, 0);
    dlg_note(&t, 7, 36, 180, 9, ID_PBYTES, 0);
    dlg_button(&t, 203, 53, 50, IDCANCEL, L"Cancel", 1);
    progress_dialog = (dialog){0};
    progress_dialog.a       = a;
    progress_dialog.init    = progress_init;
    progress_dialog.command = progress_command;
    progress_dialog.data    = j;
    a->progress = CreateDialogIndirectParamW(a->instance, t.s, a->hwnd,
                                             (void *)dlg_proc,
                                             (iz)&progress_dialog);
    ShowWindow(a->progress, SW_SHOW);
}

// The job's progress, every tenth of a second, its dialog shown once it
// has run a third of a second.
static void job_tick(app *a)
{
    job *j = a->job;
    if (!j) {
        return;
    }
    if (!a->progress && !a->asking &&
        GetTickCount()-j->began>=300) {
        show_progress(a, j);
    }
    if (!a->progress) {
        return;
    }
    i32  cur = __atomic_load_n(&j->current, __ATOMIC_ACQUIRE);
    c16  name[96];
    bytecopy(name, j->named[cur], (iz)sizeof(name));
    name[countof(name)-1] = 0;
    SetDlgItemTextW(a->progress, ID_PNAME, name);

    i64  read  = __atomic_load_n(&j->read, __ATOMIC_RELAXED);
    i64  total = j->total ? j->total : j->zc.total;
    read = total>0 ? MIN(read, total) : read;  // (headers count too)
    i32  per   = total>0 ? (i32)(read*1000/total) : -1;
    if (per != a->shown) {
        SendMessageW(GetDlgItem(a->progress, ID_PBAR), PBM_SETPOS,
                     (uptr)MAX(per, 0), 0);
        a->shown = per;
    }
    c16  buf[96];
    wbuf b = wbuf_of(buf, countof(buf));
    wputsize(&b, (u64)read);
    if (total > 0) {
        wput(&b, L" of ");
        wputsize(&b, (u64)total);
    }
    SetDlgItemTextW(a->progress, ID_PBYTES, buf);
}

// What a status means, as zip and unzip report them, for a summary.
static c16 const *status_text(i32 who, i32 status)
{
    if (status == STATUS_STOPPED) {
        return L"Cancelled.";
    } else if (who == WHO_ZIP) {
        switch (status) {
        case ZE_OK:    return L"Done.";
        case ZE_EOF:   return L"The archive ended unexpectedly.";
        case ZE_FORM:  return L"The archive's structure is invalid.";
        case ZE_MEM:   return L"Not enough memory.";
        case ZE_TEMP:  return L"The temporary file failed.";
        case ZE_READ:  return L"The archive could not be read.";
        case ZE_NONE:  return L"Nothing to do.";
        case ZE_WRITE: return L"Writing the archive failed.";
        case ZE_CREAT: return L"The archive could not be created or "
                              L"replaced.";
        case ZE_PARMS: return L"That cannot be done.";
        case ZE_OPEN:  return L"Some files could not be read.";
        }
    } else {
        switch (status) {
        case PK_OK:     return L"Done.";
        case PK_WARN:   return L"Done, with warnings.";
        case PK_ERR:    return L"Some entries failed.";
        case PK_BADERR: return L"The archive is damaged.";
        case PK_MEM:    return L"Not enough memory.";
        case PK_NOZIP:  return L"This is not a zip archive, or it "
                               L"cannot be found.";
        case PK_FIND:   return L"The entries were not found: the archive "
                               L"may have changed.";
        case PK_BOMB:   return L"The archive's entries overlap (a possible "
                               L"zip bomb).";
        case PK_DISK:   return L"Writing failed (disk full?).";
        case PK_EOF:    return L"The archive ended unexpectedly.";
        case IZ_UNSUP:  return L"Some entries were skipped: encrypted, or "
                               L"compressed by an unsupported method.";
        }
    }
    return L"Failed.";
}

// The end of the job: its thread joined, its dialog closed, the window
// enabled, its log read, then its own end, after which its region goes,
// unless it was kept (a listing's, for its model).
static void job_end(app *a, job *j)
{
    WaitForSingleObject(j->thread, INFINITE);
    CloseHandle(j->thread);
    KillTimer(a->hwnd, TIMER_JOB);
    if (a->progress) {
        DestroyWindow(a->progress);
        a->progress = 0;
    }
    EnableWindow(a->hwnd, 1);
    SetForegroundWindow(a->hwnd);
    restore_focus(a);
    j->logtext = read_log(j, &j->perm);
    if (j->log) {
        CloseHandle(j->log);
        j->log = 0;
    }
    a->job = 0;
    if (j->done) {
        j->done(a, j);
    }
    if (!j->kept) {
        region_free(&j->h);
    }
}

// Report a job that did not go as planned, with its log.
static void job_report(app *a, job *j, c16 const *title)
{
    c16 const *summary = status_text(j->h.who, j->status);
    if (j->logtext[0]) {
        show_report(a, title, summary, j->logtext);
    } else {
        message(a, summary, MB_ICONWARNING);
    }
}


// Opening an archive: listed by a job, whose region becomes its model's

static i32 list_work(job *j)
{
    model      *m    = j->model;
    unzipconfig conf = {0};
    conf.perm    = j->perm;
    conf.scratch = host_scratch(&j->h);
    conf.windows = 1;
    unzipcall c = {0};
    c.archive = m->path;
    c.mode    = CALL_LIST;
    c.visit   = list_visit;
    c.data    = m;
    return unzip_call(&conf, &c);
}

typedef struct {
    u32 attributes;
    u32 created[2], accessed[2], written[2];
    u32 sizehi, sizelo;
} fileattrdata;

static b32 file_state(c16 const *path, i64 *size, u64 *written)
{
    fileattrdata d = {0};
    if (!GetFileAttributesExW(path, 0, &d)) {
        return 0;
    }
    *size    = (i64)((u64)d.sizehi<<32 | d.sizelo);
    *written = (u64)d.written[1]<<32 | d.written[0];
    return 1;
}

static void list_done(app *a, job *j)
{
    model *m  = j->model;
    s8     at = j->dest;  // the folder to show, if there
    if (j->status==STATUS_STOPPED || j->status==PK_MEM ||
        (j->status>PK_WARN && !m->nentries && !m->comment.len)) {
        if (j->status != STATUS_STOPPED) {
            job_report(a, j, L"Open");
        }
        if (a->m && j->reload) {
            show_model(a, 0, (s8){0});  // what was shown is gone
        }
        return;
    }
    if (j->status>PK_WARN && !m->damaged) {
        m->damaged = 1;
    }
    m->region = &j->h;
    j->kept   = 1;  // the model's now
    show_model(a, m, at);
}


// Opening, closing, and reloading

static void open_zip(app *a, s8 path, s8 at, b32 reload)
{
    if (a->job) {
        return;
    }
    job *j = job_new(a, WHO_UNZIP);
    if (!j) {
        return;
    }
    model *m = new(&j->perm, 1, model);
    m->region   = &j->h;
    m->path     = JOIN(&j->perm, path);
    m->wpath    = towide(&j->perm, path);
    m->lastdir  = &m->root;
    m->root.dir     = 1;
    m->root.sortkey = -1;
    iz cut = path.len;
    for (; cut>0 && path.s[cut-1]!='/' && path.s[cut-1]!='\\'; cut--) {}
    m->root.text = towide(&j->perm, (s8){path.s+cut, path.len-cut});
    m->root.name = S("");
    file_state(m->wpath, &m->size, &m->written);
    j->model  = m;
    j->work   = list_work;
    j->done   = list_done;
    j->title  = L"Opening";
    j->dest   = JOIN(&j->perm, at);
    j->reload = reload;
    job_start(a, j);
}

// The folder shown, as a path from the root, for a reload to return to.
static s8 folder_path(app *a, arena *perm)
{
    return a->folder && a->m ? node_path(a->folder, &a->m->root, perm)
                             : (s8){0};
}

static void reload(app *a)
{
    if (a->m) {
        arena *tmp  = &a->scratch;
        s8     path = JOIN(tmp, a->m->path);
        open_zip(a, path, folder_path(a, tmp), 1);
    }
}

// An archive's path from a dialog or the command line, made absolute.
static s8 full_path(arena *perm, c16 const *path)
{
    c16  buf[4];
    u32  len = GetFullPathNameW((c16 *)path, countof(buf), buf, 0);
    if (!len) {
        return towtf8(perm, (c16 *)path);
    }
    c16 *full = new(perm, len+1, c16);  // (garbage beside the result)
    GetFullPathNameW((c16 *)path, len+1, full, 0);
    return towtf8(perm, full);
}

static c16 const archive_filter[] =
    L"Zip archives (*.zip)\0*.zip\0All files (*.*)\0*.*\0";

static void cmd_open(app *a)
{
    c16 file[1024] = {0};
    openfilename o = {0};
    o.size    = (u32)sizeof(o);
    o.owner   = a->hwnd;
    o.filter  = archive_filter;
    o.file    = file;
    o.maxfile = countof(file);
    o.flags   = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o)) {
        open_zip(a, full_path(&a->scratch, file), (s8){0}, 0);
    }
}

static void cmd_close(app *a)
{
    show_model(a, 0, (s8){0});
}


// Questions from the worker: unzip's (ask) and zip's (replace), sent to
// the UI thread, which answers as a person would at the prompt.

enum { ASK_ZIPREPLACE = 100 };

typedef struct {
    i32 question;  // ASK_*
    s8  path;
    u8 *buf;       // for the answer, a line
    iz  cap;
    iz  reply;     // its length, or -1 for none
} asking;

static iz job_ask(void *data, i32 question, s8 path, u8 *buf, iz cap)
{
    job   *j = data;
    asking q = {question, path, buf, cap, -1};
    SendMessageW(j->a->hwnd, WM_APP_ASK, 0, (iz)&q);
    return q.reply;
}

static b32 job_replace(void *data, s8 name)
{
    job   *j = data;
    asking q = {ASK_ZIPREPLACE, name, 0, 0, 0};
    SendMessageW(j->a->hwnd, WM_APP_ASK, 0, (iz)&q);
    return q.reply > 0;
}

enum { ID_YESALL = 20, ID_NOALL, ID_CHOICE, ID_CHOSEN };

static b32 choice_command(dialog *d, i32 id, i32 code)
{
    (void)code;
    switch (id) {
    case IDYES: case IDNO: case ID_YESALL: case ID_NOALL:
        EndDialog(d->hwnd, id);
        return 1;
    }
    return 0;
}

// Yes, Yes to all, No, No to all, or Cancel, of a question about a path.
static i32 ask_choice(app *a, c16 const *title, c16 const *question,
                      s8 path, c16 const *after)
{
    arena *tmp = &a->scratch;
    dlgt   t   = dlg_begin(a, tmp, title, 300, 84, 0);
    dlg_label(&t, 7, 7, 286, 9, ID_CHOICE, question);
    c16 *shown = towide(tmp, path);
    for (c16 *c = shown; *c; c++) {
        *c = *c=='/' ? '\\' : *c;
    }
    dlg_item(&t, DLG_STATIC, 0, SS_LEFT|SS_NOPREFIX|SS_PATHELLIPSIS, 14, 20,
             279, 9, ID_CHOSEN, shown);
    dlg_label(&t, 7, 33, 286, 9, 0, after);
    dlg_button(&t,   7, 63, 50, IDYES,     L"&Yes", 1);
    dlg_button(&t,  62, 63, 50, ID_YESALL, L"Yes to &all", 0);
    dlg_button(&t, 117, 63, 50, IDNO,      L"&No", 0);
    dlg_button(&t, 172, 63, 50, ID_NOALL,  L"N&o to all", 0);
    dlg_button(&t, 243, 63, 50, IDCANCEL,  L"Cancel", 0);
    dialog d = {0};
    d.command = choice_command;
    return (i32)run_dialog(a, &t, &d);
}

static void cmd(app *a, i32 id);

static void run_cmd(app *a, i32 id, nodes s, node *base);

static void stop_job(app *a)
{
    if (a->job) {
        __atomic_store_n(&a->job->stop, 1, __ATOMIC_RELAXED);
    }
}

static void on_ask(app *a, asking *q)
{
    job *j = a->job;
    a->asking = 1;
    switch (q->question) {
    case ASK_REPLACE: {
        i32 c = ask_choice(a, L"Replace", L"This file already exists:",
                           q->path, L"Replace it with the archive's?");
        u8 r = c==IDYES ? 'y' : c==ID_YESALL ? 'A' : c==ID_NOALL ? 'N' : 'n';
        if (c == IDCANCEL) {
            stop_job(a);
        }
        if (q->cap >= 2) {
            q->buf[0] = r;
            q->buf[1] = '\n';
            q->reply  = 2;
        }
        break;
    }
    case ASK_DISK: {
        arena *tmp = &a->scratch;
        c16   *msg = towide(tmp, JOIN(tmp, S("Writing failed (disk full?):\n"),
                                      q->path, S("\n\nGo on with the rest?")));
        iptr   owner = a->progress ? a->progress : a->hwnd;
        b32    yes   = MessageBoxW(owner, msg, L"tugz", MB_YESNO |
                                   MB_ICONWARNING) == IDYES;
        if (q->cap >= 2) {
            q->buf[0] = yes ? 'y' : 'n';
            q->buf[1] = '\n';
            q->reply  = 2;
        }
        break;
    }
    case ASK_ZIPREPLACE:
        if (j && j->replaceall>=0) {
            q->reply = j->replaceall;
            break;
        }
        i32 c = ask_choice(a, L"Replace", L"The archive already has:",
                           q->path, L"Replace it with the file of that name?");
        q->reply = c==IDYES || c==ID_YESALL;
        if (j && (c==ID_YESALL || c==ID_NOALL)) {
            j->replaceall = c == ID_YESALL;
        } else if (c == IDCANCEL) {
            stop_job(a);
        }
        break;
    }
    a->asking = 0;
}


// Paths: wide, absolute, made long (\\?\) for the file system's calls

static c16 *longpath(arena *perm, s8 path)
{
    s8 unc = {0};
    if (path.len>2 && path.s[0]=='\\' && path.s[1]=='\\') {
        unc = (s8){path.s+1, path.len-1};  // \\host\share -> \\?\UNC\host...
        return towide(perm, JOIN(perm, S("\\\\?\\UNC"), unc));
    }
    return towide(perm, JOIN(perm, S("\\\\?\\"), path));
}

// Make a directory and its missing parents.
static b32 make_folders(c16 *path)
{
    if (CreateDirectoryW(path, 0) || GetFileAttributesW(path)!=(u32)-1) {
        return 1;
    }
    iz len = wlen(path);
    for (iz i = 7; i < len; i++) {  // past "\\?\X:\"
        if (path[i] == '\\') {
            path[i] = 0;
            CreateDirectoryW(path, 0);
            path[i] = '\\';
        }
    }
    return CreateDirectoryW(path, 0) || GetFileAttributesW(path)!=(u32)-1;
}

static void remove_tree(c16 *path, arena scratch)
{
    iz   len = wlen(path);
    c16 *pat = new(&scratch, len+3, c16);
    bytecopy(pat, path, len*(iz)sizeof(c16));
    pat[len] = '\\';
    pat[len+1] = '*';
    find_data fd = {0};
    iptr h = FindFirstFileExW(pat, 0, &fd, 0, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            iz n = wlen(fd.name);
            if ((n==1 && fd.name[0]=='.') ||
                (n==2 && fd.name[0]=='.' && fd.name[1]=='.')) {
                continue;
            }
            arena tmp   = scratch;
            c16  *child = new(&tmp, len+n+2, c16);
            bytecopy(child, path, len*(iz)sizeof(c16));
            child[len] = '\\';
            bytecopy(child+len+1, fd.name, n*(iz)sizeof(c16));
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            if (!(fd.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                DeleteFileW(child);
            } else if (fd.attributes & FILE_ATTRIBUTE_REPARSE) {
                RemoveDirectoryW(child);  // a link: not what it links to
            } else {
                remove_tree(child, tmp);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(path);
}

// A new directory within this session's temporary directory, which is
// made as needed, and removed at the end.
static s8 temp_dir(app *a, arena *perm)
{
    if (!a->temp.len) {
        c16  dir[300];
        wbuf b = wbuf_of(dir, countof(dir));
        b.len  = GetTempPathW(260, dir);
        wput(&b, L"tugz-");
        wputnum(&b, GetCurrentProcessId(), 0);
        a->temp = towtf8(&a->perm, dir);
        CreateDirectoryW(longpath(perm, a->temp), 0);
    }
    c16  num[16];
    wbuf b = wbuf_of(num, countof(num));
    wputnum(&b, (u64)++a->temps, 0);
    s8   dir = JOIN(perm, a->temp, S("\\"), towtf8(perm, num));
    CreateDirectoryW(longpath(perm, dir), 0);
    return dir;
}

static void remove_temp(app *a)
{
    if (a->temp.len) {
        arena tmp = a->scratch;
        remove_tree(longpath(&tmp, a->temp), tmp);
    }
}

// The archive's mark of the web, its Zone.Identifier stream, if any.
static s8 zone_of(app *a, arena *perm)
{
    s8   r    = {0};
    c16 *path = longpath(perm, JOIN(perm, a->m->path,
                                    S(":Zone.Identifier")));
    iptr h = CreateFileW(path, GENERIC_READ, FILE_SHARE_ALL, 0,
                         OPEN_EXISTING, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        u8 *buf = newbytes(perm, 1<<12);
        u32 got = 0;
        if (ReadFile(h, buf, 1<<12, &got, 0)) {
            r = (s8){buf, got};
        }
        CloseHandle(h);
    }
    return r;
}

// Mark a file that came from the archive as the archive is marked.
static void mark_file(s8 zone, s8 path, arena scratch)
{
    if (!zone.len) {
        return;
    }
    c16 *p = longpath(&scratch, JOIN(&scratch, path,
                                     S(":Zone.Identifier")));
    iptr h = CreateFileW(p, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        u32 n = 0;
        WriteFile(h, zone.s, (u32)zone.len, &n, 0);
        CloseHandle(h);
    }
}


// Picks: the entries chosen, in central directory order, each with the
// name to extract it as, relative to the folder shown (or its own name
// alone, flat), so a selection extracts as it is seen.

typedef struct {
    entry *e;
    s8     as;
} chosen;

static iz count_entries(node *n)
{
    iz r = !n->dir;
    for (entry *e = n->dir ? n->e : 0; e; e = e->same) {
        r++;
    }
    for (iz i = 0; n->dir && i<n->nkids; i++) {
        r += count_entries(n->kids[i]);
    }
    return r;
}

static void choose(chosen *v, iz *n, node *k, node *base, b32 flat,
                   arena *perm)
{
    if (!k->dir) {
        v[*n].e  = k->e;
        v[*n].as = flat ? k->name : node_path(k, base, perm);
        (*n)++;
        return;
    }
    if (!flat) {
        s8 as = JOIN(perm, node_path(k, base, perm), S("/"));
        for (entry *e = k->e; e; e = e->same) {
            v[*n].e  = e;
            v[*n].as = as;
            (*n)++;
        }
    }
    for (iz i = 0; i < k->nkids; i++) {
        choose(v, n, k->kids[i], base, flat, perm);
    }
}

static void sort_chosen(chosen *v, iz n, arena scratch)
{
    chosen *t = new(&scratch, n, chosen);
    for (iz w = 1; w < n; w *= 2) {
        for (iz lo = 0; lo < n; lo += 2*w) {
            iz mid = MIN(lo+w, n);
            iz hi  = MIN(lo+2*w, n);
            iz i = lo, j = mid, k = lo;
            while (i<mid || j<hi) {
                b32 right = i==mid || (j<hi && v[j].e->at<v[i].e->at);
                t[k++] = right ? v[j++] : v[i++];
            }
        }
        bytecopy(v, t, n*(iz)sizeof(*v));
    }
}

// Pick the nodes' entries, within the folder base, or with none,
// everything in it, into the job: its call's picks, named relative to
// base, their entries, and their bytes.
static void pick_nodes(job *j, nodes s, node *base, b32 flat)
{
    nodes  all  = s;
    if (!s.n) {
        all = (nodes){&base, 1};
    }
    iz cap = 0;
    for (iz i = 0; i < all.n; i++) {
        cap += count_entries(all.v[i]);
    }
    arena  *perm = &j->perm;
    chosen *v    = new(perm, cap, chosen);
    iz      n    = 0;
    for (iz i = 0; i < all.n; i++) {
        if (all.v[i] == base) {
            for (iz k = 0; k < base->nkids; k++) {
                choose(v, &n, base->kids[k], base, flat, perm);
            }
        } else {
            choose(v, &n, all.v[i], base, flat, perm);
        }
    }
    sort_chosen(v, n, j->perm);
    j->uc.picks  = new(perm, n, unzippick);
    j->uc.npicks = n;
    j->pentries  = new(perm, n, entry *);
    for (iz i = 0; i < n; i++) {
        j->uc.picks[i].at     = v[i].e->at;
        j->uc.picks[i].stored = v[i].e->stored;
        j->uc.picks[i].as     = v[i].as;
        j->pentries[i]        = v[i].e;
        j->total += v[i].e->csize;
    }
}

// Where a pick was extracted, as unzip maps its name below the directory.
static s8 picked_path(job *j, iz i, arena *perm)
{
    unzippick *p    = j->uc.picks + i;
    entry     *e    = j->pentries[i];
    uzpath     mp   = uz_mapname(p->as.s ? p->as : e->name, e->made, 0, perm);
    s8         path = JOIN(perm, j->uc.exdir, S("\\"), mp.path);
    for (iz k = 0; k < path.len; k++) {
        path.s[k] = path.s[k]=='/' ? '\\' : path.s[k];
    }
    return path;
}

static void mark_picks(job *j, arena scratch)
{
    for (iz i = 0; j->zone.len && i<j->uc.npicks; i++) {
        entry *e = j->pentries[i];
        if (j->uc.picks[i].done && !(e->name.len &&
                                     e->name.s[e->name.len-1]=='/')) {
            arena tmp = scratch;
            mark_file(j->zone, picked_path(j, i, &tmp), tmp);
        }
    }
}


// The engines' work, on the worker thread

static i32 unzip_work(job *j)
{
    unzipconfig conf = {0};
    conf.perm    = j->perm;
    conf.scratch = host_scratch(&j->h);
    conf.windows = 1;
    return unzip_call(&conf, &j->uc);
}

static i32 zip_work(job *j)
{
    zipconfig conf = {0};
    conf.perm    = j->perm;
    conf.scratch = host_scratch(&j->h);
    conf.windows = 1;
    return zip_call(&conf, &j->zc);
}

// A job on the archive shown, its path the job's own copy.
static job *archive_job(app *a, i32 who, c16 const *title)
{
    job *j = job_new(a, who);
    if (j) {
        j->title = title;
        j->work  = who==WHO_ZIP ? zip_work : unzip_work;
        s8 path  = JOIN(&j->perm, a->m->path);
        j->uc.archive = j->zc.archive = path;
        j->uc.ask     = job_ask;
        j->uc.data    = j;
        j->zc.data    = j;
        j->zc.level   = a->opt.level;
        j->zone       = zone_of(a, &j->perm);
    }
    return j;
}

// After zip changed the archive: what it said, if anything went wrong,
// then the archive again, at the folder that was shown.
static void zip_done(app *a, job *j)
{
    if (j->status!=ZE_OK && j->status!=STATUS_STOPPED) {
        job_report(a, j, j->title);
    }
    if (!a->m || s8equals(a->m->path, j->zc.archive)) {
        open_zip(a, j->zc.archive, j->dest, 1);
    }
}


// Extract, and Move: extract, then delete what was extracted

enum {
    ID_DEST = 10, ID_BROWSE, ID_FOLDERS, ID_EXISTING, ID_OPENAFTER,
};

typedef struct {
    s8   dest;
    b32  move;
} extract_data;

static b32 extract_init(dialog *d)
{
    app          *a = d->a;
    extract_data *x = d->data;
    arena        *tmp = &a->scratch;
    SetDlgItemTextW(d->hwnd, ID_DEST, towide(tmp, x->dest));
    dlg_check(d, ID_FOLDERS, !a->opt.flat);
    dlg_check(d, ID_OPENAFTER, a->opt.openafter);
    iptr box = GetDlgItem(d->hwnd, ID_EXISTING);
    SendMessageW(box, CB_ADDSTRING, 0, (iz)L"Ask");
    SendMessageW(box, CB_ADDSTRING, 0, (iz)L"Replace it");
    SendMessageW(box, CB_ADDSTRING, 0, (iz)L"Keep it, skipping the entry");
    SendMessageW(box, CB_SETCURSEL, (uptr)a->opt.overwrite, 0);
    return 1;
}

// A folder chosen in the shell's folder browser, or null.
static s8 browse_folder(app *a, iptr owner, c16 const *title, arena *perm)
{
    c16 name[300] = {0};
    browseinfo b = {0};
    b.owner       = owner;
    b.displayname = name;
    b.title       = title;
    b.flags       = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    void *list = SHBrowseForFolderW(&b);
    if (!list) {
        return (s8){0};
    }
    c16 path[300] = {0};
    b32 ok = SHGetPathFromIDListW(list, path);
    CoTaskMemFree(list);
    (void)a;
    return ok ? towtf8(perm, path) : (s8){0};
}

static b32 extract_command(dialog *d, i32 id, i32 code)
{
    (void)code;
    app   *a   = d->a;
    arena *tmp = &a->scratch;
    if (id == ID_BROWSE) {
        s8 dir = browse_folder(a, d->hwnd, L"Extract to:", tmp);
        if (dir.s) {
            SetDlgItemTextW(d->hwnd, ID_DEST, towide(tmp, dir));
        }
        return 1;
    } else if (id == IDOK) {
        extract_data *x = d->data;
        s8 dest = dlg_text(d, ID_DEST, &a->perm);
        for (; dest.len && (dest.s[dest.len-1]=='\\' ||
                            dest.s[dest.len-1]=='/'); dest.len--) {}
        if (!dest.len) {
            MessageBoxW(d->hwnd, L"Choose a folder to extract to.", L"tugz",
                        MB_OK | MB_ICONWARNING);
            return 1;
        }
        x->dest = full_path(&a->perm, towide(tmp, dest));
        a->opt.flat      = !dlg_checked(d, ID_FOLDERS);
        a->opt.openafter = dlg_checked(d, ID_OPENAFTER);
        a->opt.overwrite = (i32)SendMessageW(GetDlgItem(d->hwnd, ID_EXISTING),
                                             CB_GETCURSEL, 0, 0);
        a->opt.overwrite = MAX(a->opt.overwrite, 0);
        EndDialog(d->hwnd, IDOK);
        return 1;
    }
    return 0;
}

// The archive's path without its extension: the folder to extract to.
static s8 default_dest(app *a, arena *perm)
{
    s8 path = a->m->path;
    iz dot  = path.len;
    for (iz i = path.len-1; i>=0 && path.s[i]!='\\' && path.s[i]!='/'; i--) {
        if (path.s[i] == '.') {
            dot = i;
            break;
        }
    }
    s8 dest = {path.s, dot};
    return dot==path.len ? JOIN(perm, path, S(" files")) : JOIN(perm, dest);
}

static void extract_done(app *a, job *j);

static void cmd_extract(app *a, nodes s, node *base, b32 move)
{
    extract_data x = {0};
    x.dest = default_dest(a, &a->scratch);
    x.move = move;

    arena *tmp = &a->scratch;
    dlgt   t   = dlg_begin(a, tmp, move ? L"Move out" : L"Extract",
                           280, 112, 0);
    dlg_label(&t, 7, 7, 266, 9, 0, s.n ? L"&Extract the chosen items to:"
                                       : L"&Extract everything here to:");
    dlg_item(&t, DLG_EDIT, 0, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
             7, 18, 210, 13, ID_DEST, 0);
    dlg_button(&t, 222, 17, 51, ID_BROWSE, L"&Browse...", 0);
    dlg_item(&t, DLG_BUTTON, 0, BS_AUTOCHECKBOX | WS_TABSTOP, 7, 38, 266,
             10, ID_FOLDERS, L"Keep the &folders");
    dlg_label(&t, 7, 54, 90, 9, 0, L"If a file &exists:");
    dlg_item(&t, DLG_COMBOBOX, 0, CBS_DROPDOWNLIST | WS_VSCROLL |
             WS_TABSTOP, 100, 52, 173, 80, ID_EXISTING, 0);
    dlg_item(&t, DLG_BUTTON, 0, BS_AUTOCHECKBOX | WS_TABSTOP, 7, 71, 266,
             10, ID_OPENAFTER, L"&Open the folder when done");
    dlg_button(&t, 168, 92, 50, IDOK, move ? L"Move" : L"Extract", 1);
    dlg_button(&t, 223, 92, 50, IDCANCEL, L"Cancel", 0);
    dialog d = {0};
    d.init    = extract_init;
    d.command = extract_command;
    d.data    = &x;
    if (run_dialog(a, &t, &d) != IDOK) {
        return;
    }

    job *j = archive_job(a, WHO_UNZIP, move ? L"Moving" : L"Extracting");
    if (!j) {
        return;
    }
    j->uc.mode      = CALL_EXTRACT;
    j->uc.exdir     = JOIN(&j->perm, x.dest);
    j->uc.overwrite = a->opt.overwrite==1 ? OVERWRT_ALWAYS :
                      a->opt.overwrite==2 ? OVERWRT_NEVER  : OVERWRT_QUERY;
    j->move         = move;
    j->openafter    = a->opt.openafter;
    j->done         = extract_done;
    pick_nodes(j, s, base, a->opt.flat);
    if (!make_folders(longpath(&j->perm, x.dest))) {
        message(a, L"The folder could not be made.", MB_ICONERROR);
        region_free(&j->h);
        return;
    }
    job_start(a, j);
}

static void delete_entries(app *a, entry **v, iz n, s8 at);

static void extract_done(app *a, job *j)
{
    mark_picks(j, a->scratch);
    if (j->status == STATUS_STOPPED) {
        return;
    } else if (j->status != PK_OK) {
        job_report(a, j, j->move ? L"Move" : L"Extract");
    }
    if (j->openafter && j->status<=PK_WARN) {
        shellexecuteinfo x = {0};
        x.size = (u32)sizeof(x);
        x.hwnd = a->hwnd;
        x.file = towide(&a->scratch, j->uc.exdir);
        x.show = SW_SHOWNORMAL;
        ShellExecuteExW(&x);
    }
    if (j->move) {
        // Delete exactly what was extracted, nothing else
        arena  *tmp  = &a->scratch;
        entry **done = new(tmp, j->uc.npicks, entry *);
        iz      n    = 0;
        for (iz i = 0; i < j->uc.npicks; i++) {
            if (j->uc.picks[i].done) {
                done[n++] = j->pentries[i];
            }
        }
        if (n) {
            delete_entries(a, done, n, folder_path(a, tmp));
        }
    }
}


// Test

static void test_done(app *a, job *j)
{
    if (j->status == STATUS_STOPPED) {
        return;
    }
    c16 const *summary = j->status==PK_OK ? L"No errors were found."
                                          : status_text(WHO_UNZIP, j->status);
    show_report(a, L"Test", summary, j->logtext);
}

static void cmd_test(app *a, nodes s, node *base)
{
    job *j = archive_job(a, WHO_UNZIP, L"Testing");
    if (!j) {
        return;
    }
    j->uc.mode = CALL_TEST;
    j->done    = test_done;
    if (s.n || base!=&a->m->root) {
        pick_nodes(j, s, base, 0);
    } else {
        j->total = a->m->root.csize;  // all: UnZip's own summary
    }
    job_start(a, j);
}


// Delete, and Rename: zip's changes, by ordinal

static void collect(node *n, entry **v, iz *len)
{
    if (!n->dir) {
        v[(*len)++] = n->e;
        return;
    }
    for (entry *e = n->e; e; e = e->same) {
        v[(*len)++] = e;
    }
    for (iz i = 0; i < n->nkids; i++) {
        collect(n->kids[i], v, len);
    }
}

static void delete_entries(app *a, entry **v, iz n, s8 at)
{
    job *j = archive_job(a, WHO_ZIP, L"Deleting");
    if (!j) {
        return;
    }
    j->zc.changes  = new(&j->perm, n, zipchange);
    j->zc.nchanges = n;
    for (iz i = 0; i < n; i++) {
        j->zc.changes[i].ordinal = v[i]->ordinal;
        j->zc.changes[i].stored  = JOIN(&j->perm, v[i]->stored);
    }
    j->dest = JOIN(&j->perm, at);
    j->done = zip_done;
    job_start(a, j);
}

static void cmd_delete(app *a, nodes s)
{
    arena *tmp = &a->scratch;
    iz     cap = 0;
    for (iz i = 0; i < s.n; i++) {
        cap += count_entries(s.v[i]);
    }
    c16  buf[400];
    wbuf b = wbuf_of(buf, countof(buf));
    if (s.n == 1) {
        wput(&b, L"Delete \x201c");
        wput(&b, s.v[0]->text);
        wput(&b, s.v[0]->dir ? L"\x201d and everything in it" : L"\x201d");
    } else {
        wput(&b, L"Delete these ");
        wputnum(&b, (u64)s.n, a->sep);
        wput(&b, L" items");
    }
    wput(&b, L" from the archive?");
    if (MessageBoxW(a->hwnd, buf, L"Delete", MB_YESNO | MB_ICONWARNING |
                    MB_DEFBUTTON2) != IDYES) {
        return;
    }
    entry **v = new(tmp, cap, entry *);
    iz      n = 0;
    for (iz i = 0; i < s.n; i++) {
        collect(s.v[i], v, &n);
    }
    if (n) {
        delete_entries(a, v, n, folder_path(a, tmp));
    } else {
        message(a, L"There are no entries to delete.", MB_ICONINFORMATION);
    }
}

// A name for a file or folder, as it may be typed: something, and
// nothing a Windows name cannot have.
static b32 valid_name(c16 const *s)
{
    iz len = wlen(s);
    if (!len || (len==1 && s[0]=='.') || (len==2 && s[0]=='.' && s[1]=='.')) {
        return 0;
    }
    for (iz i = 0; i < len; i++) {
        switch (s[i]) {
        case '\\': case '/': case ':': case '*': case '?': case '"':
        case '<': case '>': case '|':
            return 0;
        }
        if (s[i] < 0x20) {
            return 0;
        }
    }
    return 1;
}

// Rename a node: a file's entry, or every entry within a folder, under
// its new path, all in one change.
static void rename_node(app *a, node *n, c16 *text)
{
    arena *tmp = &a->scratch;
    if (!text || !wlen(text)) {
        return;
    }
    if (!valid_name(text)) {
        message(a, L"A name cannot be empty, nor have any of these:\n"
                   L"\\ / : * ? \" < > |", MB_ICONWARNING);
        return;
    }
    s8 name = towtf8(tmp, text);
    if (s8equals(name, n->name)) {
        return;
    }
    for (iz i = 0; i < n->parent->nkids; i++) {
        node *k = n->parent->kids[i];
        if (k!=n && !lstrcmpiW(k->text, text)) {
            message(a, L"There is already an item of that name here.",
                    MB_ICONWARNING);
            return;
        }
    }

    iz      cap = count_entries(n);
    entry **v   = new(tmp, cap, entry *);
    iz      len = 0;
    collect(n, v, &len);
    if (!len) {
        return;  // a folder only in its entries' paths, now none
    }
    job *j = archive_job(a, WHO_ZIP, L"Renaming");
    if (!j) {
        return;
    }
    arena *perm   = &j->perm;
    s8     parent = node_path(n->parent, &a->m->root, perm);
    s8     head   = parent.len ? JOIN(perm, parent, S("/"), name) : name;
    j->zc.changes  = new(perm, len, zipchange);
    j->zc.nchanges = len;
    for (iz i = 0; i < len; i++) {
        entry *e = v[i];
        s8     newname = head;
        if (n->dir) {
            // Its path below the folder, as the tree has it, under the
            // folder's new name, and a folder's own entries end in '/'
            uzpath mp   = uz_mapname(e->name, e->made, UZ_KEEPVER, tmp);
            s8     path = mp.path;
            s8     old  = node_path(n, &a->m->root, tmp);
            s8     rest = {path.s+old.len, path.len-old.len};
            b32    own  = (mp.flags & UZ_DIR) && !rest.len;
            newname = JOIN(perm, head, rest, own ? S("/") : S(""));
        }
        j->zc.changes[i].ordinal = e->ordinal;
        j->zc.changes[i].stored  = JOIN(perm, e->stored);
        j->zc.changes[i].name    = newname;
    }
    // Show the folder renamed, if that was the one shown, or its parent
    s8 at = folder_path(a, tmp);
    s8 np = node_path(n, &a->m->root, tmp);
    if (n->dir && at.len>=np.len && s8equals((s8){at.s, np.len}, np) &&
        (at.len==np.len || at.s[np.len]=='/')) {
        at = JOIN(tmp, head, (s8){at.s+np.len, at.len-np.len});
    }
    j->dest = JOIN(perm, at);
    j->done = zip_done;
    job_start(a, j);
}

static void cmd_rename(app *a)
{
    i32 i = (i32)SendMessageW(a->list, LVM_GETNEXTITEM, (uptr)-1,
                              LVNI_SELECTED);
    if (i >= 0) {
        SetFocus(a->list);
        SendMessageW(a->list, LVM_EDITLABELW, (uptr)i, 0);
    }
}


// Add: files and folders, under the folder shown

enum { ID_LEVEL = 10, ID_MODE, ID_HIDDEN, ID_NOEXTRA, ID_NODIRS, ID_ASK,
       ID_ADDWHAT };

static b32 add_init(dialog *d)
{
    app *a = d->a;
    SetDlgItemTextW(d->hwnd, ID_ADDWHAT, d->data);
    iptr level = GetDlgItem(d->hwnd, ID_LEVEL);
    c16 const *levels[] = {
        L"Store (no compression)", L"Fastest", L"Normal", L"Maximum",
    };
    for (i32 i = 0; i < countof(levels); i++) {
        SendMessageW(level, CB_ADDSTRING, 0, (iz)levels[i]);
    }
    i32 l = a->opt.level;
    SendMessageW(level, CB_SETCURSEL, l==0 ? 0 : l<=2 ? 1 : l<=7 ? 2 : 3, 0);
    iptr mode = GetDlgItem(d->hwnd, ID_MODE);
    SendMessageW(mode, CB_ADDSTRING, 0, (iz)L"Add and replace");
    SendMessageW(mode, CB_ADDSTRING, 0, (iz)L"Add, and replace if newer");
    SendMessageW(mode, CB_ADDSTRING, 0, (iz)L"Only replace, if newer");
    SendMessageW(mode, CB_SETCURSEL, (uptr)a->opt.mode, 0);
    dlg_check(d, ID_HIDDEN,  a->opt.hidden);
    dlg_check(d, ID_NOEXTRA, a->opt.noextra);
    dlg_check(d, ID_NODIRS,  a->opt.nodirs);
    dlg_check(d, ID_ASK,     a->opt.askreplace);
    return 1;
}

static b32 add_command(dialog *d, i32 id, i32 code)
{
    (void)code;
    if (id == IDOK) {
        app *a = d->a;
        i32  l = (i32)SendMessageW(GetDlgItem(d->hwnd, ID_LEVEL),
                                   CB_GETCURSEL, 0, 0);
        i32  levels[] = {0, 1, 6, 9};
        a->opt.level      = levels[MIN(MAX(l, 0), 3)];
        a->opt.mode       = (i32)SendMessageW(GetDlgItem(d->hwnd, ID_MODE),
                                              CB_GETCURSEL, 0, 0);
        a->opt.mode       = MIN(MAX(a->opt.mode, 0), 2);
        a->opt.hidden     = dlg_checked(d, ID_HIDDEN);
        a->opt.noextra    = dlg_checked(d, ID_NOEXTRA);
        a->opt.nodirs     = dlg_checked(d, ID_NODIRS);
        a->opt.askreplace = dlg_checked(d, ID_ASK);
        EndDialog(d->hwnd, IDOK);
        return 1;
    }
    return 0;
}

static b32 add_dialog(app *a, c16 const *what)
{
    arena *tmp = &a->scratch;
    dlgt   t   = dlg_begin(a, tmp, L"Add", 260, 134, 0);
    dlg_item(&t, DLG_STATIC, 0, SS_LEFT|SS_NOPREFIX|SS_PATHELLIPSIS, 7, 7,
             246, 9, ID_ADDWHAT, 0);
    dlg_label(&t, 7, 24, 70, 9, 0, L"&Compression:");
    dlg_item(&t, DLG_COMBOBOX, 0, CBS_DROPDOWNLIST | WS_VSCROLL |
             WS_TABSTOP, 80, 22, 173, 80, ID_LEVEL, 0);
    dlg_label(&t, 7, 41, 70, 9, 0, L"&Mode:");
    dlg_item(&t, DLG_COMBOBOX, 0, CBS_DROPDOWNLIST | WS_VSCROLL |
             WS_TABSTOP, 80, 39, 173, 80, ID_MODE, 0);
    dlg_item(&t, DLG_BUTTON, 0, BS_AUTOCHECKBOX | WS_TABSTOP, 7, 58, 246,
             10, ID_ASK, L"As&k before replacing an entry");
    dlg_item(&t, DLG_BUTTON, 0, BS_AUTOCHECKBOX | WS_TABSTOP, 7, 71, 246,
             10, ID_HIDDEN, L"Include &hidden and system files in folders");
    dlg_item(&t, DLG_BUTTON, 0, BS_AUTOCHECKBOX | WS_TABSTOP, 7, 84, 246,
             10, ID_NOEXTRA, L"Leave out e&xtra attributes (times, owners)");
    dlg_item(&t, DLG_BUTTON, 0, BS_AUTOCHECKBOX | WS_TABSTOP, 7, 97, 246,
             10, ID_NODIRS, L"Leave out &folder entries");
    dlg_button(&t, 148, 114, 50, IDOK, L"Add", 1);
    dlg_button(&t, 203, 114, 50, IDCANCEL, L"Cancel", 0);
    dialog d = {0};
    d.init    = add_init;
    d.command = add_command;
    d.data    = (void *)what;
    return run_dialog(a, &t, &d) == IDOK;
}

// A new archive's path, from a save dialog, or null.
static s8 new_archive(app *a, arena *perm)
{
    c16 file[1024] = L"New archive.zip";
    openfilename o = {0};
    o.size    = (u32)sizeof(o);
    o.owner   = a->hwnd;
    o.filter  = archive_filter;
    o.file    = file;
    o.maxfile = countof(file);
    o.defext  = L"zip";
    o.title   = L"New archive";
    o.flags   = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
    return GetSaveFileNameW(&o) ? full_path(perm, file) : (s8){0};
}

// New: an empty archive (an end record alone), made, then opened. The
// save dialog asked before replacing a file.
static void cmd_new(app *a)
{
    arena *tmp  = &a->scratch;
    s8     path = new_archive(a, tmp);
    if (!path.s) {
        return;
    }
    static u8 const empty[22] = {'P', 'K', 5, 6};
    iptr h = CreateFileW(longpath(tmp, path), GENERIC_WRITE, 0, 0,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    u32  n = 0;
    b32  ok = h!=INVALID_HANDLE_VALUE &&
              WriteFile(h, (u8 *)empty, sizeof(empty), &n, 0) &&
              n==sizeof(empty);
    if (h != INVALID_HANDLE_VALUE) {
        ok &= CloseHandle(h);
    }
    if (!ok) {
        message(a, L"The archive could not be made.", MB_ICONERROR);
        return;
    }
    open_zip(a, path, (s8){0}, 0);
}

// Add files and folders, by their absolute paths, to a folder, or to a
// new archive if none is open.
static void add_paths(app *a, node *into, s8 *paths, iz n)
{
    arena *tmp = &a->scratch;
    if (!n) {
        return;
    }
    s8 archive = a->m ? a->m->path : new_archive(a, tmp);
    if (!archive.s) {
        return;
    }
    // Into: shown as where it goes
    c16  what[400];
    wbuf b = wbuf_of(what, countof(what));
    wput(&b, L"Add ");
    if (n == 1) {
        iz cut = paths[0].len;
        for (; cut>0 && paths[0].s[cut-1]!='\\'; cut--) {}
        wput(&b, L"\x201c");
        wput8(&b, (s8){paths[0].s+cut, paths[0].len-cut});
        wput(&b, L"\x201d");
    } else {
        wputnum(&b, (u64)n, a->sep);
        wput(&b, L" items");
    }
    wput(&b, L" to ");
    s8 prefix = a->m&&into ? node_path(into, &a->m->root, tmp) : (s8){0};
    if (a->m) {
        wput(&b, a->m->root.text);
        if (prefix.len) {
            wputc(&b, '\\');
            wput8(&b, prefix);
        }
    } else {
        iz cut = archive.len;
        for (; cut>0 && archive.s[cut-1]!='\\'; cut--) {}
        wput8(&b, (s8){archive.s+cut, archive.len-cut});
    }
    if (!add_dialog(a, what)) {
        return;
    }

    job *j = job_new(a, WHO_ZIP);
    if (!j) {
        return;
    }
    arena *perm = &j->perm;
    j->title       = L"Adding";
    j->work        = zip_work;
    j->done        = zip_done;
    j->zc.archive  = JOIN(perm, archive);
    j->zc.level    = a->opt.level;
    j->zc.mode     = a->opt.mode;
    j->zc.hidden   = a->opt.hidden;
    j->zc.noextra  = a->opt.noextra;
    j->zc.nodirs   = a->opt.nodirs;
    j->zc.replace  = a->opt.askreplace ? job_replace : 0;
    j->zc.data     = j;
    j->zc.sources  = new(perm, n, zipsource);
    j->zc.nsources = n;
    for (iz i = 0; i < n; i++) {
        s8 path = paths[i];
        for (; path.len>3 && path.s[path.len-1]=='\\'; path.len--) {}
        iz cut = path.len;
        for (; cut>0 && path.s[cut-1]!='\\' && path.s[cut-1]!=':'; cut--) {}
        s8 base = {path.s+cut, path.len-cut};
        j->zc.sources[i].path = JOIN(perm, path);
        j->zc.sources[i].name = prefix.len ? JOIN(perm, prefix, S("/"), base)
                                           : JOIN(perm, base);
    }
    j->dest = JOIN(perm, prefix);
    job_start(a, j);
}

static void cmd_addfiles(app *a, node *into)
{
    arena *tmp = &a->scratch;
    iz     cap = 1 << 16;
    c16   *buf = new(tmp, cap, c16);
    openfilename o = {0};
    o.size    = (u32)sizeof(o);
    o.owner   = a->hwnd;
    o.filter  = L"All files (*.*)\0*.*\0";
    o.file    = buf;
    o.maxfile = (u32)cap;
    o.title   = L"Add files";
    o.flags   = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT |
                OFN_NODEREFERENCELINKS | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&o)) {
        return;
    }
    // One path, or a directory then names, each null-terminated
    iz  n   = 0;
    s8 *v   = new(tmp, cap/2, s8);
    c16 *dir = buf;
    c16 *p   = buf + wlen(buf) + 1;
    if (!*p) {
        v[n++] = full_path(tmp, buf);
    }
    for (; *p; p += wlen(p) + 1) {
        s8 d = towtf8(tmp, dir);
        b32 sep = d.len && d.s[d.len-1]=='\\';
        v[n++] = JOIN(tmp, d, sep ? S("") : S("\\"), towtf8(tmp, p));
    }
    add_paths(a, into, v, n);
}

static void cmd_addfolder(app *a, node *into)
{
    arena *tmp = &a->scratch;
    s8 dir = browse_folder(a, a->hwnd, L"Add a folder:", tmp);
    if (dir.s) {
        add_paths(a, into, &dir, 1);
    }
}


// Open, Open with, and Edit: an entry extracted to a temporary folder

static void open_done(app *a, job *j);

// A file's CRC-32, to tell whether an editor changed it.
static u32 file_crc(c16 *path, arena scratch)
{
    u32  crc = 0;
    i32  cpu = 0;
    iptr h   = CreateFileW(path, GENERIC_READ, FILE_SHARE_ALL, 0,
                           OPEN_EXISTING, 0, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    u8 *buf = newbytes(&scratch, 1<<16);
    for (u32 n = 0; ReadFile(h, buf, 1<<16, &n, 0) && n;) {
        crc = crc32_update(crc, buf, n, &cpu);
    }
    CloseHandle(h);
    return crc;
}

static void extract_temp(app *a, node *n, i32 then)
{
    if (!n || n->dir || !n->e) {
        return;
    }
    job *j = archive_job(a, WHO_UNZIP, L"Opening");
    if (!j) {
        return;
    }
    j->uc.mode      = CALL_EXTRACT;
    j->uc.exdir     = temp_dir(a, &j->perm);
    j->uc.overwrite = OVERWRT_ALWAYS;
    j->then         = then;
    j->entry        = n->e;
    j->done         = open_done;
    nodes s = {&n, 1};
    pick_nodes(j, s, n->parent, 1);
    job_start(a, j);
}

static void edit_start(app *a, s8 path, entry *e);

static void open_done(app *a, job *j)
{
    if (j->status == STATUS_STOPPED) {
        return;
    } else if (!j->uc.npicks || !j->uc.picks[0].done) {
        job_report(a, j, L"Open");
        return;
    }
    mark_picks(j, a->scratch);
    s8 path = picked_path(j, 0, &a->scratch);
    if (j->then == THEN_EDIT) {
        edit_start(a, path, j->entry);
        return;
    }
    shellexecuteinfo x = {0};
    x.size = (u32)sizeof(x);
    x.mask = SEE_MASK_INVOKEIDLIST * (j->then == THEN_OPENWITH);
    x.hwnd = a->hwnd;
    x.verb = j->then==THEN_OPENWITH ? L"openas" : 0;
    x.file = towide(&a->scratch, path);
    x.show = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&x) && j->then!=THEN_OPENWITH) {
        // No program for it: ask which
        x.verb = L"openas";
        x.mask = SEE_MASK_INVOKEIDLIST;
        ShellExecuteExW(&x);
    }
}

// Edit: the configured command run on the file, the window free
// meanwhile, a bar above the panes saying what is being edited. A timer
// watches each editor; once one exits 0 having changed its file, the
// entry is replaced as soon as no other job runs. An editor that exits
// at once, unchanged, has likely handed the file to another program
// (VS Code, Office): the bar then waits for Update. Stop waiting forgets
// them all, and the editors run on.

typedef struct edit edit;
struct edit {
    host  h;          // its own region, freed with it
    edit *next;
    iptr  process;    // the editor's, until it exits
    s8    archive;
    s8    path;       // the temporary file
    s8    name;       // its entry's name
    c16  *shown;      // the file's own name, for the bar
    i64   size0;      // the file as it began
    u64   written0;
    u32   crc0;
    u32   began;
    b32   handed;     // returned at once, unchanged: waiting on Update
    b32   ready;      // changed: to replace the entry once free
};

// The command line: the configured command, %1 replaced by the quoted
// path, or the path appended.
static c16 *edit_command(app *a, s8 path, arena *perm)
{
    c16 *file = towide(perm, path);
    iz   cap  = wlen(a->opt.edit) + 8*(wlen(file)+2) + 8;
    c16 *cmd  = new(perm, cap, c16);
    wbuf b    = wbuf_of(cmd, cap);
    b32  put  = 0;
    for (c16 *p = a->opt.edit; *p; p++) {
        if (p[0]=='%' && p[1]=='1') {
            b32 quoted = p>a->opt.edit && p[-1]=='"';
            if (!quoted) wputc(&b, '"');
            wput(&b, file);
            if (!quoted) wputc(&b, '"');
            put = 1;
            p++;
        } else {
            wputc(&b, *p);
        }
    }
    if (!put) {
        wput(&b, L" \"");
        wput(&b, file);
        wputc(&b, '"');
    }
    return cmd;
}

static void layout(app *a);

// The bar's text and buttons, as the edits stand.
static void show_edits(app *a)
{
    iz   n      = 0;
    b32  handed = 0;
    for (edit *e = a->edits; e; e = e->next) {
        n++;
        handed |= e->handed;
    }
    c16  buf[512];
    wbuf b = wbuf_of(buf, countof(buf));
    if (n) {
        edit *e = a->edits;  // the latest
        for (edit *h = a->edits; handed && h; h = h->next) {
            e = h->handed ? h : e;
        }
        wput(&b, handed ? L"\x201c" : L"Editing \x201c");
        wput(&b, e->shown);
        wput(&b, handed ? L"\x201d was opened in another program: once "
                          L"it is saved, choose Update."
                        : L"\x201d: the archive is updated when its "
                          L"editor closes.");
        if (n > 1) {
            wput(&b, L" (and ");
            wputnum(&b, (u64)(n-1), a->sep);
            wput(&b, n==2 ? L" more file)" : L" more files)");
        }
    }
    SetWindowTextW(a->editbar, buf);
    i32 show = n ? SW_SHOW : SW_HIDE;
    ShowWindow(a->editbar, show);
    ShowWindow(a->editstop, show);
    ShowWindow(a->editupdate, handed ? SW_SHOW : SW_HIDE);
    layout(a);
}

static void edit_remove(app *a, edit *e)
{
    for (edit **p = &a->edits; *p; p = &(*p)->next) {
        if (*p == e) {
            *p = e->next;
            break;
        }
    }
    if (e->process) {
        CloseHandle(e->process);
    }
    region_free(&e->h);
}

static b32 edit_changed(edit *e, arena scratch)
{
    c16 *file    = towide(&scratch, e->path);
    i64  size    = 0;
    u64  written = 0;
    file_state(file, &size, &written);
    return size!=e->size0 || written!=e->written0 ||
           file_crc(file, scratch)!=e->crc0;
}

static void edit_start(app *a, s8 path, entry *e)
{
    arena *tmp  = &a->scratch;
    c16   *file = towide(tmp, path);
    SetFileAttributesW(file, FILE_ATTRIBUTE_NORMAL);  // not read-only

    startupinfo si = {0};
    si.cb = (u32)sizeof(si);
    processinfo pi = {0};
    c16 *cmd = edit_command(a, path, tmp);
    if (!CreateProcessW(0, cmd, 0, 0, 0, 0, 0, 0, &si, &pi)) {
        c16  buf[400];
        wbuf b = wbuf_of(buf, countof(buf));
        wput(&b, L"The editor could not be run:\n\n");
        wput(&b, a->opt.edit);
        wput(&b, L"\n\nChoose another in Tools \x2192 Options.");
        message(a, buf, MB_ICONERROR);
        return;
    }
    CloseHandle(pi.thread);

    edit *d = region_new((iz)1<<24, (iz)sizeof(edit));
    if (!d) {
        CloseHandle(pi.process);
        message(a, L"Not enough memory.", MB_ICONERROR);
        return;
    }
    arena perm = host_perm(&d->h);
    d->process = pi.process;
    d->archive = JOIN(&perm, a->m->path);
    d->path    = JOIN(&perm, path);
    d->name    = JOIN(&perm, e->name);
    iz cut = path.len;
    for (; cut>0 && path.s[cut-1]!='\\'; cut--) {}
    d->shown   = towide(&perm, (s8){path.s+cut, path.len-cut});
    d->began   = GetTickCount();
    file_state(file, &d->size0, &d->written0);
    d->crc0    = file_crc(file, *tmp);
    d->next    = a->edits;
    a->edits   = d;
    SetTimer(a->hwnd, TIMER_EDIT, 250, 0);
    show_edits(a);
}

// Replace an edited entry, the archive shown again if it is the one.
static void edit_update(app *a, edit *e)
{
    job *z = job_new(a, WHO_ZIP);
    if (!z) {
        return;
    }
    arena *perm = &z->perm;
    z->title       = L"Updating";
    z->work        = zip_work;
    z->done        = zip_done;
    z->zc.archive  = JOIN(perm, e->archive);
    z->zc.level    = a->opt.level;
    z->zc.data     = z;
    z->zc.sources  = new(perm, 1, zipsource);
    z->zc.nsources = 1;
    z->zc.sources[0].path = JOIN(perm, e->path);
    z->zc.sources[0].name = JOIN(perm, e->name);
    z->dest = folder_path(a, perm);
    job_start(a, z);
}

// Watch the editors: which exited, how, and what to update.
static void edit_tick(app *a)
{
    if (a->ticking) {
        return;  // (within a message box of its own)
    }
    a->ticking = 1;
    b32 changed = 0;
    for (edit *e = a->edits, *next; e; e = next) {
        next = e->next;
        if (!e->process || WaitForSingleObject(e->process, 0)!=WAIT_OBJECT_0) {
            continue;
        }
        u32 status = 0;
        GetExitCodeProcess(e->process, &status);
        CloseHandle(e->process);
        e->process = 0;
        changed = 1;
        if (status) {
            c16  buf[400];
            wbuf b = wbuf_of(buf, countof(buf));
            wput(&b, L"The editor of \x201c");
            wput(&b, e->shown);
            wput(&b, L"\x201d ended with status ");
            wputnum(&b, status, 0);
            wput(&b, L", so its entry is unchanged.");
            edit_remove(a, e);
            show_edits(a);
            message(a, buf, MB_ICONWARNING);
        } else if (edit_changed(e, a->scratch)) {
            e->ready = 1;
        } else if (GetTickCount()-e->began < 3000) {
            e->handed = 1;
        } else {
            edit_remove(a, e);  // closed unchanged
        }
    }

    // One update at a time, as jobs go
    b32 waiting = 0;
    for (edit *e = a->edits; e; e = e->next) {
        waiting |= e->process || e->ready;
    }
    if (!a->job) {
        for (edit *e = a->edits; e; e = e->next) {
            if (e->ready) {
                edit_update(a, e);
                edit_remove(a, e);
                changed = 1;
                break;
            }
        }
    }
    if (!waiting) {
        KillTimer(a->hwnd, TIMER_EDIT);
    }
    if (changed) {
        show_edits(a);
    }
    a->ticking = 0;
}

// Update: the files handed to other programs, as they are saved now.
static void edit_handed(app *a)
{
    b32 any = 0;
    for (edit *e = a->edits; e; e = e->next) {
        if (e->handed && edit_changed(e, a->scratch)) {
            e->handed = 0;
            e->ready  = any = 1;
        }
    }
    if (!any) {
        message(a, L"Nothing has changed yet: save the file first.",
                MB_ICONINFORMATION);
        return;
    }
    SetTimer(a->hwnd, TIMER_EDIT, 250, 0);
    edit_tick(a);
}

static void edit_stop(app *a)
{
    while (a->edits) {
        edit_remove(a, a->edits);
    }
    KillTimer(a->hwnd, TIMER_EDIT);
    show_edits(a);
}


// Properties: ZipInfo's report of the archive, and of an entry

static void properties_done(app *a, job *j)
{
    if (j->status == STATUS_STOPPED) {
        return;
    }
    c16 const *name = a->m->root.text;
    if (j->entry) {
        name = towide(&a->scratch, j->entry->name);
    }
    show_report(a, L"Properties", name, j->logtext);
}

static void cmd_properties(app *a, nodes s)
{
    job *j = archive_job(a, WHO_UNZIP, L"Reading");
    if (!j) {
        return;
    }
    j->uc.mode = CALL_ZIPINFO;
    j->done    = properties_done;
    node *n = s.n==1 ? s.v[0] : 0;
    if (n && (!n->dir || n->e)) {
        entry *e = n->e;
        j->entry     = e;
        j->uc.picks  = new(&j->perm, 1, unzippick);
        j->uc.npicks = 1;
        j->uc.picks[0].at     = e->at;
        j->uc.picks[0].stored = e->stored;
    } else {
        j->uc.picks  = new(&j->perm, 1, unzippick);  // none: the archive
        j->uc.npicks = 0;
    }
    job_start(a, j);
}


// Options: the editor's command

enum { ID_EDITCMD = 10 };

static b32 options_init(dialog *d)
{
    SetDlgItemTextW(d->hwnd, ID_EDITCMD, d->a->opt.edit);
    return 1;
}

static b32 options_command(dialog *d, i32 id, i32 code)
{
    (void)code;
    if (id == IDOK) {
        app *a = d->a;
        GetDlgItemTextW(d->hwnd, ID_EDITCMD, a->opt.edit, countof(a->opt.edit));
        if (!a->opt.edit[0]) {
            wbuf b = wbuf_of(a->opt.edit, countof(a->opt.edit));
            wput(&b, L"notepad.exe");
        }
        EndDialog(d->hwnd, IDOK);
        return 1;
    }
    return 0;
}

static void cmd_options(app *a)
{
    arena *tmp = &a->scratch;
    dlgt   t   = dlg_begin(a, tmp, L"Options", 280, 76, 0);
    dlg_label(&t, 7, 7, 266, 9, 0, L"&Edit runs this command, %1 the file:");
    dlg_item(&t, DLG_EDIT, 0, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
             7, 18, 266, 13, ID_EDITCMD, 0);
    dlg_label(&t, 7, 35, 266, 18, 0, L"Once it exits with status 0, a "
              L"changed file replaces the entry.");
    dlg_button(&t, 168, 56, 50, IDOK, L"OK", 1);
    dlg_button(&t, 223, 56, 50, IDCANCEL, L"Cancel", 0);
    dialog d = {0};
    d.init    = options_init;
    d.command = options_command;
    run_dialog(a, &t, &d);
}

static void cmd_about(app *a)
{
    MessageBoxW(a->hwnd, L"tugz " TUGZ_VERSION L"\n\nZip archives, by the "
                L"zip and unzip of tugz.\n\nThis is free and unencumbered "
                L"software released into the public domain.", L"About tugz",
                MB_OK | MB_ICONINFORMATION);
}


// Menus: the bar's, and the context menu, enabled as they open

static iptr make_menu(void)
{
    iptr bar  = CreateMenu();
    iptr file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, CMD_NEW,   L"&New archive...");
    AppendMenuW(file, MF_STRING, CMD_OPEN,  L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, CMD_CLOSE, L"&Close\tCtrl+W");
    AppendMenuW(file, MF_SEPARATOR, 0, 0);
    AppendMenuW(file, MF_STRING, CMD_PROPERTIES,
                L"P&roperties\tAlt+Enter");
    AppendMenuW(file, MF_SEPARATOR, 0, 0);
    AppendMenuW(file, MF_STRING, CMD_EXIT,  L"E&xit");
    AppendMenuW(bar, MF_POPUP, (uptr)file, L"&File");

    iptr edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, CMD_ADDFILES,  L"&Add files...\tIns");
    AppendMenuW(edit, MF_STRING, CMD_ADDFOLDER, L"Add f&older...");
    AppendMenuW(edit, MF_SEPARATOR, 0, 0);
    AppendMenuW(edit, MF_STRING, CMD_EXTRACT,   L"&Extract...\tCtrl+E");
    AppendMenuW(edit, MF_STRING, CMD_MOVE,      L"&Move out...");
    AppendMenuW(edit, MF_STRING, CMD_TEST,      L"&Test\tCtrl+T");
    AppendMenuW(edit, MF_SEPARATOR, 0, 0);
    AppendMenuW(edit, MF_STRING, CMD_OPENENTRY, L"O&pen\tEnter");
    AppendMenuW(edit, MF_STRING, CMD_OPENWITH,  L"Open &with...");
    AppendMenuW(edit, MF_STRING, CMD_EDIT,      L"E&dit\tF4");
    AppendMenuW(edit, MF_SEPARATOR, 0, 0);
    AppendMenuW(edit, MF_STRING, CMD_RENAME,    L"&Rename\tF2");
    AppendMenuW(edit, MF_STRING, CMD_DELETE,    L"De&lete\tDel");
    AppendMenuW(edit, MF_SEPARATOR, 0, 0);
    AppendMenuW(edit, MF_STRING, CMD_SELECTALL, L"Select a&ll\tCtrl+A");
    AppendMenuW(bar, MF_POPUP, (uptr)edit, L"&Edit");

    iptr view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, CMD_UP,      L"&Up\tAlt+Up");
    AppendMenuW(view, MF_STRING, CMD_REFRESH, L"&Refresh\tF5");
    AppendMenuW(bar, MF_POPUP, (uptr)view, L"&View");

    iptr tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, CMD_OPTIONS, L"&Options...");
    AppendMenuW(bar, MF_POPUP, (uptr)tools, L"&Tools");

    iptr help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, CMD_ABOUT, L"&About tugz");
    AppendMenuW(bar, MF_POPUP, (uptr)help, L"&Help");
    return bar;
}

static iptr make_accelerators(void)
{
    typedef struct { u16 flags, key, cmd; } accel;  // ACCEL
    static accel const table[] = {
        {FVIRTKEY|FCONTROL, 'O',       CMD_OPEN},
        {FVIRTKEY|FCONTROL, 'W',       CMD_CLOSE},
        {FVIRTKEY|FCONTROL, 'E',       CMD_EXTRACT},
        {FVIRTKEY|FCONTROL, 'T',       CMD_TEST},
        {FVIRTKEY,          VK_F4,     CMD_EDIT},
        {FVIRTKEY,          VK_F5,     CMD_REFRESH},
        {FVIRTKEY,          VK_INSERT, CMD_ADDFILES},
        {FVIRTKEY|FALT,     VK_RETURN, CMD_PROPERTIES},
        {FVIRTKEY|FALT,     VK_UP,     CMD_UP},
    };
    return CreateAcceleratorTableW((void *)table, countof(table));
}

static void enable_menu(app *a, iptr menu)
{
    arena *tmp = &a->scratch;
    nodes  s   = selection(a, tmp);
    for (i32 id = CMD_NEW; id <= CMD_ABOUT; id++) {
        EnableMenuItem(menu, (u32)id, command_on(a, id, s) && !a->job
                                      ? MF_ENABLED : MF_GRAYED);
    }
}

static void context_menu(app *a, i32 x, i32 y)
{
    arena *tmp = &a->scratch;
    nodes  s   = selection(a, tmp);
    if (x==-1 && y==-1) {  // the keyboard's: at the focused item
        i32  i = (i32)SendMessageW(a->list, LVM_GETNEXTITEM, (uptr)-1,
                                   LVNI_SELECTED);
        rect r = {0};
        r.left = LVIR_LABEL;
        if (i>=0 && SendMessageW(a->list, LVM_GETITEMRECT, (uptr)i, (iz)&r)) {
            point p = {r.left, r.bottom};
            ClientToScreen(a->list, &p);
            x = p.x;
            y = p.y;
        } else {
            point p = {0, 0};
            ClientToScreen(a->list, &p);
            x = p.x;
            y = p.y;
        }
    }
    iptr menu = CreatePopupMenu();
    if (s.n) {
        b32 dir = s.n==1 && s.v[0]->dir;
        AppendMenuW(menu, MF_STRING, CMD_OPENENTRY, L"&Open");
        if (!dir) {
            AppendMenuW(menu, MF_STRING, CMD_OPENWITH, L"Open &with...");
            AppendMenuW(menu, MF_STRING, CMD_EDIT,     L"E&dit");
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, 0);
        AppendMenuW(menu, MF_STRING, CMD_EXTRACT, L"&Extract...");
        AppendMenuW(menu, MF_STRING, CMD_MOVE,    L"&Move out...");
        AppendMenuW(menu, MF_STRING, CMD_TEST,    L"&Test");
        AppendMenuW(menu, MF_SEPARATOR, 0, 0);
        AppendMenuW(menu, MF_STRING, CMD_RENAME,  L"&Rename");
        AppendMenuW(menu, MF_STRING, CMD_DELETE,  L"De&lete");
        AppendMenuW(menu, MF_SEPARATOR, 0, 0);
        AppendMenuW(menu, MF_STRING, CMD_PROPERTIES, L"&Properties");
    } else {
        AppendMenuW(menu, MF_STRING, CMD_ADDFILES,  L"&Add files...");
        AppendMenuW(menu, MF_STRING, CMD_ADDFOLDER, L"Add f&older...");
        AppendMenuW(menu, MF_SEPARATOR, 0, 0);
        AppendMenuW(menu, MF_STRING, CMD_EXTRACT,   L"&Extract...");
        AppendMenuW(menu, MF_STRING, CMD_TEST,      L"&Test");
        AppendMenuW(menu, MF_SEPARATOR, 0, 0);
        AppendMenuW(menu, MF_STRING, CMD_SELECTALL, L"Select a&ll");
        AppendMenuW(menu, MF_STRING, CMD_PROPERTIES, L"&Properties");
    }
    enable_menu(a, menu);
    i32 c = (i32)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y,
                                0, a->hwnd, 0);
    DestroyMenu(menu);
    if (c) {
        cmd(a, c);
    }
}

// The tree's own commands, on a folder there (or the archive, at its
// root), wherever the folder shown is: as if chosen in its parent.

static node *tree_node(app *a, iptr item)
{
    tvitem it = {0};
    it.mask = TVIF_PARAM;
    it.item = item;
    return item && SendMessageW(a->tree, TVM_GETITEMW, 0, (iz)&it)
           ? (node *)it.lparam : 0;
}

static void tree_cmd(app *a, node *n, i32 id)
{
    b32   root = !n->parent;
    nodes s    = root ? (nodes){0} : (nodes){&n, 1};
    node *base = root ? n : n->parent;
    switch (id) {
    case CMD_OPENENTRY:
        navigate(a, n, 0);
        update_commands(a);
        break;
    case CMD_ADDFILES:
        if (!a->job && command_on(a, id, s)) cmd_addfiles(a, n);
        break;
    case CMD_ADDFOLDER:
        if (!a->job && command_on(a, id, s)) cmd_addfolder(a, n);
        break;
    case CMD_RENAME:
        if (!root && !a->job && command_on(a, id, s)) {
            SetFocus(a->tree);
            SendMessageW(a->tree, TVM_EDITLABELW, 0, (iz)n->item);
        }
        break;
    default:
        run_cmd(a, id, s, base);
    }
}

static void tree_menu(app *a, i32 x, i32 y)
{
    node *n = 0;
    if (x==-1 && y==-1) {  // the keyboard's: at the selected item
        iptr item = (iptr)SendMessageW(a->tree, TVM_GETNEXTITEM, TVGN_CARET,
                                       0);
        n = tree_node(a, item);
        union { rect r; iptr item; } u = {0};  // in: the item, out: its rect
        u.item = item;
        if (!n || !SendMessageW(a->tree, TVM_GETITEMRECT, 1, (iz)&u)) {
            return;
        }
        point p = {u.r.left, u.r.bottom};
        ClientToScreen(a->tree, &p);
        x = p.x;
        y = p.y;
    } else {
        tvhittestinfo h = {0};
        h.pt = (point){x, y};
        ScreenToClient(a->tree, &h.pt);
        iptr item = (iptr)SendMessageW(a->tree, TVM_HITTEST, 0, (iz)&h);
        n = h.flags&TVHT_ONITEM ? tree_node(a, item) : 0;
    }
    if (!n) {
        return;
    }

    iptr menu = CreatePopupMenu();
    if (n->parent) {
        AppendMenuW(menu, MF_STRING, CMD_OPENENTRY, L"&Open");
        AppendMenuW(menu, MF_SEPARATOR, 0, 0);
    }
    AppendMenuW(menu, MF_STRING, CMD_ADDFILES,  L"&Add files...");
    AppendMenuW(menu, MF_STRING, CMD_ADDFOLDER, L"Add fol&der...");
    AppendMenuW(menu, MF_SEPARATOR, 0, 0);
    AppendMenuW(menu, MF_STRING, CMD_EXTRACT,   L"&Extract...");
    if (n->parent) {
        AppendMenuW(menu, MF_STRING, CMD_MOVE,  L"&Move out...");
    }
    AppendMenuW(menu, MF_STRING, CMD_TEST,      L"&Test");
    if (n->parent) {
        AppendMenuW(menu, MF_SEPARATOR, 0, 0);
        AppendMenuW(menu, MF_STRING, CMD_RENAME, L"&Rename");
        AppendMenuW(menu, MF_STRING, CMD_DELETE, L"De&lete");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, 0);
    AppendMenuW(menu, MF_STRING, CMD_PROPERTIES, L"&Properties");

    nodes s = n->parent ? (nodes){&n, 1} : (nodes){0};
    for (i32 id = CMD_NEW; id <= CMD_ABOUT; id++) {
        b32 on = !a->job && (id==CMD_OPENENTRY || command_on(a, id, s));
        EnableMenuItem(menu, (u32)id, on ? MF_ENABLED : MF_GRAYED);
    }

    // Lit while its menu is up, as Explorer does
    SendMessageW(a->tree, TVM_SELECTITEM, TVGN_DROPHILITE, (iz)n->item);
    a->popup = 1;
    i32 c = (i32)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y,
                                0, a->hwnd, 0);
    a->popup = 0;
    SendMessageW(a->tree, TVM_SELECTITEM, TVGN_DROPHILITE, 0);
    DestroyMenu(menu);
    if (c) {
        tree_cmd(a, n, c);
    }
}


// Icons, painted: shapes measured by their signed distance from each
// pixel's center, covering it as far as it is within, and layered as
// premultiplied colors, at the exact size wanted, sharp at any DPI. The
// toolbar's: an archive with a zipper, and each command's badge.

typedef float f32;

typedef struct {
    f32 *px;    // premultiplied RGBA, row by row, top down
    i32  size;
} canvas;

enum { SHAPE_BOX, SHAPE_SEGMENT, SHAPE_CIRCLE };

typedef struct {
    i32 kind;
    f32 x, y;  // a box's or circle's center, a segment's first end
    f32 w, h;  // a box's half sizes, or a segment's second end
    f32 r;     // a box's corners', a segment's half width, a circle's
} shape;

static f32 fabsf_(f32 x)
{
    return x<0 ? -x : x;
}

static f32 fsqrt(f32 x)
{
    if (x <= 0) {
        return 0;
    }
    f32 r = x>1 ? x : 1;
    for (i32 i = 0; i < 24; i++) {
        f32 next = (r + x/r) / 2;
        if (next >= r) {
            break;
        }
        r = next;
    }
    return r;
}

static f32 clampf(f32 x, f32 lo, f32 hi)
{
    return x<lo ? lo : x>hi ? hi : x;
}

// Its distance from (px, py), negative within, in the shape's units.
static f32 distance(shape *s, f32 px, f32 py)
{
    switch (s->kind) {
    case SHAPE_BOX: {
        f32 qx = fabsf_(px-s->x) - s->w + s->r;
        f32 qy = fabsf_(py-s->y) - s->h + s->r;
        f32 ox = MAX(qx, 0), oy = MAX(qy, 0);
        return fsqrt(ox*ox + oy*oy) + MIN(MAX(qx, qy), 0) - s->r;
    }
    case SHAPE_SEGMENT: {
        f32 pax = px - s->x, pay = py - s->y;
        f32 bax = s->w - s->x, bay = s->h - s->y;
        f32 len = bax*bax + bay*bay;
        f32 h   = len>0 ? clampf((pax*bax + pay*bay)/len, 0, 1) : 0;
        f32 dx  = pax - bax*h, dy = pay - bay*h;
        return fsqrt(dx*dx + dy*dy) - s->r;
    }
    case SHAPE_CIRCLE: {
        f32 dx = px - s->x, dy = py - s->y;
        return fsqrt(dx*dx + dy*dy) - s->r;
    }
    }
    return 1;
}

// Paint a shape in a color (0xrrggbb), grown by grow pixels (negative,
// shrunk), or with a color of -1, erase it.
static void paint(canvas *c, shape s, i32 rgb, f32 grow)
{
    f32 n = (f32)c->size;
    f32 r = (f32)(rgb>>16 & 255) / 255;
    f32 g = (f32)(rgb>> 8 & 255) / 255;
    f32 b = (f32)(rgb     & 255) / 255;
    for (i32 y = 0; y < c->size; y++) {
        for (i32 x = 0; x < c->size; x++) {
            f32 d   = distance(&s, ((f32)x+0.5f)/n, ((f32)y+0.5f)/n)*n - grow;
            f32 cov = clampf(0.5f - d, 0, 1);
            if (cov <= 0) {
                continue;
            }
            f32 *p = c->px + 4*(y*c->size + x);
            if (rgb < 0) {
                for (i32 k = 0; k < 4; k++) {
                    p[k] *= 1 - cov;
                }
            } else {
                p[0] = r*cov + p[0]*(1-cov);
                p[1] = g*cov + p[1]*(1-cov);
                p[2] = b*cov + p[2]*(1-cov);
                p[3] =   cov + p[3]*(1-cov);
            }
        }
    }
}

static shape box(f32 x, f32 y, f32 w, f32 h, f32 r)
{
    return (shape){SHAPE_BOX, x, y, w, h, r};
}

static shape segment(f32 x0, f32 y0, f32 x1, f32 y1, f32 r)
{
    return (shape){SHAPE_SEGMENT, x0, y0, x1, y1, r};
}

static shape circle(f32 x, f32 y, f32 r)
{
    return (shape){SHAPE_CIRCLE, x, y, 0, 0, r};
}

// The pixel width of a line: one at small sizes, growing with the size.
static f32 stroke(canvas *c)
{
    return MAX(1.0f, (f32)c->size/24);
}

// The archive: a box with a zipper down its middle, centered at cx, of
// half sizes hw and hh.
static void paint_archive(canvas *c, f32 cx, f32 cy, f32 hw, f32 hh)
{
    f32 w = stroke(c);
    paint(c, box(cx, cy, hw, hh, 0.08f), 0x9a6418, 0);
    paint(c, box(cx, cy, hw, hh, 0.08f), 0xf2b84b, -w);
    paint(c, box(cx-hw/2, cy, hw/2, hh, 0.04f), 0xf7c96a, -w);  // its light
    f32 top  = cy - hh + 0.06f;
    f32 pull = cy + hh*0.30f;
    for (f32 y = top; y < pull-0.04f; y += 0.08f) {
        paint(c, box(cx-0.035f, y,         0.035f, 0.022f, 0), 0x6b4510, 0);
        paint(c, box(cx+0.035f, y+0.04f,   0.035f, 0.022f, 0), 0x6b4510, 0);
    }
    paint(c, box(cx, pull+0.07f, 0.06f, 0.09f, 0.03f), 0x6b4510, 0);
    paint(c, box(cx, pull+0.10f, 0.022f, 0.035f, 0.01f), 0xf2b84b, 0);
}

static void paint_folder(canvas *c)
{
    f32 w = stroke(c);
    paint(c, box(0.30f, 0.24f, 0.20f, 0.10f, 0.05f), 0xc08a1c, 0);
    paint(c, box(0.30f, 0.24f, 0.20f, 0.10f, 0.05f), 0xe5ad2c, -w);
    paint(c, box(0.50f, 0.56f, 0.44f, 0.32f, 0.06f), 0xc08a1c, 0);
    paint(c, box(0.50f, 0.56f, 0.44f, 0.32f, 0.06f), 0xe5ad2c, -w);
    paint(c, box(0.50f, 0.62f, 0.44f, 0.26f, 0.06f), 0xc08a1c, 0);
    paint(c, box(0.50f, 0.62f, 0.44f, 0.26f, 0.06f), 0xf8cd55, -w);
}

enum {
    GLYPH_PLUS, GLYPH_DOWN, GLYPH_RIGHT, GLYPH_CROSS, GLYPH_CURSOR,
    GLYPH_CHECK, GLYPH_PENCIL,
};

// A badge at the lower right: a gap cut around it, a disc, its glyph.
static void paint_badge(canvas *c, i32 rgb, i32 glyph)
{
    f32 x = 0.70f, y = 0.70f;
    f32 t = 0.05f;  // the glyph's half width
    paint(c, circle(x, y, 0.30f), -1, 0);
    paint(c, circle(x, y, 0.27f), rgb, 0);
    i32 white = 0xffffff;
    switch (glyph) {
    case GLYPH_PLUS:
        paint(c, segment(x-0.14f, y, x+0.14f, y, t), white, 0);
        paint(c, segment(x, y-0.14f, x, y+0.14f, t), white, 0);
        break;
    case GLYPH_DOWN:
        paint(c, segment(x, y-0.15f, x, y+0.12f, t), white, 0);
        paint(c, segment(x-0.11f, y+0.01f, x, y+0.12f, t), white, 0);
        paint(c, segment(x+0.11f, y+0.01f, x, y+0.12f, t), white, 0);
        break;
    case GLYPH_RIGHT:
        paint(c, segment(x-0.15f, y, x+0.12f, y, t), white, 0);
        paint(c, segment(x+0.01f, y-0.11f, x+0.12f, y, t), white, 0);
        paint(c, segment(x+0.01f, y+0.11f, x+0.12f, y, t), white, 0);
        break;
    case GLYPH_CROSS:
        paint(c, segment(x-0.10f, y-0.10f, x+0.10f, y+0.10f, t), white, 0);
        paint(c, segment(x-0.10f, y+0.10f, x+0.10f, y-0.10f, t), white, 0);
        break;
    case GLYPH_CURSOR:
        paint(c, segment(x, y-0.13f, x, y+0.13f, t*0.8f), white, 0);
        paint(c, segment(x-0.08f, y-0.14f, x+0.08f, y-0.14f, t*0.8f),
              white, 0);
        paint(c, segment(x-0.08f, y+0.14f, x+0.08f, y+0.14f, t*0.8f),
              white, 0);
        break;
    case GLYPH_CHECK:
        paint(c, segment(x-0.13f, y+0.01f, x-0.04f, y+0.10f, t), white, 0);
        paint(c, segment(x-0.04f, y+0.10f, x+0.13f, y-0.09f, t), white, 0);
        break;
    case GLYPH_PENCIL:
        paint(c, segment(x-0.08f, y+0.08f, x+0.11f, y-0.11f, 0.065f),
              white, 0);
        paint(c, segment(x-0.15f, y+0.15f, x-0.08f, y+0.08f, 0.03f),
              white, 0);
        paint(c, segment(x+0.03f, y-0.09f, x+0.09f, y-0.03f, 0.018f),
              rgb, 0);
        break;
    }
}

enum {
    ICON_OPEN, ICON_ADD, ICON_EXTRACT, ICON_MOVE, ICON_DELETE, ICON_RENAME,
    ICON_TEST, ICON_EDIT, ICON_UP, NICONS
};

static void paint_icon(canvas *c, i32 icon)
{
    static struct { i32 rgb, glyph; } const badges[] = {
        [ICON_ADD]     = {0x2e9e4f, GLYPH_PLUS},
        [ICON_EXTRACT] = {0x2f6fdb, GLYPH_DOWN},
        [ICON_MOVE]    = {0x7b52c9, GLYPH_RIGHT},
        [ICON_DELETE]  = {0xd9443a, GLYPH_CROSS},
        [ICON_RENAME]  = {0x5a6b7d, GLYPH_CURSOR},
        [ICON_TEST]    = {0x13917f, GLYPH_CHECK},
        [ICON_EDIT]    = {0xe07b24, GLYPH_PENCIL},
    };
    switch (icon) {
    case ICON_OPEN:
        paint_folder(c);
        break;
    case ICON_UP: {
        f32 t = 0.07f;
        paint(c, segment(0.5f, 0.20f, 0.5f, 0.82f, t), 0x3b4a5a, 0);
        paint(c, segment(0.24f, 0.46f, 0.5f, 0.20f, t), 0x3b4a5a, 0);
        paint(c, segment(0.76f, 0.46f, 0.5f, 0.20f, t), 0x3b4a5a, 0);
        break;
    }
    default:
        paint_archive(c, 0.40f, 0.48f, 0.30f, 0.42f);
        paint_badge(c, badges[icon].rgb, badges[icon].glyph);
    }
}

// The canvas as a 32-bit DIB, premultiplied (for image lists) or not
// (for icons), and grayed and faded, if dimmed.
static iptr canvas_dib(canvas *c, b32 premultiplied, b32 dimmed)
{
    struct {
        bitmapinfoheader h;
        u32              masks[3];
    } bi = {0};
    bi.h.size     = (u32)sizeof(bi.h);
    bi.h.width    = c->size;
    bi.h.height   = -c->size;
    bi.h.planes   = 1;
    bi.h.bitcount = 32;
    u8  *bits = 0;
    iptr dib  = CreateDIBSection(0, &bi, 0, (void **)&bits, 0, 0);
    if (!dib) {
        return 0;
    }
    for (i32 i = 0; i < c->size*c->size; i++) {
        f32 *p = c->px + 4*i;
        f32  r = p[0], g = p[1], b = p[2], a = p[3];
        if (dimmed) {
            f32 gray = 0.30f*r + 0.59f*g + 0.11f*b;
            gray = gray*0.6f + a*0.3f;  // lighter
            r = g = b = gray*0.45f;
            a *= 0.45f;
        }
        if (!premultiplied && a>0) {
            r /= a;
            g /= a;
            b /= a;
        }
        bits[4*i+0] = (u8)(clampf(b, 0, 1)*255 + 0.5f);
        bits[4*i+1] = (u8)(clampf(g, 0, 1)*255 + 0.5f);
        bits[4*i+2] = (u8)(clampf(r, 0, 1)*255 + 0.5f);
        bits[4*i+3] = (u8)(clampf(a, 0, 1)*255 + 0.5f);
    }
    return dib;
}

static canvas new_canvas(arena *perm, i32 size)
{
    canvas c = {0};
    c.size = size;
    c.px   = new(perm, 4*size*size, f32);
    return c;
}

// An icon of the painter's, as an icon handle, for windows and buttons.
static iptr painted_icon(app *a, i32 icon, i32 size, arena scratch)
{
    (void)a;
    canvas c = new_canvas(&scratch, size);
    if (icon < 0) {
        paint_archive(&c, 0.5f, 0.5f, 0.36f, 0.46f);  // the program's own
    } else {
        paint_icon(&c, icon);
    }
    iconinfo ii = {0};
    ii.icon  = 1;
    ii.color = canvas_dib(&c, 0, 0);
    ii.mask  = CreateBitmap(size, size, 1, 1, 0);
    iptr r   = CreateIconIndirect(&ii);
    DeleteObject(ii.color);
    DeleteObject(ii.mask);
    return r;
}

// The toolbar's images, and their dimmed forms, at the window's DPI.
static void make_tbimages(app *a)
{
    if (a->tbimages) {
        ImageList_Destroy(a->tbimages);
        ImageList_Destroy(a->tbdimmed);
    }
    a->tbsize   = scale(a, 24);
    a->tbimages = ImageList_Create(a->tbsize, a->tbsize, ILC_COLOR32, NICONS,
                                   0);
    a->tbdimmed = ImageList_Create(a->tbsize, a->tbsize, ILC_COLOR32, NICONS,
                                   0);
    for (i32 i = 0; i < NICONS; i++) {
        arena  tmp = a->scratch;
        canvas c   = new_canvas(&tmp, a->tbsize);
        paint_icon(&c, i);
        iptr lit = canvas_dib(&c, 1, 0);
        iptr dim = canvas_dib(&c, 1, 1);
        ImageList_Add(a->tbimages, lit, 0);
        ImageList_Add(a->tbdimmed, dim, 0);
        DeleteObject(lit);
        DeleteObject(dim);
    }
}

// The window's icons, big and small, at its DPI.
static void set_window_icons(app *a)
{
    iptr big   = painted_icon(a, -1, metric(a, SM_CXICON), a->scratch);
    iptr small = painted_icon(a, -1, metric(a, SM_CXSMICON), a->scratch);
    iptr old;
    old = (iptr)SendMessageW(a->hwnd, WM_SETICON, ICON_BIG, (iz)big);
    if (old) DestroyIcon(old);
    old = (iptr)SendMessageW(a->hwnd, WM_SETICON, ICON_SMALL, (iz)small);
    if (old) DestroyIcon(old);
    iptr up = painted_icon(a, ICON_UP, metric(a, SM_CXSMICON), a->scratch);
    old = (iptr)SendMessageW(a->upbutton, BM_SETIMAGE, IMAGE_ICON, (iz)up);
    if (old) DestroyIcon(old);
}


// Drag and drop, by OLE, which XP has too. In: files dropped on the list
// or the tree are added to the folder under the cursor (or the archive is
// opened, or made). Out: the chosen items, offered as files that do not
// yet exist, extracted to a temporary folder at the moment of the drop,
// once the button is released over a target that takes them; until then
// no engine runs. The files stay until the program ends, since a target
// may copy them after the drag.

typedef struct { u32 a; u16 b, c; u8 d[8]; } guid;
typedef struct { i32 x, y; } pointl;

static guid const iid_unknown    = {0x00000000, 0, 0,
                                    {0xc0,0,0,0,0,0,0,0x46}};
static guid const iid_dataobject = {0x0000010e, 0, 0,
                                    {0xc0,0,0,0,0,0,0,0x46}};
static guid const iid_dropsource = {0x00000121, 0, 0,
                                    {0xc0,0,0,0,0,0,0,0x46}};
static guid const iid_droptarget = {0x00000122, 0, 0,
                                    {0xc0,0,0,0,0,0,0,0x46}};

static b32 guid_equals(void const *x, guid const *y)
{
    u8 const *p = x;
    u8 const *q = (u8 const *)y;
    for (iz i = 0; i < (iz)sizeof(guid); i++) {
        if (p[i] != q[i]) {
            return 0;
        }
    }
    return 1;
}

W32(i32) SHCreateStdEnumFmtEtc(u32, formatetc const *, void **);

// Paths dropped, kept in a region of their own until their work is done
typedef struct {
    host  h;
    s8   *paths;
    iz    npaths;
    node *into;
} dropping;

// Another's data object, by its interface: what a drop target calls.
typedef struct dataobject dataobject;
typedef struct {
    i32 (__stdcall *queryinterface)(dataobject *, void const *, void **);
    u32 (__stdcall *addref)(dataobject *);
    u32 (__stdcall *release)(dataobject *);
    i32 (__stdcall *getdata)(dataobject *, formatetc *, stgmedium *);
    i32 (__stdcall *getdatahere)(dataobject *, formatetc *, stgmedium *);
    i32 (__stdcall *querygetdata)(dataobject *, formatetc *);
    i32 (__stdcall *getcanonical)(dataobject *, formatetc *, formatetc *);
    i32 (__stdcall *setdata)(dataobject *, formatetc *, stgmedium *, b32);
    i32 (__stdcall *enumformatetc)(dataobject *, u32, void **);
    i32 (__stdcall *dadvise)(dataobject *, formatetc *, u32, void *, u32 *);
    i32 (__stdcall *dunadvise)(dataobject *, u32);
    i32 (__stdcall *enumdadvise)(dataobject *, void **);
} dataobject_vtbl;
struct dataobject {
    dataobject_vtbl const *vtbl;
};

static formatetc hdrop_format(void)
{
    formatetc f = {0};
    f.format = CF_HDROP;
    f.aspect = DVASPECT_CONTENT;
    f.index  = -1;
    f.tymed  = TYMED_HGLOBAL;
    return f;
}


// In: the drop target

typedef struct droptarget droptarget;
typedef struct {
    i32 (__stdcall *queryinterface)(droptarget *, void const *, void **);
    u32 (__stdcall *addref)(droptarget *);
    u32 (__stdcall *release)(droptarget *);
    i32 (__stdcall *dragenter)(droptarget *, dataobject *, u32, pointl,
                               u32 *);
    i32 (__stdcall *dragover)(droptarget *, u32, pointl, u32 *);
    i32 (__stdcall *dragleave)(droptarget *);
    i32 (__stdcall *drop)(droptarget *, dataobject *, u32, pointl, u32 *);
} droptarget_vtbl;
struct droptarget {
    droptarget_vtbl const *vtbl;
    app  *a;
    iptr  hwnd;     // the list or the tree
    b32   files;    // the data has files
    node *hilited;  // the folder lit as the target
};

static i32 __stdcall dt_queryinterface(droptarget *t, void const *iid,
                                       void **out)
{
    if (guid_equals(iid, &iid_unknown) || guid_equals(iid, &iid_droptarget)) {
        *out = t;
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}

static u32 __stdcall dt_addref(droptarget *t)
{
    (void)t;
    return 1;  // it lives as long as the window
}

static u32 __stdcall dt_release(droptarget *t)
{
    (void)t;
    return 1;
}

// The folder a drop at a screen point would go into: a folder under it,
// else the folder shown, or null if none is open.
static node *drop_folder(droptarget *t, pointl pt)
{
    app  *a = t->a;
    point p = {pt.x, pt.y};
    ScreenToClient(t->hwnd, &p);
    if (t->hwnd == a->list) {
        lvhittestinfo h = {0};
        h.pt = p;
        i32 i = (i32)SendMessageW(a->list, LVM_HITTEST, 0, (iz)&h);
        if (a->folder && i>=0 && i<a->folder->nkids && (h.flags&LVHT_ONITEM)
            && a->folder->kids[i]->dir) {
            return a->folder->kids[i];
        }
        return a->folder;
    }
    tvhittestinfo h = {0};
    h.pt = p;
    iptr item = (iptr)SendMessageW(a->tree, TVM_HITTEST, 0, (iz)&h);
    if (item && (h.flags & TVHT_ONITEM)) {
        tvitem it = {0};
        it.mask = TVIF_PARAM;
        it.item = item;
        SendMessageW(a->tree, TVM_GETITEMW, 0, (iz)&it);
        return (node *)it.lparam;
    }
    return a->folder;
}

// Light the folder a drop would go into, as Explorer does.
static void hilite(droptarget *t, node *d)
{
    app *a = t->a;
    if (d == t->hilited) {
        return;
    }
    lvitem none = {0};
    none.statemask = LVIS_DROPHILITED;
    SendMessageW(a->list, LVM_SETITEMSTATE, (uptr)-1, (iz)&none);
    SendMessageW(a->tree, TVM_SELECTITEM, TVGN_DROPHILITE, 0);
    t->hilited = d;
    if (!d || d==a->folder) {
        return;
    } else if (t->hwnd == a->list) {
        for (iz i = 0; i < a->folder->nkids; i++) {
            if (a->folder->kids[i] == d) {
                lvitem it = {0};
                it.state = it.statemask = LVIS_DROPHILITED;
                SendMessageW(a->list, LVM_SETITEMSTATE, (uptr)i, (iz)&it);
            }
        }
    } else if (d->item) {
        SendMessageW(a->tree, TVM_SELECTITEM, TVGN_DROPHILITE, (iz)d->item);
    }
}

static u32 drop_effect(droptarget *t, pointl pt)
{
    app *a = t->a;
    if (!t->files || a->job || a->dragging ||
        (a->m && (a->m->readonly || a->m->damaged))) {
        hilite(t, 0);
        return DROPEFFECT_NONE;
    }
    hilite(t, a->m ? drop_folder(t, pt) : 0);
    return DROPEFFECT_COPY;
}

static i32 __stdcall dt_dragenter(droptarget *t, dataobject *d, u32 keys,
                                  pointl pt, u32 *effect)
{
    (void)keys;
    formatetc f = hdrop_format();
    t->files   = d->vtbl->querygetdata(d, &f) == S_OK;
    t->hilited = 0;
    *effect   &= drop_effect(t, pt);
    return S_OK;
}

static i32 __stdcall dt_dragover(droptarget *t, u32 keys, pointl pt,
                                 u32 *effect)
{
    (void)keys;
    *effect &= drop_effect(t, pt);
    return S_OK;
}

static i32 __stdcall dt_dragleave(droptarget *t)
{
    hilite(t, 0);
    return S_OK;
}

// The drop: its paths taken, its work posted, so that the source, which
// waits on this, is let go before any dialog.
static i32 __stdcall dt_drop(droptarget *t, dataobject *d, u32 keys,
                             pointl pt, u32 *effect)
{
    (void)keys;
    app *a = t->a;
    u32  e = drop_effect(t, pt);
    node *into = t->hilited ? t->hilited : a->folder;
    hilite(t, 0);
    *effect &= e;
    if (!*effect) {
        return S_OK;
    }
    formatetc f = hdrop_format();
    stgmedium m = {0};
    if (d->vtbl->getdata(d, &f, &m) != S_OK) {
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    host *h = region_new((iz)1<<24, (iz)sizeof(dropping));
    if (h) {
        dropping *r    = (dropping *)h;
        arena     perm = host_perm(&r->h);
        u32 n = DragQueryFileW(m.hglobal, (u32)-1, 0, 0);
        r->paths = new(&perm, n, s8);
        for (u32 i = 0; i < n; i++) {
            u32  len = DragQueryFileW(m.hglobal, i, 0, 0);
            c16 *buf = new(&perm, len+1, c16);  // (garbage beside)
            DragQueryFileW(m.hglobal, i, buf, len+1);
            r->paths[r->npaths++] = towtf8(&perm, buf);
        }
        r->into = into;
        PostMessageW(a->hwnd, WM_APP_DROP, 0, (iz)r);
    }
    ReleaseStgMedium(&m);
    return S_OK;
}

static droptarget_vtbl const droptarget_methods = {
    dt_queryinterface, dt_addref, dt_release,
    dt_dragenter, dt_dragover, dt_dragleave, dt_drop,
};

static b32 ends_with_zip(s8 path)
{
    if (path.len < 4) {
        return 0;
    }
    u8 *s = path.s + path.len - 4;
    return s[0]=='.' && (s[1]|32)=='z' && (s[2]|32)=='i' && (s[3]|32)=='p';
}

// A drop's work, back in the window's own time: with no archive open, a
// single .zip opens; otherwise the files are added.
static void on_drop(app *a, dropping *r)
{
    SetForegroundWindow(a->hwnd);
    if (a->job) {
        // (a job began meanwhile)
    } else if (!a->m && r->npaths==1 && ends_with_zip(r->paths[0])) {
        open_zip(a, r->paths[0], (s8){0}, 0);
    } else {
        add_paths(a, r->into, r->paths, r->npaths);
    }
    region_free(&r->h);
}

static void register_drops(app *a)
{
    static droptarget list, tree;
    list = (droptarget){&droptarget_methods, a, a->list, 0, 0};
    tree = (droptarget){&droptarget_methods, a, a->tree, 0, 0};
    RegisterDragDrop(a->list, &list);
    RegisterDragDrop(a->tree, &tree);
}

static void revoke_drops(app *a)
{
    RevokeDragDrop(a->list);
    RevokeDragDrop(a->tree);
}


// Out: our data object and drop source

typedef struct dragobject dragobject;
typedef struct {
    i32 (__stdcall *queryinterface)(dragobject *, void const *, void **);
    u32 (__stdcall *addref)(dragobject *);
    u32 (__stdcall *release)(dragobject *);
    i32 (__stdcall *getdata)(dragobject *, formatetc *, stgmedium *);
    i32 (__stdcall *getdatahere)(dragobject *, formatetc *, stgmedium *);
    i32 (__stdcall *querygetdata)(dragobject *, formatetc *);
    i32 (__stdcall *getcanonical)(dragobject *, formatetc *, formatetc *);
    i32 (__stdcall *setdata)(dragobject *, formatetc *, stgmedium *, b32);
    i32 (__stdcall *enumformatetc)(dragobject *, u32, void **);
    i32 (__stdcall *dadvise)(dragobject *, formatetc *, u32, void *, u32 *);
    i32 (__stdcall *dunadvise)(dragobject *, u32);
    i32 (__stdcall *enumdadvise)(dragobject *, void **);
} dragobject_vtbl;

typedef struct dropsource dropsource;
typedef struct {
    i32 (__stdcall *queryinterface)(dropsource *, void const *, void **);
    u32 (__stdcall *addref)(dropsource *);
    u32 (__stdcall *release)(dropsource *);
    i32 (__stdcall *querycontinuedrag)(dropsource *, b32, u32);
    i32 (__stdcall *givefeedback)(dropsource *, u32);
} dropsource_vtbl;

struct dragobject {
    dragobject_vtbl const *vtbl;
    i32   refs;
    c16  *files;  // DROPFILES' list: paths, each null-terminated, then null
    iz    len;    // its length, its final null included
};

struct dropsource {
    dropsource_vtbl const *vtbl;
    app  *a;
    u32   effect;  // the target's, as last given
    b32   failed;  // the extraction
    b32   done;    // extracted
    nodes chosen;
    node *base;
    s8    dir;     // the temporary folder extracted into
};

static i32 __stdcall do_queryinterface(dragobject *o, void const *iid,
                                       void **out)
{
    if (guid_equals(iid, &iid_unknown) || guid_equals(iid, &iid_dataobject)) {
        o->refs++;
        *out = o;
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}

static u32 __stdcall do_addref(dragobject *o)
{
    return (u32)++o->refs;
}

static u32 __stdcall do_release(dragobject *o)
{
    return (u32)--o->refs;  // its memory is the app's
}

static i32 __stdcall do_querygetdata(dragobject *o, formatetc *f)
{
    (void)o;
    if (f->format!=CF_HDROP || !(f->tymed & TYMED_HGLOBAL) ||
        f->aspect!=DVASPECT_CONTENT) {
        return DV_E_FORMATETC;
    }
    return S_OK;
}

static i32 __stdcall do_getdata(dragobject *o, formatetc *f, stgmedium *m)
{
    i32 r = do_querygetdata(o, f);
    if (r != S_OK) {
        return r;
    }
    iz   size = (iz)sizeof(dropfiles) + o->len*(iz)sizeof(c16);
    iptr g    = GlobalAlloc(GMEM_MOVEABLE|GMEM_ZEROINIT, size);
    if (!g) {
        return E_OUTOFMEMORY;
    }
    dropfiles *d = GlobalLock(g);
    d->files = (u32)sizeof(dropfiles);
    d->wide  = 1;
    bytecopy(d+1, o->files, o->len*(iz)sizeof(c16));
    GlobalUnlock(g);
    m->tymed         = TYMED_HGLOBAL;
    m->hglobal       = g;
    m->unkforrelease = 0;
    return S_OK;
}

static i32 __stdcall do_getdatahere(dragobject *o, formatetc *f,
                                    stgmedium *m)
{
    (void)o; (void)f; (void)m;
    return E_NOTIMPL;
}

static i32 __stdcall do_getcanonical(dragobject *o, formatetc *f,
                                     formatetc *out)
{
    (void)o; (void)f;
    out->ptd = 0;
    return E_NOTIMPL;
}

static i32 __stdcall do_setdata(dragobject *o, formatetc *f, stgmedium *m,
                                b32 release)
{
    (void)o; (void)f; (void)m; (void)release;
    return E_NOTIMPL;
}

static i32 __stdcall do_enumformatetc(dragobject *o, u32 dir, void **out)
{
    (void)o;
    if (dir != DATADIR_GET) {
        *out = 0;
        return E_NOTIMPL;
    }
    formatetc f = hdrop_format();
    return SHCreateStdEnumFmtEtc(1, &f, out);
}

static i32 __stdcall do_dadvise(dragobject *o, formatetc *f, u32 flags,
                                void *sink, u32 *conn)
{
    (void)o; (void)f; (void)flags; (void)sink; (void)conn;
    return OLE_E_ADVISENOTSUPPORTED;
}

static i32 __stdcall do_dunadvise(dragobject *o, u32 conn)
{
    (void)o; (void)conn;
    return OLE_E_ADVISENOTSUPPORTED;
}

static i32 __stdcall do_enumdadvise(dragobject *o, void **out)
{
    (void)o;
    *out = 0;
    return OLE_E_ADVISENOTSUPPORTED;
}

static dragobject_vtbl const dragobject_methods = {
    do_queryinterface, do_addref, do_release, do_getdata, do_getdatahere,
    do_querygetdata, do_getcanonical, do_setdata, do_enumformatetc,
    do_dadvise, do_dunadvise, do_enumdadvise,
};

static i32 __stdcall ds_queryinterface(dropsource *s, void const *iid,
                                       void **out)
{
    if (guid_equals(iid, &iid_unknown) || guid_equals(iid, &iid_dropsource)) {
        *out = s;
        return S_OK;
    }
    *out = 0;
    return E_NOINTERFACE;
}

static u32 __stdcall ds_addref(dropsource *s)
{
    (void)s;
    return 1;
}

static u32 __stdcall ds_release(dropsource *s)
{
    (void)s;
    return 1;
}

static void drag_done(app *a, job *j)
{
    mark_picks(j, a->scratch);
    dropsource *s = a->drag;
    s->done   = 1;
    s->failed = j->status>PK_WARN;
    if (s->failed && j->status!=STATUS_STOPPED) {
        job_report(a, j, L"Drag");
    }
}

// The button released over a target that takes the files: extract them
// now, waiting in a loop of our own, then let the drop go on.
static i32 __stdcall ds_querycontinuedrag(dropsource *s, b32 escape,
                                          u32 keys)
{
    app *a = s->a;
    if (escape) {
        return DRAGDROP_S_CANCEL;
    } else if (keys & MK_LBUTTON) {
        return S_OK;
    } else if (s->effect == DROPEFFECT_NONE) {
        return DRAGDROP_S_DROP;  // nothing takes it: OLE cancels
    }

    job *j = archive_job(a, WHO_UNZIP, L"Extracting");
    if (!j) {
        return DRAGDROP_S_CANCEL;
    }
    j->uc.mode      = CALL_EXTRACT;
    j->uc.exdir     = JOIN(&j->perm, s->dir);
    j->uc.overwrite = OVERWRT_ALWAYS;
    j->done         = drag_done;
    pick_nodes(j, s->chosen, s->base, 0);
    a->drag = s;
    if (!job_start(a, j)) {
        return DRAGDROP_S_CANCEL;
    }
    while (!s->done) {
        winmsg m;
        if (GetMessageW(&m, 0, 0, 0) <= 0) {
            PostQuitMessage((i32)m.wparam);
            return DRAGDROP_S_CANCEL;
        }
        if (a->progress && IsDialogMessageW(a->progress, &m)) {
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return s->failed ? DRAGDROP_S_CANCEL : DRAGDROP_S_DROP;
}

static i32 __stdcall ds_givefeedback(dropsource *s, u32 effect)
{
    s->effect = effect;
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

static dropsource_vtbl const dropsource_methods = {
    ds_queryinterface, ds_addref, ds_release, ds_querycontinuedrag,
    ds_givefeedback,
};

// Drag the chosen items out of the list.
static void drag_out(app *a)
{
    arena *tmp = &a->scratch;
    nodes  s   = selection(a, tmp);
    if (!s.n || a->job || !a->m) {
        return;
    }
    // The paths they will have, in a new temporary folder: the app's,
    // since a target may hold the data object after the drag
    s8  dir = temp_dir(a, &a->perm);
    iz  cap = 1;
    c16 *wdir = towide(tmp, dir);
    for (iz i = 0; i < s.n; i++) {
        cap += wlen(wdir) + 1 + 2*s.v[i]->name.len + 1;
    }
    dragobject *o = new(&a->perm, 1, dragobject);
    o->vtbl  = &dragobject_methods;
    o->refs  = 1;
    o->files = new(&a->perm, cap, c16);
    for (iz i = 0; i < s.n; i++) {
        wbuf b = wbuf_of(o->files+o->len, cap-o->len);
        wput(&b, wdir);
        wputc(&b, '\\');
        wput(&b, towide(tmp, s.v[i]->name));
        o->len += b.len + 1;
    }
    o->len++;  // the list's final null

    dropsource src = {0};
    src.vtbl   = &dropsource_methods;
    src.a      = a;
    src.chosen = s;
    src.base   = a->folder;
    src.dir    = dir;
    a->dragging = 1;
    u32 effect = 0;
    DoDragDrop(o, &src, DROPEFFECT_COPY, &effect);
    a->dragging = 0;
    a->drag     = 0;
}


static void save_columns(app *a);

// Settings, under HKEY_CURRENT_USER\Software\tugz: the window's place,
// its splitter, columns, and sort, and the options of the dialogs.

static c16 const settings_key[] = L"Software\\tugz";

enum { KEY_QUERY_VALUE = 0x0001, KEY_SET_VALUE = 0x0002 };

static iptr open_settings(void)
{
    iptr key = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, settings_key, 0, 0, 0,
                        KEY_QUERY_VALUE|KEY_SET_VALUE, 0, &key, 0)) {
        return 0;
    }
    return key;
}

static b32 get_value(iptr key, c16 const *name, void *buf, u32 len)
{
    u32 type = 0;
    u32 got  = len;
    return key && !RegQueryValueExW(key, name, 0, &type, buf, &got) &&
           got==len;
}

static i32 get_number(iptr key, c16 const *name, i32 dflt, i32 lo, i32 hi)
{
    i32 v = 0;
    if (!get_value(key, name, &v, sizeof(v)) || v<lo || v>hi) {
        return dflt;
    }
    return v;
}

static void set_number(iptr key, c16 const *name, i32 v)
{
    RegSetValueExW(key, name, 0, REG_DWORD, &v, sizeof(v));
}

static void load_settings(app *a)
{
    iptr     key = open_settings();
    options *o   = &a->opt;
    a->split      = get_number(key, L"Split",      220, 60, 2000);
    a->sortcol    = get_number(key, L"SortColumn", 0, 0, NCOLS-1);
    a->sortdesc   = get_number(key, L"SortDown",   0, 0, 1);
    o->level      = get_number(key, L"Level",      6, 0, 9);
    o->mode       = get_number(key, L"Mode",       MODE_ADD, 0, 2);
    o->hidden     = get_number(key, L"Hidden",     0, 0, 1);
    o->noextra    = get_number(key, L"NoExtra",    0, 0, 1);
    o->nodirs     = get_number(key, L"NoFolders",  0, 0, 1);
    o->askreplace = get_number(key, L"AskReplace", 1, 0, 1);
    o->overwrite  = get_number(key, L"Overwrite",  0, 0, 2);
    o->flat       = get_number(key, L"Flat",       0, 0, 1);
    o->openafter  = get_number(key, L"OpenAfter",  0, 0, 1);
    i32 widths[NCOLS] = {0};
    if (get_value(key, L"Columns", widths, sizeof(widths))) {
        for (i32 c = 0; c < NCOLS; c++) {
            a->colwidth[c] = widths[c]>=0 && widths[c]<=4000 ? widths[c] : 0;
        }
    }
    c16 edit[countof(o->edit)] = {0};
    u32 type = 0;
    u32 len  = (u32)sizeof(edit) - 2;
    if (key && !RegQueryValueExW(key, L"Edit", 0, &type, edit, &len) &&
        type==REG_SZ && edit[0]) {
        bytecopy(o->edit, edit, (iz)sizeof(edit));
    } else {
        wbuf b = wbuf_of(o->edit, countof(o->edit));
        wput(&b, L"notepad.exe");
    }
    if (key) {
        RegCloseKey(key);
    }
}

// The window's place, as it was last saved, if it is on a monitor now.
static void load_placement(app *a)
{
    iptr key = open_settings();
    windowplacement p = {0};
    if (get_value(key, L"Placement", &p, sizeof(p)) &&
        p.length==sizeof(p)) {
        rect r = p.normal;
        if (r.right-r.left>=100 && r.bottom-r.top>=100 &&
            MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            p.flags = 0;
            if (p.showcmd != SW_SHOWMAXIMIZED) {
                p.showcmd = SW_SHOWNORMAL;
            }
            SetWindowPlacement(a->hwnd, &p);
        }
    }
    if (key) {
        RegCloseKey(key);
    }
}

static void save_settings(app *a)
{
    iptr key = open_settings();
    if (!key) {
        return;
    }
    options *o = &a->opt;
    windowplacement p = {0};
    p.length = (u32)sizeof(p);
    if (GetWindowPlacement(a->hwnd, &p)) {
        RegSetValueExW(key, L"Placement", 0, REG_BINARY, &p, sizeof(p));
    }
    save_columns(a);
    RegSetValueExW(key, L"Columns", 0, REG_BINARY, a->colwidth,
                   sizeof(a->colwidth));
    set_number(key, L"Split",      a->split);
    set_number(key, L"SortColumn", a->sortcol);
    set_number(key, L"SortDown",   a->sortdesc);
    set_number(key, L"Level",      o->level);
    set_number(key, L"Mode",       o->mode);
    set_number(key, L"Hidden",     o->hidden);
    set_number(key, L"NoExtra",    o->noextra);
    set_number(key, L"NoFolders",  o->nodirs);
    set_number(key, L"AskReplace", o->askreplace);
    set_number(key, L"Overwrite",  o->overwrite);
    set_number(key, L"Flat",       o->flat);
    set_number(key, L"OpenAfter",  o->openafter);
    RegSetValueExW(key, L"Edit", 0, REG_SZ, o->edit,
                   (u32)((wlen(o->edit)+1)*(iz)sizeof(c16)));
    RegCloseKey(key);
}

// Back in the window: if the archive changed on disk meanwhile, by
// another program, show it as it is now.
static void on_activate(app *a)
{
    if (!a->m || a->job || a->asking || a->dragging) {
        return;
    }
    i64 size    = 0;
    u64 written = 0;
    if (!file_state(a->m->wpath, &size, &written)) {
        written = 1;  // gone: the reload says so
    }
    if (size!=a->m->size || written!=a->m->written) {
        reload(a);
    }
}


// The window procedure and its messages

static i32 list_column_width(app *a, i32 c)
{
    return scale(a, a->colwidth[c] ? a->colwidth[c] : column_widths[c]);
}

static void add_columns(app *a)
{
    for (i32 c = 0; c < NCOLS; c++) {
        lvcolumn col = {0};
        col.mask    = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        col.fmt     = column_right(c) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        col.cx      = list_column_width(a, c);
        col.text    = (c16 *)column_names[c];
        col.subitem = c;
        SendMessageW(a->list, LVM_INSERTCOLUMNW, (uptr)c, (iz)&col);
    }
    show_sort(a);
}

// The column widths, at 96 DPI, kept as the window's DPI changes.
static void save_columns(app *a)
{
    for (i32 c = 0; c < NCOLS; c++) {
        i32 w = (i32)SendMessageW(a->list, LVM_GETCOLUMNWIDTH, (uptr)c, 0);
        a->colwidth[c] = MulDiv(w, 96, (i32)a->dpi);
    }
}

static void make_toolbar(app *a);
static void set_fonts(app *a);

static void create_controls(app *a)
{
    iptr w = a->hwnd;
    a->toolbar = CreateWindowExW(0, L"ToolbarWindow32", 0, WS_CHILD |
                                 WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
                                 CCS_NODIVIDER | CCS_TOP, 0, 0, 0, 0, w,
                                 ID_TOOLBAR, a->instance, 0);
    a->upbutton = CreateWindowExW(0, L"BUTTON", L"Up", WS_CHILD |
                                  WS_VISIBLE | BS_PUSHBUTTON | BS_ICON, 0, 0,
                                  0, 0, w,
                                  ID_UP, a->instance, 0);
    a->address = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD |
                                 WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                                 0, 0, 0, 0, w, ID_ADDRESS, a->instance, 0);
    a->info = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT |
                              SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 0, 0, w,
                              ID_INFO, a->instance, 0);
    a->editbar = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT |
                                 SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 0, 0, w,
                                 ID_EDITBAR, a->instance, 0);
    a->editupdate = CreateWindowExW(0, L"BUTTON", L"Update", WS_CHILD |
                                    BS_PUSHBUTTON, 0, 0, 0, 0, w,
                                    ID_EDITUPDATE, a->instance, 0);
    a->editstop = CreateWindowExW(0, L"BUTTON", L"Stop waiting", WS_CHILD |
                                  BS_PUSHBUTTON, 0, 0, 0, 0, w, ID_EDITSTOP,
                                  a->instance, 0);
    u32 lines = a->xp ? TVS_HASLINES : 0;
    a->tree = CreateWindowExW(WS_EX_CLIENTEDGE, L"SysTreeView32", 0,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              TVS_HASBUTTONS | TVS_LINESATROOT | lines |
                              TVS_SHOWSELALWAYS | TVS_EDITLABELS, 0, 0, 0, 0,
                              w, ID_TREE, a->instance, 0);
    a->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"SysListView32", 0,
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS |
                              LVS_EDITLABELS | LVS_SHAREIMAGELISTS, 0, 0, 0,
                              0, w, ID_LIST, a->instance, 0);
    a->status = CreateWindowExW(0, L"msctls_statusbar32", 0, WS_CHILD |
                                WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, w,
                                ID_STATUS, a->instance, 0);
    if (a->api.setwindowtheme) {
        a->api.setwindowtheme(a->tree, L"Explorer", 0);
        a->api.setwindowtheme(a->list, L"Explorer", 0);
    }
    SendMessageW(a->list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                 LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
    SendMessageW(a->list, LVM_SETCALLBACKMASK, LVIS_OVERLAYMASK, 0);
    SendMessageW(a->tree, TVM_SETEXTENDEDSTYLE,
                 TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    add_columns(a);
    make_images(a);
    make_toolbar(a);
    set_fonts(a);
}

static void set_fonts(app *a)
{
    iptr controls[] = {a->toolbar, a->upbutton, a->address, a->info,
                       a->editbar, a->editupdate, a->editstop,
                       a->tree, a->list, a->status};
    for (i32 i = 0; i < countof(controls); i++) {
        SendMessageW(controls[i], WM_SETFONT, (uptr)a->font, 1);
    }
}

static void cmd(app *a, i32 id);

// The splitter: the gap between the tree and the list
static b32 on_splitter(app *a, i32 x, i32 y)
{
    rect tr = {0}, lr = {0};
    GetWindowRect(a->tree, &tr);
    GetWindowRect(a->list, &lr);
    point p = {x, y};
    ClientToScreen(a->hwnd, &p);
    return p.x>=tr.right && p.x<lr.left && p.y>=tr.top && p.y<tr.bottom;
}

static iz on_notify(app *a, nmhdr *h);

static void on_dpi(app *a, u32 dpi, rect *suggested)
{
    save_columns(a);
    a->dpi = dpi;
    make_fonts(a);
    set_fonts(a);
    make_images(a);
    make_toolbar(a);
    set_window_icons(a);
    for (i32 c = 0; c < NCOLS; c++) {
        SendMessageW(a->list, LVM_SETCOLUMNWIDTH, (uptr)c,
                     list_column_width(a, c));
    }
    if (suggested) {
        SetWindowPos(a->hwnd, 0, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout(a);
    InvalidateRect(a->hwnd, 0, 1);
}

static iz main_message(app *a, iptr hwnd, u32 msg, uptr wp, iz lp)
{
    switch (msg) {
    case WM_CREATE:
        a->hwnd = hwnd;
        a->dpi  = window_dpi(a, hwnd);
        make_fonts(a);
        create_controls(a);
        set_window_icons(a);
        register_drops(a);
        return 0;

    case WM_SIZE:
        layout(a);
        return 0;

    case WM_GETMINMAXINFO: {
        minmaxinfo *mm = (minmaxinfo *)lp;
        mm->mintracksize.x = scale(a, 420);
        mm->mintracksize.y = scale(a, 300);
        return 0;
    }

    case WM_DPICHANGED:
        on_dpi(a, (u32)(wp & 0xffff), (rect *)lp);
        return 0;

    case WM_SETCURSOR:
        if ((iptr)wp == hwnd) {
            point p = {0};
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (on_splitter(a, p.x, p.y)) {
                SetCursor(LoadCursorW(0, IDC_SIZEWE));
                return 1;
            }
        }
        break;

    case WM_LBUTTONDOWN:
        if (on_splitter(a, (i32)(short)(lp & 0xffff), (i32)(short)(lp >> 16 & 0xffff))) {
            a->splitting = 1;
            SetCapture(hwnd);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (a->splitting) {
            i32 x = (i32)(short)(lp & 0xffff);
            a->split = MulDiv(MAX(x, 0), 96, (i32)a->dpi);
            layout(a);
        }
        return 0;

    case WM_LBUTTONUP:
        if (a->splitting) {
            a->splitting = 0;
            ReleaseCapture();
        }
        return 0;

    case WM_CAPTURECHANGED:
        a->splitting = 0;
        return 0;

    case WM_NOTIFY:
        return on_notify(a, (nmhdr *)lp);

    case WM_COMMAND:
        if ((wp & 0xffff) == ID_UP) {
            cmd(a, CMD_UP);
        } else if ((wp & 0xffff) == ID_EDITUPDATE) {
            edit_handed(a);
        } else if ((wp & 0xffff) == ID_EDITSTOP) {
            edit_stop(a);
        } else if (!lp || (iptr)lp==a->toolbar) {
            cmd(a, (i32)(wp & 0xffff));
        }
        return 0;

    case WM_ERASEBKGND:
        if (a->edits) {
            iz r = DefWindowProcW(hwnd, msg, wp, lp);
            FillRect((iptr)wp, &a->editrow, GetSysColorBrush(COLOR_INFOBK));
            return r;
        }
        break;

    case WM_CTLCOLORSTATIC:
        if ((iptr)lp==a->info || (iptr)lp==a->editbar) {
            SetTextColor((iptr)wp, GetSysColor(COLOR_INFOTEXT));
            SetBkMode((iptr)wp, TRANSPARENT);
            return (iz)GetSysColorBrush(COLOR_INFOBK);
        }
        break;

    case WM_TIMER:
        if (wp == TIMER_JOB) {
            job_tick(a);
        } else if (wp == TIMER_EDIT) {
            edit_tick(a);
        }
        return 0;

    case WM_APP_ASK:
        on_ask(a, (asking *)lp);
        return 0;

    case WM_APP_DROP:
        on_drop(a, (dropping *)lp);
        return 0;

    case WM_ACTIVATE:
        if ((wp & 0xffff) == 0) {  // WA_INACTIVE
            save_focus(a);
        } else if (!(wp >> 16 & 0xffff) && IsWindowEnabled(hwnd)) {
            restore_focus(a);  // not minimized
        }
        return 0;

    case WM_SETFOCUS:
        restore_focus(a);
        return 0;

    case WM_ACTIVATEAPP:
        if (wp) {
            PostMessageW(hwnd, WM_APP_ACTIVATED, 0, 0);
        }
        break;

    case WM_APP_ACTIVATED:
        on_activate(a);
        return 0;

    case WM_CONTEXTMENU:
        if ((iptr)wp == a->list) {
            context_menu(a, (i32)(short)(lp & 0xffff),
                         (i32)(short)(lp >> 16 & 0xffff));
            return 0;
        } else if ((iptr)wp == a->tree) {
            tree_menu(a, (i32)(short)(lp & 0xffff),
                      (i32)(short)(lp >> 16 & 0xffff));
            return 0;
        }
        break;

    case WM_INITMENUPOPUP:
        if (!a->popup) {
            enable_menu(a, (iptr)wp);
        }
        return 0;

    case WM_APP_JOBDONE:
        job_end(a, (job *)lp);
        if (a->quitting) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_CLOSE:
        if (a->edits && MessageBoxW(hwnd, L"Files are still being edited. "
                                    L"Quit without updating the archive "
                                    L"from them?", L"tugz", MB_YESNO |
                                    MB_ICONQUESTION | MB_DEFBUTTON2)!=IDYES) {
            return 0;
        }
        if (a->job) {
            if (MessageBoxW(a->progress ? a->progress : hwnd,
                            L"Stop the work under way, and quit?", L"tugz",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                __atomic_store_n(&a->job->stop, 1, __ATOMIC_RELAXED);
                a->quitting = 1;  // closes once the job ends
            }
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        save_settings(a);
        revoke_drops(a);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Each message takes scratch from where it stands, and gives it back,
// for messages within it (a nested loop's) to take what is beyond.
static iz __stdcall main_proc(iptr hwnd, u32 msg, uptr wp, iz lp)
{
    app *a = (app *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!a) {
        if (msg == 0x0081) {  // WM_NCCREATE: its CREATESTRUCT's param
            a = *(app **)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (iz)a);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    byte *mark = a->scratch.end;
    iz    r    = main_message(a, hwnd, msg, wp, lp);
    a->scratch.end = mark;
    return r;
}


// Notifications from the tree, the list, and the toolbar

static void open_node(app *a, node *n);

static iz on_list_notify(app *a, nmhdr *h)
{
    switch (h->code) {
    case LVN_GETDISPINFOW: {
        nmlvdispinfo *d = (nmlvdispinfo *)h;
        if (!a->folder || d->item.item<0 || d->item.item>=a->folder->nkids) {
            return 0;
        }
        node *n = a->folder->kids[d->item.item];
        if (d->item.mask & LVIF_TEXT) {
            wbuf b = wbuf_of(d->item.text, d->item.textmax);
            put_cell(a, &b, n, d->item.subitem);
        }
        if (d->item.mask & LVIF_IMAGE) {
            d->item.image = node_image(a, n);
        }
        if ((d->item.mask & LVIF_STATE) && n->e &&
            (n->e->flags & ZIP_FLAG_ENCRYPTED)) {
            d->item.state     = 1 << 8;  // the first overlay: a lock
            d->item.statemask = LVIS_OVERLAYMASK;
        }
        return 0;
    }
    case LVN_ITEMCHANGED:
    case LVN_ODSTATECHANGED:
        set_status(a);
        update_commands(a);
        return 0;
    case LVN_COLUMNCLICK: {
        nmlistview *v = (nmlistview *)h;
        if (v->subitem == a->sortcol) {
            a->sortdesc = !a->sortdesc;
        } else {
            a->sortcol  = v->subitem;
            a->sortdesc = 0;
        }
        show_sort(a);
        if (a->folder) {
            navigate(a, a->folder, 0);
        }
        return 0;
    }
    case LVN_ITEMACTIVATE: {
        nmlistview *v = (nmlistview *)h;
        if (a->folder && v->item>=0 && v->item<a->folder->nkids) {
            open_node(a, a->folder->kids[v->item]);
        }
        return 0;
    }
    case LVN_BEGINDRAG:
        drag_out(a);
        return 0;
    case LVN_BEGINLABELEDITW: {
        arena tmp = a->scratch;
        return a->job || !command_on(a, CMD_RENAME, selection(a, &tmp));
    }
    case LVN_ENDLABELEDITW: {
        nmlvdispinfo *d = (nmlvdispinfo *)h;
        if (d->item.text && a->folder && d->item.item>=0 &&
            d->item.item<a->folder->nkids) {
            rename_node(a, a->folder->kids[d->item.item], d->item.text);
        }
        return 0;
    }
    case LVN_KEYDOWN: {
        nmkeydown *k = (nmkeydown *)h;
        b32 ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (k->vkey) {
        case VK_BACK:   cmd(a, CMD_UP);      break;
        case VK_DELETE: cmd(a, CMD_DELETE);  break;
        case VK_F2:     cmd(a, CMD_RENAME);  break;
        case 'A':       if (ctrl) cmd(a, CMD_SELECTALL);  break;
        }
        return 0;
    }
    }
    return 0;
}

static iz on_tree_notify(app *a, nmhdr *h)
{
    switch (h->code) {
    case TVN_ITEMEXPANDINGW: {
        nmtreeview *t = (nmtreeview *)h;
        node *d = (node *)t->new.lparam;
        if (d) {
            tree_fill(a, d);
        }
        return 0;
    }
    case TVN_SELCHANGEDW: {
        nmtreeview *t = (nmtreeview *)h;
        node *d = (node *)t->new.lparam;
        if (d && !a->syncing && d!=a->folder) {
            navigate(a, d, 0);
            update_commands(a);
        }
        return 0;
    }
    case TVN_BEGINLABELEDITW: {
        nmtvdispinfo *d = (nmtvdispinfo *)h;
        node *n = (node *)d->item.lparam;
        nodes s = {&n, 1};
        return !n || !n->parent || a->job || !command_on(a, CMD_RENAME, s);
    }
    case TVN_ENDLABELEDITW: {
        nmtvdispinfo *d = (nmtvdispinfo *)h;
        node *n = tree_node(a, d->item.item);
        if (n && d->item.text) {
            rename_node(a, n, d->item.text);
        }
        return 0;
    }
    case TVN_KEYDOWN: {
        nmkeydown *k = (nmkeydown *)h;
        iptr item = (iptr)SendMessageW(a->tree, TVM_GETNEXTITEM, TVGN_CARET,
                                       0);
        node *n = tree_node(a, item);
        if (n && k->vkey==VK_F2) {
            tree_cmd(a, n, CMD_RENAME);
        } else if (n && n->parent && k->vkey==VK_DELETE) {
            tree_cmd(a, n, CMD_DELETE);
        }
        return 0;
    }
    }
    return 0;
}

static iz on_notify(app *a, nmhdr *h)
{
    if (h->from == a->list) {
        return on_list_notify(a, h);
    } else if (h->from == a->tree) {
        return on_tree_notify(a, h);
    } else if (h->from==a->toolbar && h->code==TBN_DROPDOWN) {
        nmtoolbar *t = (nmtoolbar *)h;
        if (t->item == CMD_ADD) {
            rect r = {0};
            SendMessageW(a->toolbar, TB_GETRECT, CMD_ADD, (iz)&r);
            point p = {r.left, r.bottom};
            ClientToScreen(a->toolbar, &p);
            iptr menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, CMD_ADDFILES, L"Add &files...");
            AppendMenuW(menu, MF_STRING, CMD_ADDFOLDER, L"Add f&older...");
            i32 c = (i32)TrackPopupMenu(menu, TPM_RETURNCMD|TPM_RIGHTBUTTON,
                                        p.x, p.y, 0, a->hwnd, 0);
            DestroyMenu(menu);
            if (c) {
                cmd(a, c);
            }
        }
        return TBDDRET_DEFAULT;
    }
    return 0;
}

static void open_node(app *a, node *n)
{
    if (n->dir) {
        navigate(a, n, 0);
        update_commands(a);
    } else if (!a->job) {
        extract_temp(a, n, THEN_OPEN);
    }
}


// The toolbar: graphical buttons, each its icon above its name

typedef struct {
    i32        cmd;
    i32        image;
    c16 const *text;
    c16 const *tip;
    b32        dropdown;
} button;

static button const buttons[] = {
    {CMD_OPEN,    0, L"Open",    L"Open an archive (Ctrl+O)", 0},
    {0,           0, 0, 0, 0},
    {CMD_ADD,     1, L"Add",     L"Add files to the archive (Ins)", 1},
    {CMD_EXTRACT, 2, L"Extract", L"Extract to a folder (Ctrl+E)", 0},
    {CMD_MOVE,    3, L"Move",    L"Extract, then delete from the archive", 0},
    {CMD_DELETE,  4, L"Delete",  L"Delete from the archive (Del)", 0},
    {CMD_RENAME,  5, L"Rename",  L"Rename (F2)", 0},
    {CMD_EDIT,    7, L"Edit",    L"Edit with a tool, then update (F4)", 0},
    {CMD_TEST,    6, L"Test",    L"Test the entries' data (Ctrl+T)", 0},
};

static void make_toolbar(app *a)
{
    while (SendMessageW(a->toolbar, TB_BUTTONCOUNT, 0, 0) > 0) {
        SendMessageW(a->toolbar, TB_DELETEBUTTON, 0, 0);
    }
    SendMessageW(a->toolbar, TB_BUTTONSTRUCTSIZE, sizeof(tbbutton), 0);
    SendMessageW(a->toolbar, TB_SETEXTENDEDSTYLE, 0,
                 TBSTYLE_EX_DRAWDDARROWS | TBSTYLE_EX_DOUBLEBUFFER);
    make_tbimages(a);
    SendMessageW(a->toolbar, TB_SETIMAGELIST, 0, (iz)a->tbimages);
    SendMessageW(a->toolbar, TB_SETDISABLEDIMAGELIST, 0, (iz)a->tbdimmed);
    tbbutton b[countof(buttons)] = {0};
    for (i32 i = 0; i < countof(buttons); i++) {
        b[i].bitmap  = buttons[i].image;
        b[i].command = buttons[i].cmd;
        b[i].state   = TBSTATE_ENABLED;
        b[i].style   = !buttons[i].cmd ? BTNS_SEP :
                       BTNS_BUTTON | BTNS_AUTOSIZE |
                       (buttons[i].dropdown ? BTNS_DROPDOWN : 0);
        b[i].string  = (iz)buttons[i].text;
    }
    SendMessageW(a->toolbar, TB_ADDBUTTONSW, countof(b), (iz)b);
    SendMessageW(a->toolbar, TB_AUTOSIZE, 0, 0);
}


// Commands

// A command on nodes within a folder, base: the list's selection in the
// folder shown, or the tree's item in its parent.
static void run_cmd(app *a, i32 id, nodes s, node *base)
{
    if (a->job || !command_on(a, id, s)) {
        return;  // one at a time
    }
    switch (id) {
    case CMD_OPEN:    cmd_open(a);  break;
    case CMD_CLOSE:   cmd_close(a); break;
    case CMD_REFRESH: reload(a);    break;
    case CMD_EXIT:    PostMessageW(a->hwnd, WM_CLOSE, 0, 0); break;
    case CMD_UP:
        if (a->folder && a->folder->parent) {
            navigate(a, a->folder->parent, a->folder);
            update_commands(a);
        }
        break;
    case CMD_SELECTALL: {
        lvitem it = {0};
        it.state = it.statemask = LVIS_SELECTED;
        SendMessageW(a->list, LVM_SETITEMSTATE, (uptr)-1, (iz)&it);
        break;
    }
    case CMD_NEW:        cmd_new(a);                     break;
    case CMD_ADD:
    case CMD_ADDFILES:   cmd_addfiles(a, a->folder);     break;
    case CMD_ADDFOLDER:  cmd_addfolder(a, a->folder);    break;
    case CMD_EXTRACT:    cmd_extract(a, s, base, 0);     break;
    case CMD_MOVE:       cmd_extract(a, s, base, 1);     break;
    case CMD_TEST:       cmd_test(a, s, base);           break;
    case CMD_DELETE:     cmd_delete(a, s);         break;
    case CMD_RENAME:     cmd_rename(a);            break;
    case CMD_PROPERTIES: cmd_properties(a, s);     break;
    case CMD_OPTIONS:    cmd_options(a);           break;
    case CMD_ABOUT:      cmd_about(a);             break;
    case CMD_OPENENTRY:
        if (s.v[0]->dir) {
            navigate(a, s.v[0], 0);
            update_commands(a);
        } else {
            extract_temp(a, s.v[0], THEN_OPEN);
        }
        break;
    case CMD_OPENWITH:   extract_temp(a, s.v[0], THEN_OPENWITH);  break;
    case CMD_EDIT:       extract_temp(a, s.v[0], THEN_EDIT);      break;
    }
}

static void cmd(app *a, i32 id)
{
    arena *tmp = &a->scratch;
    run_cmd(a, id, selection(a, tmp), a->folder);
}


// The program

static c16 const window_class[] = L"tugz";

[[gnu::force_align_arg_pointer]]
void WinMainCRTStartup(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    app *a = region_new(sizeof(void *)==8 ? (iz)1<<32 : (iz)1<<26,
                        (iz)sizeof(app));
    if (!a) {
        ExitProcess(4);
    }
    a->h.who = WHO_UI;
    os_init(&a->h.ctx);
    a->h.ctx.handles[0] = a->h.ctx.handles[1] = a->h.ctx.handles[2] = 0;
    a->h.ctx.consoles   = 0;
    a->perm    = host_perm(&a->h);
    a->scratch = host_scratch(&a->h);
    a->instance = GetModuleHandleW(0);
    a->xp = (GetVersion() & 0xff) < 6;
    load_guiapi(&a->api);
    if (a->api.setdpicontext) {
        a->api.setdpicontext(-4);  // per-monitor (version 2)
    } else if (a->api.setdpiawareness) {
        a->api.setdpiawareness(2);
    } else if (a->api.setdpiaware) {
        a->api.setdpiaware();
    }
    OleInitialize(0);
    struct { u32 size, icc; } icc = {8, ICC_WIN95_CLASSES|ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);

    c16 sep[4] = {0};
    a->sep = GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, sep, 4)
             ? sep[0] : ',';
    load_settings(a);
    a->accel = make_accelerators();

    wndclassex wc = {0};
    wc.size       = (u32)sizeof(wc);
    wc.proc       = main_proc;
    wc.instance   = a->instance;
    wc.cursor     = LoadCursorW(0, IDC_ARROW);
    wc.background = (iptr)(COLOR_BTNFACE + 1);
    wc.classname  = window_class;
    RegisterClassExW(&wc);

    iptr hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, window_class, L"tugz",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 980, 640, 0,
                                make_menu(), a->instance, a);
    // The default size, made for the monitor's DPI and to fit it
    a->dpi = window_dpi(a, hwnd);
    monitorinfo mi = {0};
    mi.size = (u32)sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    i32 w = MIN(scale(a, 980), (mi.work.right - mi.work.left)*9/10);
    i32 h = MIN(scale(a, 640), (mi.work.bottom - mi.work.top)*9/10);
    SetWindowPos(hwnd, 0, mi.work.left + (mi.work.right-mi.work.left-w)/2,
                 mi.work.top + (mi.work.bottom-mi.work.top-h)/2, w, h,
                 SWP_NOZORDER);
    load_placement(a);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // An archive named on the command line
    i32 argc = 0;
    s8 *argv = os_args(&a->perm, &argc);
    if (argc > 1) {
        arena tmp = a->scratch;
        open_zip(a, full_path(&tmp, towide(&tmp, argv[1])), (s8){0}, 0);
    } else {
        show_model(a, 0, (s8){0});
    }

    winmsg m;
    while (GetMessageW(&m, 0, 0, 0) > 0) {
        if (a->progress && IsDialogMessageW(a->progress, &m)) {
            continue;
        } else if (!a->job && TranslateAcceleratorW(a->hwnd, a->accel, &m)) {
            continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    remove_temp(a);
    OleUninitialize();
    ExitProcess(0);
}
