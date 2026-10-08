# Stand-in for SecondSonPC.exe in tools\tests\game_process_tests.ps1 (never started by the game
# scripts): like the game it is a console process (powershell.exe, own console window) with a
# top-level window of class SDL_app. The window is shown without taking the focus.
#   -Mode normal          WM_CLOSE -> DestroyWindow -> exit 0
#   -Mode ignore-close    WM_CLOSE is ignored (terminate-after-timeout path)
#   -Mode crash-on-close  WM_CLOSE -> ExitProcess(-ExitCode)
#   -Mode exit-early      ExitProcess(-ExitCode) 1 s after the window appears
#   -Mode two-windows     two SDL_app windows, 640x480 and 320x240; WM_CLOSE to either -> exit 0
#   -Mode no-window       no window at all, waits until terminated
param(
    [ValidateSet('normal', 'ignore-close', 'crash-on-close', 'exit-early', 'two-windows', 'no-window')]
    [string]$Mode = 'normal',
    [uint32]$ExitCode = 0  # decimal: 0xC0000005 = 3221225477
)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class FakeGameWindow {
    delegate IntPtr WndProc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct WNDCLASSEX {
        public uint cbSize; public uint style; public WndProc lpfnWndProc; public int cbClsExtra; public int cbWndExtra;
        public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground;
        public string lpszMenuName; public string lpszClassName; public IntPtr hIconSm;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public int x, y; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern ushort RegisterClassEx(ref WNDCLASSEX wc);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr CreateWindowEx(uint exStyle, string cls, string title,
        uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr param);
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr hwnd, int cmd);
    [DllImport("user32.dll")] static extern int GetMessage(out MSG msg, IntPtr hwnd, uint min, uint max);
    [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll")] static extern IntPtr DispatchMessage(ref MSG msg);
    [DllImport("user32.dll")] static extern IntPtr DefWindowProc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern void PostQuitMessage(int code);
    [DllImport("user32.dll")] static extern UIntPtr SetTimer(IntPtr hwnd, UIntPtr id, uint ms, IntPtr callback);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern IntPtr GetModuleHandle(string name);
    [DllImport("kernel32.dll")] static extern void ExitProcess(uint code);

    const uint WM_DESTROY = 0x0002, WM_CLOSE = 0x0010, WM_TIMER = 0x0113;
    static readonly WndProc Proc = WindowProc; // must stay alive as long as the windows
    static string mode;
    static uint exitCode;

    static IntPtr WindowProc(IntPtr hwnd, uint msg, IntPtr w, IntPtr l) {
        switch (msg) {
        case WM_CLOSE:
            if (mode == "ignore-close") return IntPtr.Zero;
            if (mode == "crash-on-close") ExitProcess(exitCode);
            DestroyWindow(hwnd);
            return IntPtr.Zero;
        case WM_DESTROY:
            PostQuitMessage(0);
            return IntPtr.Zero;
        case WM_TIMER:
            ExitProcess(exitCode);
            return IntPtr.Zero;
        }
        return DefWindowProc(hwnd, msg, w, l);
    }

    public static void Run(string runMode, uint code) {
        mode = runMode;
        exitCode = code;
        IntPtr instance = GetModuleHandle(null);
        var wc = new WNDCLASSEX();
        wc.cbSize = (uint)Marshal.SizeOf(typeof(WNDCLASSEX));
        wc.lpfnWndProc = Proc;
        wc.hInstance = instance;
        wc.lpszClassName = "SDL_app";
        if (RegisterClassEx(ref wc) == 0) ExitProcess(2);
        int count = mode == "no-window" ? 0 : mode == "two-windows" ? 2 : 1;
        for (int i = 0; i < count; ++i) {
            IntPtr hwnd = CreateWindowEx(0, "SDL_app", "SecondSonPC test stand-in", 0x00CF0000 /* WS_OVERLAPPEDWINDOW */,
                40 + i * 40, 40 + i * 40, i == 0 ? 640 : 320, i == 0 ? 480 : 240, IntPtr.Zero, IntPtr.Zero, instance, IntPtr.Zero);
            if (hwnd == IntPtr.Zero) ExitProcess(2);
            ShowWindow(hwnd, 4); // SW_SHOWNOACTIVATE
            if (i == 0 && mode == "exit-early") SetTimer(hwnd, (UIntPtr)1, 1000, IntPtr.Zero);
        }
        MSG m;
        while (GetMessage(out m, IntPtr.Zero, 0, 0) > 0) {
            TranslateMessage(ref m);
            DispatchMessage(ref m);
        }
        ExitProcess(0);
    }
}
"@
[FakeGameWindow]::Run($Mode, $ExitCode)
