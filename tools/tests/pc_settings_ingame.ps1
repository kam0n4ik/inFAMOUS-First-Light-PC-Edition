# SecondSonPC PC settings: one game run that drives the settings menu (F1) and the keyboard/mouse through the OPERATING
# SYSTEM's input (SendInput scan codes, relative mouse moves), not through --press (which turns the menu off).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\pc_settings_ingame.ps1 -Out <dir>
#       [-Phase change|verify] [-Scene beach] [-Exe <path>] [-Env "A=b;C=d"] [-Seconds 80]
# change: title -> Continue (Space at 12 and 22 s, like the P4.6 route), W held (movement), mouse moves (camera),
#         F1 -> menu, W held while the menu is open (must not reach the game), Tab x3 -> Mouse, Right x2 (mouse
#         sensitivity +0.10), F1 -> closed, W held again (no stuck keys), WM_CLOSE. Settings go to <dir>\..\settings.toml
#         (--settings), never to %LOCALAPPDATA%.
# verify: the same start with the same --settings file, no input; the log must show the override loaded.
# Input is sent ONLY while the game window is the foreground window (checked before every event); otherwise the step
# is skipped and recorded. Screenshots: the screen area of the game window (CopyFromScreen - the menu's layered windows
# are in it), <dir>\shot_<n>_<what>.png. Actions and checks: <dir>\actions.txt. The save is a fresh copy, as in p46_runs.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [ValidateSet("change", "verify")][string]$Phase = "change",
    [string]$Scene = "beach",
    [string]$Exe = "E:\SecondSonPC\build\bin\SecondSonPC.exe",
    [string]$Env = "",
    [int]$Seconds = 80
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
Import-Module "$root\tools\game_process.psm1" -Force
Add-Type -AssemblyName System.Drawing
if (-not ('SspcSendInput' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class SspcSendInput {
    [StructLayout(LayoutKind.Sequential)] struct MOUSEINPUT { public int dx, dy; public uint data, flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] struct INPUT { public uint type; public UNION u; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    const uint KEYEVENTF_EXTENDEDKEY = 1, KEYEVENTF_KEYUP = 2, KEYEVENTF_SCANCODE = 8, MOUSEEVENTF_MOVE = 1;
    public static uint Key(ushort scan, bool extended, bool up) {
        var i = new INPUT[1];
        i[0].type = 1;
        i[0].u.ki.scan = scan;
        i[0].u.ki.flags = KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    public static uint Move(int dx, int dy) {
        var i = new INPUT[1];
        i[0].type = 0;
        i[0].u.mi.dx = dx; i[0].u.mi.dy = dy; i[0].u.mi.flags = MOUSEEVENTF_MOVE;
        return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
    // The usual way to take the foreground from a background process: share the foreground thread's input state.
    public static bool Focus(IntPtr hwnd) {
        if (GetForegroundWindow() == hwnd) return true;
        uint pid;
        uint fg = GetWindowThreadProcessId(GetForegroundWindow(), out pid);
        uint me = GetCurrentThreadId();
        bool attached = fg != 0 && fg != me && AttachThreadInput(me, fg, true);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        if (attached) AttachThreadInput(me, fg, false);
        return GetForegroundWindow() == hwnd;
    }
}
"@
}
# scan codes (set 1): Space 39, W 11, F1 3B, Tab 0F, Right E0 4D
$keys = @{ Space = @(0x39, $false); W = @(0x11, $false); F1 = @(0x3B, $false); Tab = @(0x0F, $false); Right = @(0x4D, $true) }

New-Item -ItemType Directory -Force $Out | Out-Null
$settings = Join-Path (Split-Path $Out -Parent) "settings.toml"
$actions = "$Out\actions.txt"
"pc_settings_ingame phase $Phase, scene $Scene, exe $Exe, env '$Env', settings $settings ($(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))" |
    Out-File $actions -Encoding utf8
$sources = @{
    beach    = "$root\logs\performance\p43_ab\snapshot\home"
    early    = "$root\userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc"
    user1022 = "$root\logs\p32\scene_user_1022\home"
}
$profileDir = "$root\logs\pc_settings_integration\profile"
if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
New-Item -ItemType Directory -Force $profileDir | Out-Null
Copy-Item $sources[$Scene] "$profileDir\home" -Recurse
foreach ($f in "gpu_native.txt", "gpu_frames.csv", "gpu_device_lost.txt") { Remove-Item "$root\logs\$f" -ErrorAction SilentlyContinue }
if (Get-Process -Name SecondSonPC -ErrorAction SilentlyContinue) {
    "SecondSonPC.exe is already running - stop" | Out-Host
    Add-Content -Path $actions -Value "SecondSonPC.exe is already running - stop" -Encoding utf8
    exit 2
}

$saved = @{}
foreach ($kv in ($Env -split ';' | Where-Object { $_ })) {
    $n, $v = $kv -split '=', 2
    $saved[$n] = [Environment]::GetEnvironmentVariable($n)
    [Environment]::SetEnvironmentVariable($n, $v)
}
$argList = @("--loader", "sspc", "--gpu-backend", "sspc", "--user-data", $profileDir, "--vram-log", "5", "--settings", $settings)
try {
    $p = Start-GameProcess -FilePath $Exe -ArgumentList $argList -WorkingDirectory $root
} finally {
    foreach ($n in $saved.Keys) { [Environment]::SetEnvironmentVariable($n, $saved[$n]) }
}
$t0 = Get-Date
function Log([string]$s) {
    $line = "{0,6:N1} s  {1}" -f ((Get-Date) - $t0).TotalSeconds, $s
    Add-Content -Path $actions -Value $line -Encoding utf8 # Tee-Object -Append would add UTF-16 in PowerShell 5.1
    $line | Out-Host
}
function WaitUntil([double]$sec) { while (((Get-Date) - $t0).TotalSeconds -lt $sec -and -not $p.HasExited) { Start-Sleep -Milliseconds 50 } }
$script:hwnd = [IntPtr]::Zero
function Window {
    if ($script:hwnd -eq [IntPtr]::Zero) {
        $sel = Select-GameWindow -Candidates @(Get-GameWindowCandidates -Process $p)
        if ($sel.Window) { $script:hwnd = $sel.Window.Hwnd; Log ("game window 0x{0:X} ({1})" -f $script:hwnd.ToInt64(), $sel.Reason) }
    }
    $script:hwnd
}
function Foreground {
    $h = Window
    if ($h -eq [IntPtr]::Zero) { return $false }
    if ([SspcSendInput]::GetForegroundWindow() -eq $h) { return $true }
    $ok = [SspcSendInput]::Focus($h)
    Log "foreground requested: $ok"
    $ok
}
$script:skipped = 0
function Tap([string]$k, [int]$holdMs = 80) {
    if (-not (Foreground)) { $script:skipped++; Log "SKIPPED $k (the game window is not in the foreground)"; return }
    $sc, $ext = $keys[$k]
    [void][SspcSendInput]::Key([uint16]$sc, $ext, $false)
    Start-Sleep -Milliseconds $holdMs
    [void][SspcSendInput]::Key([uint16]$sc, $ext, $true) # the release always goes, even if focus moved meanwhile
    Log "key $k ($holdMs ms)"
}
function MoveMouse([int]$dx, [int]$steps) {
    if (-not (Foreground)) { $script:skipped++; Log "SKIPPED mouse (the game window is not in the foreground)"; return }
    for ($i = 0; $i -lt $steps; $i++) { [void][SspcSendInput]::Move($dx, 0); Start-Sleep -Milliseconds 16 }
    Log "mouse dx $dx x $steps"
}
$script:shot = 0
function Shot([string]$what) {
    $h = Window
    if ($h -eq [IntPtr]::Zero) { return }
    $r = New-Object SspcGameWindows+RECT
    [void][SspcGameWindows]::GetWindowRect($h, [ref]$r)
    $wd = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($wd -le 0 -or $ht -le 0) { return }
    $script:shot++
    $bmp = New-Object System.Drawing.Bitmap $wd, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size)
    $g.Dispose()
    $file = "$Out\shot_{0:D2}_{1}.png" -f $script:shot, $what
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    Log "screenshot $file ($wd x $ht, foreground $([SspcSendInput]::GetForegroundWindow() -eq $h))"
}

Log "started pid $($p.Id): $($argList -join ' ')"
if ($Phase -eq "change") {
    WaitUntil 8;  [void](Foreground)
    WaitUntil 12; Tap Space
    WaitUntil 22; Tap Space
    WaitUntil 45; Shot "gameplay"
    WaitUntil 46; Tap W 1500; Shot "after_W"
    WaitUntil 50; MoveMouse 40 25; Start-Sleep -Milliseconds 300; Shot "after_mouse"
    WaitUntil 54; Tap F1; Start-Sleep -Milliseconds 700; Shot "menu_open"
    Tap W 1200; Start-Sleep -Milliseconds 300; Shot "menu_W_held"
    Tap Tab; Tap Tab; Tap Tab; Start-Sleep -Milliseconds 300; Shot "menu_mouse_section"
    Tap Right; Tap Right; Start-Sleep -Milliseconds 400; Shot "menu_sensitivity_changed"
    Tap F1; Start-Sleep -Milliseconds 700; Shot "menu_closed"
    WaitUntil 66; Tap W 1200; Start-Sleep -Milliseconds 1500; Shot "after_menu_W_released"
    MoveMouse -40 25; Start-Sleep -Milliseconds 300; Shot "after_menu_mouse"
}
WaitUntil $Seconds
Log "closing (WM_CLOSE)"
$stop = Stop-GameProcess -Process $p -CloseTimeoutMs 15000
Log "ended: how=$($stop.how) exit=$($stop.exit_code) ($($stop.exit_hex) $($stop.exit_name)); skipped input steps: $script:skipped"
foreach ($f in "SecondSonPC.log", "gpu_native.txt", "gpu_frames.csv", "gpu_device_lost.txt") {
    if (Test-Path "$root\logs\$f") { Copy-Item "$root\logs\$f" $Out -Force }
}
if (Test-Path $settings) { Copy-Item $settings "$Out\settings.toml.after" -Force; Log "settings file:"; Get-Content $settings | ForEach-Object { Log "  $_" } }
else { Log "settings file: none" }
$log = "$Out\SecondSonPC.log"
foreach ($pat in "user settings", "settings menu", "setting input\.", "\[Error\]", "DingDong") {
    $n = @(Select-String -Path $log -Pattern $pat -ErrorAction SilentlyContinue).Count
    Log ("log '{0}': {1} line(s)" -f $pat, $n)
}
Select-String -Path $log -Pattern "user settings|settings menu|setting input\." -ErrorAction SilentlyContinue |
    Select-Object -First 30 | ForEach-Object { Log ("  " + $_.Line) }
