# Native Engine Reconstruction: one city run of a worktree's SecondSonPC.exe with the shared-machine rules
# (docs/NATIVE_ENGINE_RECONSTRUCTION.md §5.4). Waits while E:\research\sspc_bench.lock exists (an FPS series of another
# agent), takes E:\research\sspc_game.lock, runs tools\run_phase0.ps1 on a fresh copy of the stand's save, copies the
# run's logs out of the worktree and releases the lock.
#   powershell -File run_native_engine.ps1 -Tag <tag> [-Env "SSPC_NATIVE_ENGINE=verify;..."] [-Seconds 150]
#       [-Scene city|beach|harbor|village] [-LogsDir <dir>]
# R6: -Scene picks the save, the route and the length of the stand's scenes (city: the native-engine route, 150 s,
# window 70..148 s; beach / harbor (early) / village (user1022): tools\tests\p46_runs.ps1's saves and route, 90 s, window
# 62..88 s); -Save / -Press / -Seconds given explicitly override it. -LogsDir keeps run_phase0's logs per agent.
param(
    [Parameter(Mandatory)] [string]$Tag,
    [string]$Env = "",
    [int]$Seconds = 0,
    [string]$Worktree = "E:\SecondSonPC-native-recon",
    [string]$Out = "E:\research\native-recon\runs",
    [ValidateSet("city", "beach", "harbor", "village")] [string]$Scene = "city",
    [string]$Save = "",
    [string]$ShaderCache = "E:\SecondSonPC\cache\shaders",
    [string]$Press = "",
    [string]$Owner = "native-recon",
    [string]$LogsDir = "E:\research\native-recon\logs",
    [int]$QuietSeconds = 300
)
$ErrorActionPreference = "Stop"
$p46Route = "space@12,space@22,move:-3000:0@56:0.25,move:-3000:0@58:0.25,move:-3000:0@60:0.25"
$scenes = @{ # save, whether it is a "home" folder (p46's saves) or a whole user-data folder, route, seconds
    city    = @("E:\research\native-recon\saves\p0_orig", $false, "space@12,space@22,w@60:3,return@75", 150)
    beach   = @("E:\SecondSonPC\logs\performance\p43_ab\snapshot\home", $true, $p46Route, 90)
    harbor  = @("E:\SecondSonPC\userdata\savedata_backups\2026-10-10_03-44-58_before_manual_sspc", $true, $p46Route, 90)
    village = @("E:\SecondSonPC\userdata\savedata_backups\2026-10-10_12-47-35_before_manual_sspc", $true, $p46Route, 90)
}
$preset = $scenes[$Scene]
$saveIsHome = $preset[1] -and -not $Save
if (-not $Save) { $Save = $preset[0] }
if (-not $Press) { $Press = $preset[2] }
if ($Seconds -le 0) { $Seconds = $preset[3] }
$bench = "E:\research\sspc_bench.lock"
# a bench lock this agent took for its own FPS series (series.sh) does not hold its runs back
function OtherBench {
    (Test-Path $bench) -and -not ((Get-Content "$bench\owner.txt" -ErrorAction SilentlyContinue) -match "^$Owner ")
}
$lock = "E:\research\sspc_game.lock"
# a build of any worktree (E:\research\sspc_build.ps1) holds this one; a run never starts during it
$build = "E:\research\sspc_build.lock"
$exe = "$Worktree\build\bin\SecondSonPC.exe"
$run = Join-Path $Out $Tag
New-Item -ItemType Directory -Force $run | Out-Null

# the shared-machine rules: no FPS series running, the game lock taken atomically, no other game process
# An FPS series re-creates its bench lock for every run, so the gap between two of its runs is not "no series": the
# game lock is taken only after neither lock has existed for -QuietSeconds.
$quiet = $null
$staleWarned = $false
while ($true) {
    # a lock whose owner died (its shell killed: no finally) is never freed by itself: said once, removed only by hand
    if (-not $staleWarned -and (Test-Path $lock) -and -not (Get-Process SecondSonPC -ErrorAction SilentlyContinue) -and
        ((Get-Date) - (Get-Item $lock).CreationTime).TotalMinutes -gt 40) {
        Write-Output "STALE? $lock held $([int]((Get-Date) - (Get-Item $lock).CreationTime).TotalMinutes) min with no game: $(Get-Content "$lock\owner.txt" -ErrorAction SilentlyContinue)"
        $staleWarned = $true
    }
    if ((OtherBench) -or (Test-Path $lock) -or (Test-Path $build) -or
        (Get-Process SecondSonPC -ErrorAction SilentlyContinue)) {
        $quiet = $null; Start-Sleep -Seconds 30; continue
    }
    if ($null -eq $quiet) { $quiet = Get-Date }
    if (((Get-Date) - $quiet).TotalSeconds -lt $QuietSeconds) { Start-Sleep -Seconds 30; continue }
    try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null } catch { Start-Sleep -Seconds 120; continue }
    if (OtherBench) { Remove-Item $lock -Recurse -Force; continue } # lost the race to a series
    if (Get-Process SecondSonPC -ErrorAction SilentlyContinue) { Remove-Item $lock -Recurse -Force; Start-Sleep 120; continue }
    break
}
"$Owner $(Get-Date -Format 'HH:mm:ss') $Tag" | Set-Content -Encoding ASCII "$lock\owner.txt"
try {
    $saveCopy = Join-Path $run "save"
    if (Test-Path $saveCopy) { Remove-Item $saveCopy -Recurse -Force }
    if ($saveIsHome) {
        New-Item -ItemType Directory -Force $saveCopy | Out-Null
        Copy-Item $Save "$saveCopy\home" -Recurse
    } else {
        Copy-Item $Save $saveCopy -Recurse
    }
    "scene $Scene, save $Save, $Seconds s, press $Press" | Set-Content -Encoding UTF8 "$run\scene.txt"
    # the stand's preset (docs/CITY_PERFORMANCE.md) + this run's switches
    $vars = [ordered]@{
        SSPC_CP_MODE = "native"; SSPC_REA_MERGE_RUNS = "1"; SSPC_CP_ROUND_NODRAIN = "1"; SSPC_P411_ASYNC_ACQUIRE = "1"
        SSPC_ASYNC_PIPELINES = "1"; SSPC_CITY_PERFORMANCE = "1"
    }
    foreach ($kv in $Env.Split(';', [StringSplitOptions]::RemoveEmptyEntries)) {
        $k, $v = $kv.Split('=', 2); $vars[$k.Trim()] = $v
    }
    foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
    $vars | ConvertTo-Json | Set-Content -Encoding UTF8 "$run\env.json"
    Remove-Item "$Worktree\logs\gpu_frames.csv", "$Worktree\logs\SecondSonPC.log" -ErrorAction SilentlyContinue
    Remove-Item "$Worktree\logs\native_engine" -Recurse -ErrorAction SilentlyContinue
    $extra = "--loader sspc --user-data $saveCopy --shader-cache $ShaderCache --press $Press"
    & "$Worktree\tools\run_phase0.ps1" -Seconds $Seconds -Tag $Tag -NoReport -ExtraArgs $extra -Exe $exe `
        -LogsDir $LogsDir | Tee-Object "$run\console.txt"
    foreach ($f in "SecondSonPC.log", "gpu_frames.csv", "gpu_native.txt", "bridge_shadps4.log") {
        if (Test-Path "$Worktree\logs\$f") { Copy-Item "$Worktree\logs\$f" $run }
    }
    if (Test-Path "$Worktree\logs\native_engine") { Copy-Item "$Worktree\logs\native_engine" $run -Recurse }
    $rj = "$LogsDir\runs\$Tag\run.json"
    if (Test-Path $rj) { Copy-Item $rj $run }
    if (Test-Path "$LogsDir\runs\$Tag\screen.png") { Copy-Item "$LogsDir\runs\$Tag\screen.png" $run }
} finally {
    foreach ($k in @("SSPC_NATIVE_ENGINE", "SSPC_NATIVE_ENGINE_FUNCS", "SSPC_NATIVE_ENGINE_SAMPLE",
                     "SSPC_NATIVE_ENGINE_MEASURE", "SSPC_NATIVE_ENGINE_RECOMP")) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    Remove-Item $lock -Recurse -Force -ErrorAction SilentlyContinue
}
"done -> $run"
