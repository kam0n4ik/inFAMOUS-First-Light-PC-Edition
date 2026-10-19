# PC Settings UI checks in the real game, one step per call (docs/PC_SETTINGS_UI.md §10): the worktree's own exe, its
# own logs/cache (the root is the worktree), a copy of a save and its own settings file; real Windows input
# (SendInput-style key/mouse events) to the game window and screen captures of the window with the overlay on it.
#   settings_ui_drive.ps1 -Do start   [-Save <profile dir>] [-Settings <file>] [-ExtraArgs "..."]
#   settings_ui_drive.ps1 -Do key     -Keys esc,down,enter,...     (scan codes below; "wait:<ms>" between)
#   settings_ui_drive.ps1 -Do move    -X <frame x> -Y <frame y>   (a point of the 1920x1080 frame)
#   settings_ui_drive.ps1 -Do client  -X <client x> -Y <client y>  (a client pixel)
#   settings_ui_drive.ps1 -Do click   [-X .. -Y ..] [-Right] [-Client]
#   settings_ui_drive.ps1 -Do wheel   -N <notches (+ up)>
#   settings_ui_drive.ps1 -Do shot    -Name <png name>
#   settings_ui_drive.ps1 -Do size    -X <client w> -Y <client h>  (resizes the window: Win32, as a user drags it)
#   settings_ui_drive.ps1 -Do status | stop
# The game lock (E:\research\sspc_game.lock) is the caller's business: take it before "start", free it after "stop".
param(
    [Parameter(Mandatory = $true)][ValidateSet('start', 'key', 'move', 'client', 'click', 'wheel', 'shot', 'size',
                                               'status', 'stop')][string]$Do,
    [string[]]$Keys = @(),
    [double]$X = -1,
    [double]$Y = -1,
    [int]$N = 0,
    [switch]$Right,
    [switch]$Client,
    [string]$Name = "shot",
    [string]$Save = "E:\SecondSonPC-settings-ui\.tmp\ingame\profile",
    [string]$Settings = "E:\SecondSonPC-settings-ui\.tmp\ingame\settings.toml",
    [string]$Out = "E:\SecondSonPC-settings-ui\.tmp\ingame\shots",
    [string]$ExtraArgs = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC-settings-ui"
$exe = "$root\build\bin\SecondSonPC.exe"
$state = "$root\.tmp\ingame\pid.txt"
New-Item -ItemType Directory -Force $Out | Out-Null
Import-Module "$root\tools\game_process.psm1" -Force
Add-Type -AssemblyName System.Drawing
if (-not ('SspcDrive' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SspcDrive {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hh, bool repaint);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, UIntPtr extra);
    public static void Key(byte scan, bool ext) {
        uint e = ext ? 0x0001u : 0u;                                // KEYEVENTF_EXTENDEDKEY (arrows, Delete...)
        keybd_event(0, scan, 0x0008 | e, UIntPtr.Zero);            // KEYEVENTF_SCANCODE
        System.Threading.Thread.Sleep(70);
        keybd_event(0, scan, 0x0008 | 0x0002 | e, UIntPtr.Zero);   // + KEYUP
    }
    public static void Click(bool right) {
        mouse_event(right ? 0x0008u : 0x0002u, 0, 0, 0, UIntPtr.Zero);
        System.Threading.Thread.Sleep(70);
        mouse_event(right ? 0x0010u : 0x0004u, 0, 0, 0, UIntPtr.Zero);
    }
    public static void Wheel(int notches) { mouse_event(0x0800, 0, 0, notches * 120, UIntPtr.Zero); }
}
"@
}
# set 1 scan codes; "e:" = an extended key
$scan = @{
    esc = 0x01; enter = 0x1C; space = 0x39; tab = 0x0F; q = 0x10; e = 0x12; w = 0x11; a = 0x1E; s = 0x1F; d = 0x20
    f = 0x21; f10 = 0x44; f11 = 0x57; bksp = 0x0E; lshift = 0x2A
    up = 'e:0x48'; down = 'e:0x50'; left = 'e:0x4B'; right = 'e:0x4D'; del = 'e:0x53'; pgup = 'e:0x49'; pgdn = 'e:0x51'
}

function Game {
    if (-not (Test-Path $state)) { throw "no game started by this script" }
    $id = [int](Get-Content $state)
    $p = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $p -or $p.Path -ne $exe) { throw "the started game ($id) is not running" }
    return $p
}
function Window($p) {
    $sel = Select-GameWindow -Candidates @(Get-GameWindowCandidates -Process $p)
    if (-not $sel.Window) { throw "no game window ($($sel.Reason))" }
    return $sel.Window.Hwnd
}
function Front($h) {
    if ([SspcDrive]::IsIconic($h)) { [void][SspcDrive]::ShowWindow($h, 9); Start-Sleep -Milliseconds 400 }   # SW_RESTORE: started minimized
    if ([SspcDrive]::GetForegroundWindow() -ne $h) {
        # a keypress of Alt lets this process give the focus away (the foreground lock)
        [SspcDrive]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [SspcDrive]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
        [void][SspcDrive]::SetForegroundWindow($h)
        Start-Sleep -Milliseconds 250
    }
}
function ClientRect($h) {
    $r = New-Object SspcDrive+RECT; [void][SspcDrive]::GetClientRect($h, [ref]$r)
    $pt = New-Object SspcDrive+POINT; [void][SspcDrive]::ClientToScreen($h, [ref]$pt)
    return @{ X = $pt.X; Y = $pt.Y; W = $r.Right; H = $r.Bottom }
}
function FramePoint($h, [double]$fx, [double]$fy) {
    $c = ClientRect $h
    $fw = [math]::Min($c.W, $c.H * 16 / 9); $fh = $fw * 9 / 16
    $k = $fw / 1920
    return @{ X = [int]($c.X + ($c.W - $fw) / 2 + $fx * $k); Y = [int]($c.Y + ($c.H - $fh) / 2 + $fy * $k) }
}

function T($p) { try { "t={0:N1}s " -f ((Get-Date) - $p.StartTime).TotalSeconds } catch { "" } }

switch ($Do) {
    'start' {
        if (Get-Process -Name SecondSonPC -ErrorAction SilentlyContinue) { throw "a SecondSonPC.exe is running: not started" }
        $lock = "E:\research\sspc_game.lock\owner.txt"
        if (-not (Test-Path $lock) -or -not ((Get-Content $lock) -like "settings-ui*")) { throw "take the game lock first" }
        $argList = @("--user-data", $Save, "--settings", $Settings, "--no-shader-prepare") +
                   $ExtraArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
        $p = Start-GameProcess -FilePath $exe -ArgumentList $argList -WorkingDirectory $root -WindowStyle Minimized
        Set-Content $state $p.Id
        "started pid $($p.Id): $($argList -join ' ')"
    }
    'status' {
        $p = Game
        $h = Window $p
        $c = ClientRect $h
        "$(T $p)pid $($p.Id), working set $([math]::Round($p.WorkingSet64 / 1MB)) MiB, private $([math]::Round($p.PrivateMemorySize64 / 1MB)) MiB, client $($c.W)x$($c.H) at $($c.X),$($c.Y), foreground $([SspcDrive]::GetForegroundWindow() -eq $h)"
    }
    'key' {
        $p = Game; $h = Window $p; Front $h
        foreach ($k in $Keys) {
            if ($k -like 'wait:*') { Start-Sleep -Milliseconds ([int]$k.Substring(5)); continue }
            $v = $scan[$k]
            if ($null -eq $v) { throw "unknown key $k" }
            $ext = $false
            if ($v -is [string]) { $ext = $true; $v = [Convert]::ToInt32($v.Substring(4), 16) }
            [SspcDrive]::Key([byte]$v, $ext)
            Start-Sleep -Milliseconds 140
        }
        "$(T $p)keys: $($Keys -join ' ')"
    }
    { $_ -in 'move', 'client', 'click' } {
        $p = Game; $h = Window $p; Front $h
        if ($X -ge 0 -and $Y -ge 0) {
            if ($Do -eq 'client' -or $Client) {
                $c = ClientRect $h; $pt = @{ X = [int]($c.X + $X); Y = [int]($c.Y + $Y) }
            } else { $pt = FramePoint $h $X $Y }
            [void][SspcDrive]::SetCursorPos($pt.X, $pt.Y)
            Start-Sleep -Milliseconds 160
        }
        if ($Do -eq 'click') { [SspcDrive]::Click([bool]$Right); Start-Sleep -Milliseconds 120 }
        "$(T $p)$Do at $X,$Y"
    }
    'wheel' {
        $p = Game; $h = Window $p; Front $h
        [SspcDrive]::Wheel($N); Start-Sleep -Milliseconds 150
        "$(T $p)wheel $N"
    }
    'size' {
        $p = Game; $h = Window $p
        $wr = New-Object SspcDrive+RECT; [void][SspcDrive]::GetWindowRect($h, [ref]$wr)
        $c = ClientRect $h
        $bw = ($wr.Right - $wr.Left) - $c.W; $bh = ($wr.Bottom - $wr.Top) - $c.H
        [void][SspcDrive]::MoveWindow($h, 40, 40, [int]$X + $bw, [int]$Y + $bh, $true)
        Start-Sleep -Milliseconds 600
        $c = ClientRect $h
        "client now $($c.W)x$($c.H)"
    }
    'shot' {
        $p = Game; $h = Window $p; Front $h
        Start-Sleep -Milliseconds 250
        $c = ClientRect $h
        $bmp = New-Object System.Drawing.Bitmap $c.W, $c.H
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($c.X, $c.Y, 0, 0, (New-Object System.Drawing.Size $c.W, $c.H))
        $g.Dispose(); $bmp.Save("$Out\$Name.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        "$(T $p)shot $Out\$Name.png ($($c.W)x$($c.H))"
    }
    'stop' {
        $p = Game
        $r = Stop-GameProcess -Process $p -CloseTimeoutMs 20000
        Remove-Item $state -ErrorAction SilentlyContinue
        "stopped: $($r.how) exit $($r.exit_hex) ($($r.action))"
    }
}
