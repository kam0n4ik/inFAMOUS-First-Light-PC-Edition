# PC Edition UI check with real (not scripted) input: the menu controller is on (no --press), so Windows input is sent
# to the game window (SendInput) and the screen is captured with the overlay windows (CopyFromScreen).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\pc_edition\esc_menu_live.ps1 -Tag live1
# Steps (seconds after start): title screens, Esc -> the game's Settings screen, hover / click the "PC settings"
# entry, Esc back, the entry's key (F), Esc back, Esc out of the game's menu. Frames -> logs\pc_edition\<Tag>\*.png
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Save = "E:\SecondSonPC\logs\performance\p43_ab\snapshot\home",
    [int]$Ready = 50,
    [string]$ExtraArgs = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$out = "$root\logs\pc_edition\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$profileDir = "$root\logs\pc_edition\profile_live"
if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
New-Item -ItemType Directory -Force $profileDir | Out-Null
Copy-Item $Save "$profileDir\home" -Recurse
Import-Module "$root\tools\game_process.psm1" -Force
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SspcLive {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, UIntPtr extra);
    public static void Key(byte scan) {
        keybd_event(0, scan, 0x0008, UIntPtr.Zero);            // KEYEVENTF_SCANCODE
        System.Threading.Thread.Sleep(60);
        keybd_event(0, scan, 0x0008 | 0x0002, UIntPtr.Zero);   // + KEYUP
    }
    public static void Click(bool right) {
        mouse_event(right ? 0x0008u : 0x0002u, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(60);
        mouse_event(right ? 0x0010u : 0x0004u, 0, 0, 0, UIntPtr.Zero);
    }
    public static void Wheel(int notches) { mouse_event(0x0800, 0, 0, notches * 120, UIntPtr.Zero); }
}
"@
$scan = @{ esc = 0x01; space = 0x39; f = 0x21; down = 0x50; up = 0x48 }

$argList = @("--loader", "sspc", "--gpu-backend", "sspc", "--user-data", $profileDir) + $ExtraArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
$p = Start-GameProcess -FilePath "$root\build\bin\SecondSonPC.exe" -ArgumentList $argList -WorkingDirectory $root
$t0 = Get-Date
function WaitUntil([double]$s) { $d = $s - ((Get-Date) - $t0).TotalSeconds; if ($d -gt 0) { Start-Sleep -Milliseconds ([int]($d * 1000)) } }
function Hwnd { $p.Refresh(); return $p.MainWindowHandle }
function Front { [void][SspcLive]::SetForegroundWindow((Hwnd)); Start-Sleep -Milliseconds 150 }
function ClientRect {
    $h = Hwnd; $r = New-Object SspcLive+RECT; [void][SspcLive]::GetClientRect($h, [ref]$r)
    $pt = New-Object SspcLive+POINT; [void][SspcLive]::ClientToScreen($h, [ref]$pt)
    return @{ X = $pt.X; Y = $pt.Y; W = $r.Right; H = $r.Bottom }
}
# a point of the game's 1920x1080 frame on the screen (the presenter keeps the proportions, centred)
function FramePoint([double]$fx, [double]$fy) {
    $c = ClientRect
    $fw = [math]::Min($c.W, $c.H * 16 / 9); $fh = $fw * 9 / 16
    $k = $fw / 1920
    return @{ X = [int]($c.X + ($c.W - $fw) / 2 + $fx * $k); Y = [int]($c.Y + ($c.H - $fh) / 2 + $fy * $k) }
}
function Shot([string]$name) {
    $c = ClientRect
    if ($c.W -le 0) { "no window for $name" | Out-Host; return }
    $bmp = New-Object System.Drawing.Bitmap $c.W, $c.H
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($c.X, $c.Y, 0, 0, (New-Object System.Drawing.Size $c.W, $c.H))
    $g.Dispose(); $bmp.Save("$out\$name.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    "{0,6:N1} s  shot $name" -f ((Get-Date) - $t0).TotalSeconds | Out-Host
}
function Step([double]$s, [string]$what, [scriptblock]$do) {
    WaitUntil $s
    if ($p.HasExited) { throw "the game exited before '$what'" }
    Front
    & $do
    "{0,6:N1} s  $what" -f ((Get-Date) - $t0).TotalSeconds | Out-Host
}

try {
    Step 12 "space (title)" { [SspcLive]::Key($scan.space) }
    Step 22 "space (continue)" { [SspcLive]::Key($scan.space) }
    Step $Ready "esc (the game's Settings screen)" { [SspcLive]::Key($scan.esc) }
    WaitUntil ($Ready + 4); Shot "01_settings_screen"
    Step ($Ready + 5) "wheel down x2 (the game's list)" { [SspcLive]::Wheel(-1); Start-Sleep -Milliseconds 250; [SspcLive]::Wheel(-1) }
    WaitUntil ($Ready + 6.5); Shot "02_after_wheel"
    Step ($Ready + 7) "mouse over the PC settings entry" { $pt = FramePoint 1560 968; [void][SspcLive]::SetCursorPos($pt.X, $pt.Y) }
    WaitUntil ($Ready + 8); Shot "03_entry_hover"
    Step ($Ready + 9) "click the entry" { [SspcLive]::Click($false) }
    WaitUntil ($Ready + 11); Shot "04_pc_settings_open"
    Step ($Ready + 12) "wheel in the PC settings" { [SspcLive]::Wheel(-1) }
    WaitUntil ($Ready + 13); Shot "05_pc_settings_wheel"
    Step ($Ready + 14) "esc (back to the game's Settings)" { [SspcLive]::Key($scan.esc) }
    WaitUntil ($Ready + 16); Shot "06_back_to_settings"
    Step ($Ready + 17) "F (the entry's key)" { [SspcLive]::Key($scan.f) }
    WaitUntil ($Ready + 19); Shot "07_pc_settings_by_key"
    Step ($Ready + 20) "esc (back)" { [SspcLive]::Key($scan.esc) }
    Step ($Ready + 22) "right click (circle: leave the game's Settings)" { [SspcLive]::Click($true) }
    WaitUntil ($Ready + 25); Shot "08_back_in_game"
    WaitUntil ($Ready + 27)
} finally {
    $stop = Stop-GameProcess -Process $p -CloseTimeoutMs 15000
    Copy-Item "$root\logs\SecondSonPC.log" $out -ErrorAction SilentlyContinue
    "stopped: $stop" | Out-Host
}
