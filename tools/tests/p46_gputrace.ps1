# SecondSonPC P4.6-G1: one short NVIDIA Nsight Graphics GPU Trace of the native backend in a game scene.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p46_gputrace.ps1 -Tag <name>
#       [-Scene user1022|beach|early] [-StartMs 70000] [-Frames 3] [-Seconds 100] [-Clocks unaltered|base|boost]
#       [-MultiPass] [-TimeEveryAction] [-Env "A=b;C=d"] [-Ngfx <path to ngfx.exe>]
# The route is the one of tools\tests\p46_runs.ps1 (fresh copy of the scene's save in logs\p46\profile - the originals
# are never written; title -> Continue at 12 s and 22 s, three camera turns at 56..60 s). ngfx launches the game with
# GPU Trace injected and starts the trace -StartMs after data collection is ready (the warmed scene after the turns),
# for at most -Frames presents. The game is closed with WM_CLOSE (tools\game_process.psm1) -Seconds after its start or
# once ngfx has finished, whichever is later. Never run beside another game run or a measurement series: the trace
# changes the GPU's timing (its FPS is not an A/B measurement).
# Artifacts -> logs\p46\gpu_profile\<Tag>\ (the .ngfx-gputrace file and ngfx's auto-export, ngfx_stdout.txt,
# SecondSonPC.log, gpu_native.txt, gpu_frames.csv, run.json).
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [ValidateSet("beach", "early", "user1022")][string]$Scene = "user1022",
    [int]$StartMs = 70000,
    [int]$Frames = 2,
    [int]$Seconds = 100,
    [ValidateSet("unaltered", "base", "boost")][string]$Clocks = "unaltered",
    [switch]$MultiPass,
    [switch]$TimeEveryAction,
    # g1_village_b: with the shader pipelines collected (default) the game's process grew by ~1.4 GiB during the
    # trace, system commit 29.3 of 32.6 GiB (no page file), the trace file stayed an 8-byte header and the game was
    # terminated from outside; -FullCollection brings that collection back
    [switch]$FullCollection,
    [string]$Env = "",
    [string]$Ngfx = "E:\tools\NsightGraphics-2026.2\ProgramFiles64Folder\NVIDIA Corporation\Nsight Graphics 2026.2.0\host\windows-desktop-nomad-x64\ngfx.exe"
)
$ErrorActionPreference = "Stop"
Import-Module "$PSScriptRoot\..\game_process.psm1" -Force
$root = "E:\SecondSonPC"
$exe = "$root\build\bin\SecondSonPC.exe"
$sources = @{
    beach    = "$root\logs\performance\p43_ab\snapshot\home"
    early    = "$root\userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc"
    user1022 = "$root\logs\p32\scene_user_1022\home"
}
if (Get-Process SecondSonPC -ErrorAction SilentlyContinue) { throw "a SecondSonPC process is already running" }
$out = "$root\logs\p46\gpu_profile\$Tag"
if (Test-Path $out) { throw "$out exists" }
New-Item -ItemType Directory -Force $out | Out-Null

$profileDir = "$root\logs\p46\profile"
if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
New-Item -ItemType Directory -Force $profileDir | Out-Null
Copy-Item $sources[$Scene] "$profileDir\home" -Recurse
foreach ($f in "gpu_device_lost.txt", "vk_validation.log", "gpu_native.txt", "gpu_frames.csv", "gpu_markers.txt") {
    Remove-Item "$root\logs\$f" -ErrorAction SilentlyContinue
}

$press = "space@12,space@22,move:-3000:0@56:0.25,move:-3000:0@58:0.25,move:-3000:0@60:0.25"
$gameArgs = "--loader sspc --user-data $profileDir --vram-log 5 --press $press --gpu-backend sspc"
$envArg = ($Env -split ';' | Where-Object { $_ } | ForEach-Object { "$_;" }) -join ' '
# no --platform: Qt takes it for its own -platform option and ngfx aborts (WER: Qt6Core 0xC0000409); the local Windows
# platform is the default. --keep-going: ngfx would terminate the game after the trace (no exit-time logs) - the game is
# closed by WM_CLOSE below instead.
$ngfxArgs = @(
    '--activity "GPU Trace Profiler"',
    "--exe `"$exe`"", "--dir `"$root`"", "--args `"$gameArgs`"",
    "--output-dir `"$out`"", "--start-after-ms $StartMs", "--limit-to-frames $Frames", "--max-duration-ms 1000",
    "--set-gpu-clocks $Clocks", "--architecture Turing", "--metric-set-id 0", "--auto-export", "--keep-going", "--verbose"
)
if (-not $FullCollection) { $ngfxArgs += @("--disable-collect-shader-pipelines", "--disable-collect-external-shader-debug-info") }
if ($envArg) { $ngfxArgs += "--env `"$envArg`"" }
if ($MultiPass) { $ngfxArgs += "--multi-pass-metrics" }
if ($TimeEveryAction) { $ngfxArgs += "--time-every-action" }
$cmdLine = $ngfxArgs -join ' '

$started = Get-Date
# through cmd's redirection (g1_village_b: Start-Process's redirection left ngfx_stdout.txt empty); $ng is that cmd
$ng = Start-Process -FilePath "$env:ComSpec" -ArgumentList "/c `"`"$Ngfx`" $cmdLine > `"$out\ngfx_stdout.txt`" 2>&1`"" `
    -WorkingDirectory $root -PassThru -WindowStyle Hidden
"ngfx (cmd pid=$($ng.Id)): $cmdLine"

# the game: the SecondSonPC process that appears after ngfx's start
$game = $null
$sw = [System.Diagnostics.Stopwatch]::StartNew()
while (-not $game -and $sw.Elapsed.TotalSeconds -lt 60) {
    $game = Get-Process SecondSonPC -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $game) { Start-Sleep -Milliseconds 250 }
    if ($ng.HasExited -and -not $game) { break }
}
if (-not $game) { "no game process (ngfx exited: $($ng.HasExited))"; Get-Content "$out\ngfx_stdout.txt" -ErrorAction SilentlyContinue; exit 1 }
$gameStart = Get-Date
"game pid=$($game.Id) after $([math]::Round($sw.Elapsed.TotalSeconds, 1)) s"

# until -Seconds of game time and ngfx done with the trace (it may keep running until the game exits)
$traceSeen = $null
while (-not $game.HasExited) {
    $t = ((Get-Date) - $gameStart).TotalSeconds
    if (-not $traceSeen -and (Get-ChildItem $out -Recurse -Filter *.ngfx-gputrace -ErrorAction SilentlyContinue)) {
        $traceSeen = [math]::Round($t, 1); "trace file seen at game time $traceSeen s"
    }
    if ($t -ge $Seconds -and ($traceSeen -or $ng.HasExited -or $t -ge $Seconds + 60)) { break }
    Start-Sleep -Milliseconds 500
}
$stop = $null
$exitedBeforeClose = $game.HasExited
if ($exitedBeforeClose) { "the game exited before the close (game time $([math]::Round(((Get-Date) - $gameStart).TotalSeconds, 1)) s)" }
if (-not $game.HasExited) {
    $shot = {
        param($w)
        Add-Type -AssemblyName System.Drawing
        $r = New-Object SspcGameWindows+RECT
        [void][SspcGameWindows]::GetWindowRect($w.Hwnd, [ref]$r)
        $wd = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
        if ($wd -gt 0 -and $ht -gt 0) {
            $bmp = New-Object System.Drawing.Bitmap $wd, $ht
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $hdc = $g.GetHdc()
            [void][SspcGameWindows]::PrintWindow($w.Hwnd, $hdc, 2)
            $g.ReleaseHdc($hdc); $g.Dispose()
            $bmp.Save("$out\screen.png", [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        }
    }.GetNewClosure()
    $stop = Stop-GameProcess -Process $game -CloseTimeoutMs 20000 -OnWindow $shot
}
"game closed: $($stop.how) action=$($stop.action)"
if (-not $ng.WaitForExit(60000)) {
    # --keep-going: ngfx waits for the next trace; the game it launched is gone - stop this ngfx (our own process)
    "ngfx still running 60 s after the game - stopped (pid $($ng.Id))"
    Stop-Process -Id $ng.Id -Confirm:$false -ErrorAction SilentlyContinue
}
foreach ($f in "gpu_device_lost.txt", "gpu_native.txt", "gpu_frames.csv", "SecondSonPC.log", "gpu_markers.txt") {
    if (Test-Path "$root\logs\$f") { Copy-Item "$root\logs\$f" $out }
}
$run = [ordered]@{
    tag = $Tag; scene = $Scene; started = $started.ToString("yyyy-MM-ddTHH:mm:ss"); ngfx = $Ngfx; ngfx_args = $cmdLine
    game_pid = $game.Id; trace_seen_game_s = $traceSeen; game_exited_before_close = $exitedBeforeClose
    ngfx_exit = $(if ($ng.HasExited) { $ng.ExitCode } else { $null })
}
if ($stop) { foreach ($k in $stop.Keys) { $run[$k] = $stop[$k] } }
$run | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 "$out\run.json"
Get-ChildItem $out -Recurse | Select-Object FullName, Length | Format-Table -AutoSize | Out-String -Width 220
