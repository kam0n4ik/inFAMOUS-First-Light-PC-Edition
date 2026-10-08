# Phase 0 test run of SecondSonPC.exe: start, wait, screenshot, close gracefully, collect evidence.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\run_phase0.ps1 [-Lang ru-RU] [-Seconds 150] [-Tag ru]
# Stops only the process it started (tools\game_process.psm1): WM_CLOSE goes to that process's game
# window, never to its console window (that ended runs with 0xC000013A); the process is terminated
# only when it has no game window or does not exit within -CloseTimeout seconds, so the final
# call-counter dump runs. logs\runs\<tag>\run.json records the pid, the window, what the script did,
# the exit code and the classification ("how": closed-by-test, crashed, killed-close-timeout, ...).
param(
    [string]$Lang = "",
    [int]$Seconds = 150,
    [string]$Tag = "run",
    [string]$ExtraArgs = "",  # one space-separated string (arrays do not survive powershell -File)
    [int]$CloseTimeout = 15,  # seconds between WM_CLOSE and TerminateProcess
    [switch]$NoReport,        # do not regenerate docs\generated\CALLS.md (diagnostic runs)
    # Only for tools\tests\game_process_tests.ps1 (stand-in program, scratch logs folder):
    [string]$Exe = "E:\SecondSonPC\build\bin\SecondSonPC.exe",
    [string]$LogsDir = "E:\SecondSonPC\logs"
)
$ErrorActionPreference = "Stop"
Import-Module "$PSScriptRoot\game_process.psm1" -Force
$root = "E:\SecondSonPC"
$logs = $LogsDir
$out = "$logs\runs\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
# Outputs of an earlier session must not be mistaken for this run's.
foreach ($f in "calls.json", "call_order.txt") { Remove-Item "$logs\$f" -ErrorAction SilentlyContinue }
Remove-Item "$logs\shots" -Recurse -ErrorAction SilentlyContinue # --press shot@sec frames
Remove-Item "$out\run.json", "$out\screen.png" -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Drawing

$argList = @()
if ($Lang) { $argList += @("--lang", $Lang) }
$argList += $ExtraArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)
$p = Start-GameProcess -FilePath $exe -ArgumentList $argList -WorkingDirectory $root
$started = Get-Date
"started pid=$($p.Id) lang='$Lang' for $Seconds s"

$exitedEarly = $p.WaitForExit($Seconds * 1000)

# Screenshot of the chosen game window, taken just before WM_CLOSE.
$shot = {
    param($w)
    $r = New-Object SspcGameWindows+RECT
    [void][SspcGameWindows]::GetWindowRect($w.Hwnd, [ref]$r)
    $wd = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($wd -gt 0 -and $ht -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap $wd, $ht
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        [void][SspcGameWindows]::PrintWindow($w.Hwnd, $hdc, 2) # PW_RENDERFULLCONTENT
        $g.ReleaseHdc($hdc); $g.Dispose()
        $bmp.Save("$out\screen.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        "screenshot -> $out\screen.png ($wd x $ht)" | Out-Host
    }
}.GetNewClosure()

$stop = Stop-GameProcess -Process $p -CloseTimeoutMs ($CloseTimeout * 1000) -OnWindow $shot
$lifetime = $null
try { $lifetime = [math]::Round(($p.ExitTime - $p.StartTime).TotalSeconds, 1) } catch { }

$run = [ordered]@{
    tag = $Tag; exe = $exe; args = ($argList -join ' '); started = $started.ToString("yyyy-MM-ddTHH:mm:ss")
    run_seconds = $Seconds; close_timeout_s = $CloseTimeout; exited_before_deadline = $exitedEarly
    lifetime_s = $lifetime
}
foreach ($k in $stop.Keys) { $run[$k] = $stop[$k] }
$run | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "$out\run.json"

if ($run.window) {
    "window: hwnd=$($run.window.hwnd) class=$($run.window.class) thread=$($run.window.thread_id) size=$($run.window.size) ($($run.window_reason))"
} elseif (-not $exitedEarly) {
    "window: none ($($run.window_reason))"
}
foreach ($c in $run.candidates) {
    if ($c.rejected) { "  not the game window: $($c.hwnd) class=$($c.class) - $($c.rejected)" }
}
if ($run.close_posted) { "close: WM_CLOSE posted to $($run.window.hwnd), waited $($run.close_wait_ms) ms" }
elseif ($run.close_error) { "close: PostMessage failed, Win32 error $($run.close_error)" }
if ($run.kill_reason) {
    $err = if ($run.kill_error) { "; $($run.kill_error)" } else { "" }
    "terminate: $($run.kill_reason), waited $($run.kill_wait_ms) ms$err"
}
"result: how=$($run.how) exit=$($run.exit_code) ($($run.exit_hex) $($run.exit_name)) lifetime=$lifetime s"
"  $($run.note)"

foreach ($f in "SecondSonPC.log", "calls.json", "call_order.txt", "bridge_shadps4.log", "shad_log.txt") {
    if (Test-Path "$logs\$f") { Copy-Item "$logs\$f" "$out\$f" -Force }
}
if (Test-Path "$logs\shots") {
    Remove-Item "$out\shots" -Recurse -ErrorAction SilentlyContinue
    Copy-Item "$logs\shots" "$out\shots" -Recurse -Force
}
if (-not $NoReport) {
    & python -I "$root\tools\make_calls_md.py" --calls "$out\calls.json" --run-note "tag=$Tag lang='$Lang' seconds=$Seconds"
}
