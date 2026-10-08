# SSPC-002 hunt (docs\KNOWN_ISSUES.md): a bounded series of short game runs on a copy of the saves,
# alternating loaders; stops at the first crash and keeps all of its evidence.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\sspc002_runs.ps1 [-Runs 10] [-Seconds 60]
#       [-Loaders sspc,shadps4] [-Press "space@20"] [-KeepGoing] [-DryRun]
# SSPC-002 struck while a checkpoint loaded, ~15 s after start, before gameplay (~45 s without any
# input), so the default run needs no key presses.
# Safety:
#  - the real saves (userdata\home) are only read: copied once to userdata\sspc002\snapshot_<time>;
#    every run starts from a fresh copy of that snapshot (--user-data userdata\sspc002\run), and the
#    SHA-256 of the real saves is compared before and after the series;
#  - one game at a time: refuses to start while SecondSonPC.exe is running or when the free system
#    commit is below -MinFreeCommitGiB (this PC has no page file: SSPC-001);
#  - every run is ended by tools\run_phase0.ps1 (WM_CLOSE to the game window of its own pid);
#  - at most 50 runs; nothing is deleted except the previous run's profile copy when that run did
#    not crash. A crashed run's profile is kept as userdata\sspc002\crash_<tag>.
# Results: logs\runs\<Prefix>_<loader>_<n>\ (SecondSonPC.log with the first-chance report,
# bridge_shadps4.log, run.json), the minidump in logs\crash\, logs\runs\<Prefix>_summary.jsonl.
# Exit code: 0 no crash, 2 a crash was caught, 1 the series could not run.
param(
    [int]$Runs = 10,
    [int]$Seconds = 60,
    [string]$Loaders = "sspc,shadps4", # comma-separated (powershell -File cannot pass arrays)
    [string]$Press = "",               # --press script for every run, e.g. "space@20"
    [string]$Prefix = "sspc002",
    [int]$MinFreeCommitGiB = 15,
    [switch]$KeepGoing,                # do not stop at the first crash
    [switch]$DryRun,                   # print the plan and check the preconditions only
    # Only for tools\tests\game_process_tests.ps1: stand-in program, its leading arguments, a
    # scratch root with logs\ and userdata\home\.
    [string]$Exe = "",
    [string]$ExeArgsPrefix = "",
    [string]$Root = "E:\SecondSonPC"
)
$ErrorActionPreference = "Stop"
$logs = "$Root\logs"
$saves = "$Root\userdata\home"
$work = "$Root\userdata\sspc002"
$profileDir = "$work\run"
$summary = "$logs\runs\${Prefix}_summary.jsonl"
$loaderList = @($Loaders.Split(',', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object { $_.Trim() })

function Fail([string]$Message) { "sspc002_runs: $Message"; exit 1 }

function Get-SaveHashes {
    $map = [ordered]@{}
    if (Test-Path $saves) {
        Get-ChildItem $saves -Recurse -File | Sort-Object FullName | ForEach-Object {
            $map[$_.FullName.Substring($saves.Length + 1)] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
        }
    }
    $map
}

# --- preconditions --------------------------------------------------------------------------
if ($Runs -lt 1 -or $Runs -gt 50) { Fail "-Runs must be 1..50" }
if ($Seconds -lt 20 -or $Seconds -gt 600) { Fail "-Seconds must be 20..600" }
if ($loaderList.Count -eq 0 -or @($loaderList | Where-Object { $_ -notin "sspc", "shadps4" }).Count) {
    Fail "-Loaders must list sspc and/or shadps4"
}
if ($Press -match '\s') { Fail "-Press must not contain spaces" }
if ($work -match '\s') { Fail "the profile path must not contain spaces: $work" }
if (-not (Test-Path $saves)) { Fail "no saves to copy: $saves" }
$gameExe = if ($Exe) { $Exe } else { "$Root\build\bin\SecondSonPC.exe" }
if (-not $Exe -and -not (Test-Path $gameExe)) { Fail "not built: $gameExe" }
if (-not $Exe) {
    # The stand-in of the tests needs neither check.
    if (Get-Process -Name SecondSonPC -ErrorAction SilentlyContinue) {
        Fail "SecondSonPC.exe is running (a game being played?) - one game at a time"
    }
}
$os = Get-CimInstance Win32_OperatingSystem
$freeCommitGiB = [math]::Round($os.FreeVirtualMemory / 1MB, 1)   # KiB -> GiB
$limitGiB = [math]::Round($os.TotalVirtualMemorySize / 1MB, 1)
$plan = "{0} run(s) x {1} s, loaders {2}, presses '{3}', profile {4}, free commit {5} / {6} GiB (need {7})" -f `
    $Runs, $Seconds, ($loaderList -join '/'), $Press, $profileDir, $freeCommitGiB, $limitGiB, $MinFreeCommitGiB
"sspc002_runs: $plan"
if (-not $Exe -and $freeCommitGiB -lt $MinFreeCommitGiB) {
    Fail "free system commit $freeCommitGiB GiB < $MinFreeCommitGiB GiB: close other programs first"
}
if ($DryRun) {
    for ($i = 1; $i -le $Runs; ++$i) {
        $loader = $loaderList[($i - 1) % $loaderList.Count]
        "  run {0}: {1}_{2}_{0} --loader {2} --user-data {3}{4}" -f $i, $Prefix, $loader, $profileDir, $(if ($Press) { " --press $Press" } else { "" })
    }
    "sspc002_runs: dry run, nothing started"
    exit 0
}

# --- series ---------------------------------------------------------------------------------
$stamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
$snapshot = "$work\snapshot_$stamp"
New-Item -ItemType Directory -Force $snapshot | Out-Null
Copy-Item "$saves" "$snapshot\home" -Recurse
$hashesBefore = Get-SaveHashes
"snapshot of the saves: $snapshot ($($hashesBefore.Count) files)"

$caught = @()
for ($i = 1; $i -le $Runs; ++$i) {
    $loader = $loaderList[($i - 1) % $loaderList.Count]
    $tag = "{0}_{1}_{2}" -f $Prefix, $loader, $i
    if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force } # previous run's copy, no crash
    New-Item -ItemType Directory -Force $profileDir | Out-Null
    Copy-Item "$snapshot\home" "$profileDir\home" -Recurse

    $gameArgs = "--loader $loader --vram-log 5 --user-data $profileDir"
    if ($Press) { $gameArgs = "$gameArgs --press $Press" }
    if ($ExeArgsPrefix) { $gameArgs = "$ExeArgsPrefix $gameArgs" }
    $runParams = @{ Seconds = $Seconds; Tag = $tag; NoReport = $true; ExtraArgs = $gameArgs; LogsDir = $logs }
    if ($Exe) { $runParams.Exe = $Exe }
    $text = & "$PSScriptRoot\run_phase0.ps1" @runParams 2>&1 | Out-String

    $runJson = "$logs\runs\$tag\run.json"
    if (-not (Test-Path $runJson)) { "${tag}: run_phase0.ps1 wrote no run.json:"; $text; exit 1 }
    $run = Get-Content -Raw -Encoding UTF8 $runJson | ConvertFrom-Json
    $line = & python -I "$PSScriptRoot\analyze_run.py" "$logs\runs\$tag"
    Add-Content -Encoding UTF8 $summary $line
    $s = $line | ConvertFrom-Json
    $crashed = ($run.how -like "crashed*") -or ($null -ne $s.crash)
    # alive = frames were still being presented at the end (closed-by-test with alive False: a hang).
    "{0}: {1}, exit {2}, {3} s, frames {4}, alive {5}, crash in logs: {6}, first-chance faults {7}{8}" -f $tag,
        $run.how, $run.exit_hex, $run.lifetime_s, $s.frames, $s.alive_at_end,
        $(if ($s.crash) { $s.crash } else { "none" }), $s.first_chance_faults,
        $(if ($s.minidump) { ", minidump $($s.minidump)" } else { "" })
    if ($crashed) {
        $kept = "$work\crash_$tag"
        Move-Item $profileDir $kept
        $caught += $tag
        "  CRASH: evidence in $logs\runs\$tag; profile kept: $kept"
        if (-not $KeepGoing) { break }
    }
}
if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }

$hashesAfter = Get-SaveHashes
$changed = @(@($hashesBefore.Keys) + @($hashesAfter.Keys) | Sort-Object -Unique |
    Where-Object { $hashesBefore[$_] -ne $hashesAfter[$_] })
"real saves unchanged: $(if ($changed.Count) { "NO - $($changed -join ', ')" } else { 'yes' })"
if ($caught.Count) {
    "sspc002_runs: crash caught in $($caught -join ', ')"
    exit 2
}
"sspc002_runs: no crash in this series"
exit 0
