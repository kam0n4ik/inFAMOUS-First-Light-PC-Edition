# P0 FPS investigation: the same short driven run in several native/bridge modes, measured alike.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\perf_runs.ps1 [-Modes A,B,C,D] [-Seconds 100]
#       [-Press "space@12,space@22,shot@60,shot@95"] [-ExtraArgs "..."] [-Session name] [-DryRun]
# Modes (sema / event flags):  A native/native   B native/bridge (--bridge kernel-eventflag)
#                              C bridge/native (--bridge kernel-sema)   D bridge/bridge (both)
# Phase 2.2.3 groups:          N0 none of them (as before the phase)  N1 + Tier 1 (kernel-util,
#                              net-util, libc-mem)  N2 + kernel-mutex (mutexes, condvars)  N3 + kernel-rwlock
# Every run: same exe, same SecondSonPC.toml, a fresh copy of the same save snapshot (--user-data),
# the same key presses ("Continue" twice -> forest gameplay from ~30 s). tools\perf_sampler.ps1
# samples the process once a second while it runs.
# Safety: the real saves (userdata\home) are only read (copied once; SHA-256 compared before/after);
# refuses to start while SecondSonPC.exe runs or when free system commit < -MinFreeCommitGiB;
# every run is ended by tools\run_phase0.ps1 (WM_CLOSE to the game window of its own pid).
# Results: logs\performance\<session>\<mode>\ (logs of the run, samples.csv, frametimes.csv from
# --frame-log, summary.json) and tools\perf_report.py's report.md/.json/.csv in <session>\.
param(
    [string]$Modes = "A,D",                       # comma-separated (powershell -File cannot pass arrays)
    [int]$Seconds = 100,
    [string]$Press = "space@12,space@22,shot@60,shot@95",
    [string]$ExtraArgs = "",
    [string]$Session = "",
    [int]$MinFreeCommitGiB = 15,
    [switch]$NoFrameLog,                          # without --frame-log (per-frame times)
    [int]$SnapshotAt = 0,                         # copy logs\calls.json this many s after start
                                                  # (the last periodic dump) -> <mode>\calls_<N>s.json
    [switch]$DryRun
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$logs = "$root\logs"
$saves = "$root\userdata\home"
$modeArgs = @{
    A = ""; B = "--bridge kernel-eventflag"; C = "--bridge kernel-sema"
    D = "--bridge kernel-sema --bridge kernel-eventflag"
    # Phase 2.2.3 groups, added one at a time: N0 = state before the phase (NATIVE 79) ... N3 = all.
    N0 = "--bridge kernel-util --bridge net-util --bridge libc-mem --bridge kernel-mutex --bridge kernel-rwlock"
    N1 = "--bridge kernel-mutex --bridge kernel-rwlock"
    N2 = "--bridge kernel-rwlock"
    N3 = ""
}
$modeList = @($Modes.Split(',', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object { $_.Trim().ToUpper() })

function Fail([string]$Message) { "perf_runs: $Message"; exit 1 }
function Get-SaveHashes {
    $map = [ordered]@{}
    Get-ChildItem $saves -Recurse -File | Sort-Object FullName | ForEach-Object {
        $map[$_.FullName.Substring($saves.Length + 1)] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
    }
    $map
}

if (-not $modeList.Count -or @($modeList | Where-Object { -not $modeArgs.ContainsKey($_) }).Count) {
    Fail "-Modes must list A, B, C, D (sema/event flags) and/or N0..N3 (Phase 2.2.3 groups)"
}
if ($Seconds -lt 30 -or $Seconds -gt 300) { Fail "-Seconds must be 30..300 (short runs only)" }
if ($Press -match '\s') { Fail "-Press must not contain spaces" }
if (-not (Test-Path "$root\build\bin\SecondSonPC.exe")) { Fail "not built" }
if (Get-Process -Name SecondSonPC -ErrorAction SilentlyContinue) {
    Fail "SecondSonPC.exe is running (a game being played?) - one game at a time"
}
$os = Get-CimInstance Win32_OperatingSystem
$freeCommitGiB = [math]::Round($os.FreeVirtualMemory / 1MB, 1)
$limitGiB = [math]::Round($os.TotalVirtualMemorySize / 1MB, 1)
if (-not $Session) { $Session = Get-Date -Format "yyyy-MM-dd_HH-mm-ss" }
$out = "$logs\performance\$Session"
$exe = Get-Item "$root\build\bin\SecondSonPC.exe"
"perf_runs: modes $($modeList -join ','), $Seconds s each, presses '$Press', free commit $freeCommitGiB / $limitGiB GiB (need $MinFreeCommitGiB), exe $($exe.LastWriteTime.ToString('s')) -> $out"
if ($freeCommitGiB -lt $MinFreeCommitGiB) { Fail "free system commit $freeCommitGiB GiB < $MinFreeCommitGiB GiB" }
if ($DryRun) {
    foreach ($m in $modeList) { "  mode ${m}: --loader sspc --user-data $out\profile --vram-log 5$(if (-not $NoFrameLog) { ' --frame-log' }) --press $Press $($modeArgs[$m]) $ExtraArgs" }
    exit 0
}

New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $saves "$out\snapshot\home" -Recurse
$hashesBefore = Get-SaveHashes
$meta = [ordered]@{
    session = $Session; started = (Get-Date).ToString("s"); seconds = $Seconds; press = $Press; extra_args = $ExtraArgs
    exe = $exe.FullName; exe_time = $exe.LastWriteTime.ToString("s"); exe_size = $exe.Length
    git_head = (& git -C $root rev-parse --short HEAD); git_dirty = [bool](& git -C $root status --porcelain -- src third_party patches CMakeLists.txt)
    free_commit_gib_at_start = $freeCommitGiB; commit_limit_gib = $limitGiB; modes = $modeList
}
$meta | ConvertTo-Json | Set-Content -Encoding UTF8 "$out\session.json"

foreach ($m in $modeList) {
    $tag = "perf_${Session}_$m"
    $profileDir = "$out\profile"
    if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
    Copy-Item "$out\snapshot" $profileDir -Recurse
    $modeDir = "$out\$m"
    New-Item -ItemType Directory -Force $modeDir | Out-Null

    $sampler = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "$PSScriptRoot\perf_sampler.ps1", "-Out", "$modeDir\samples.csv")
    Remove-Item "$logs\frametimes.csv" -ErrorAction SilentlyContinue
    $snap = $null
    if ($SnapshotAt -gt 0) {
        # Waits for the game (run_phase0 removes the previous calls.json before starting it).
        $snapCmd = "`$p = `$null; for (`$i = 0; `$i -lt 150 -and -not `$p; `$i++) { `$p = Get-Process SecondSonPC -ErrorAction SilentlyContinue | Select-Object -First 1; if (-not `$p) { Start-Sleep -Milliseconds 200 } }; " +
                   "if (`$p) { `$t = `$p.StartTime.AddSeconds($SnapshotAt); while ((Get-Date) -lt `$t -and -not `$p.HasExited) { Start-Sleep -Milliseconds 200 }; " +
                   "Copy-Item '$logs\calls.json' '$modeDir\calls_${SnapshotAt}s.json' -ErrorAction SilentlyContinue }"
        $snap = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList @("-NoProfile", "-Command", $snapCmd)
    }
    $frameArg = if ($NoFrameLog) { "" } else { "--frame-log" }
    $gameArgs = "--loader sspc --user-data $profileDir --vram-log 5 $frameArg --press $Press $($modeArgs[$m]) $ExtraArgs".Trim()
    $text = & "$PSScriptRoot\run_phase0.ps1" -Seconds $Seconds -Tag $tag -NoReport -ExtraArgs $gameArgs 2>&1 | Out-String
    [void]$sampler.WaitForExit(30000)
    if ($snap) { [void]$snap.WaitForExit(30000) }
    Set-Content -Encoding UTF8 "$modeDir\run_phase0.txt" $text
    if (Test-Path "$logs\frametimes.csv") { Move-Item "$logs\frametimes.csv" "$modeDir\frametimes.csv" -Force }

    $runDir = "$logs\runs\$tag"
    if (-not (Test-Path "$runDir\run.json")) { "mode ${m}: run_phase0.ps1 wrote no run.json"; $text; exit 1 }
    Get-ChildItem $runDir -File | Copy-Item -Destination $modeDir -Force
    if (Test-Path "$runDir\shots") { Copy-Item "$runDir\shots" "$modeDir\shots" -Recurse -Force }
    $line = & python -I "$PSScriptRoot\analyze_run.py" $runDir
    Set-Content -Encoding UTF8 "$modeDir\summary.json" $line
    $s = $line | ConvertFrom-Json
    $run = Get-Content -Raw -Encoding UTF8 "$runDir\run.json" | ConvertFrom-Json
    "mode ${m} ($($modeArgs[$m])): $($run.how) exit $($run.exit_hex), frames $($s.frames), series $(($s.frames_series | ForEach-Object { $_[1] }) -join ' '), first-chance $($s.first_chance_faults), crash: $(if ($s.crash) { $s.crash } else { 'none' })"
}
if (Test-Path "$out\profile") { Remove-Item "$out\profile" -Recurse -Force }

$hashesAfter = Get-SaveHashes
$changed = @(@($hashesBefore.Keys) + @($hashesAfter.Keys) | Sort-Object -Unique | Where-Object { $hashesBefore[$_] -ne $hashesAfter[$_] })
"real saves unchanged: $(if ($changed.Count) { "NO - $($changed -join ', ')" } else { 'yes' })"
if (Test-Path "$PSScriptRoot\perf_report.py") { & python -I "$PSScriptRoot\perf_report.py" $out }
