# Phase 0 test run of SecondSonPC.exe: start, wait, screenshot, close gracefully, collect evidence.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\run_phase0.ps1 [-Lang ru-RU] [-Seconds 150] [-Tag ru]
# Stops only the process it started. Closing goes through WM_CLOSE first so the final call-counter
# dump runs; the process is killed only if it does not exit within 15 s.
param(
    [string]$Lang = "",
    [int]$Seconds = 150,
    [string]$Tag = "run",
    [string]$ExtraArgs = ""   # one space-separated string (arrays do not survive powershell -File)
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$exe = "$root\build\bin\SecondSonPC.exe"
$logs = "$root\logs"
$out = "$logs\runs\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class SspcWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
"@

$argList = @()
if ($Lang) { $argList += @("--lang", $Lang) }
$argList += $ExtraArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
$p = if ($argList.Count) { Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $root -PassThru }
     else { Start-Process -FilePath $exe -WorkingDirectory $root -PassThru }
"started pid=$($p.Id) lang='$Lang' for $Seconds s"

$exited = $p.WaitForExit($Seconds * 1000)
if (-not $exited) {
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -ne [IntPtr]::Zero) {
        $r = New-Object SspcWin+RECT
        [void][SspcWin]::GetWindowRect($h, [ref]$r)
        $w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
        if ($w -gt 0 -and $hgt -gt 0) {
            $bmp = New-Object System.Drawing.Bitmap $w, $hgt
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $hdc = $g.GetHdc()
            [void][SspcWin]::PrintWindow($h, $hdc, 2) # PW_RENDERFULLCONTENT
            $g.ReleaseHdc($hdc); $g.Dispose()
            $bmp.Save("$out\screen.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
            "screenshot -> $out\screen.png ($w x $hgt)"
        }
        [void][SspcWin]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) # WM_CLOSE
    }
    if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force; "killed after WM_CLOSE timeout" }
    else { "closed gracefully, exit=$($p.ExitCode)" }
} else {
    "exited by itself, exit=$($p.ExitCode)"
}

foreach ($f in "SecondSonPC.log", "calls.json", "bridge_shadps4.log", "shad_log.txt") {
    if (Test-Path "$logs\$f") { Copy-Item "$logs\$f" "$out\$f" -Force }
}
& python -I "$root\tools\make_calls_md.py" --calls "$out\calls.json" --run-note "tag=$Tag lang='$Lang' seconds=$Seconds"
