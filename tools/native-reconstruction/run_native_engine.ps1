# Native Engine Reconstruction: one city run of a worktree's SecondSonPC.exe with the shared-machine rules
# (docs/NATIVE_ENGINE_RECONSTRUCTION.md §5). Waits while E:\research\sspc_bench.lock exists (an FPS series of another
# agent), takes E:\research\sspc_game.lock, runs tools\run_phase0.ps1 on a fresh copy of the stand's save, copies the
# run's logs out of the worktree and releases the lock.
#   powershell -File run_native_engine.ps1 -Tag <tag> [-Env "SSPC_NATIVE_ENGINE=verify;..."] [-Seconds 150]
param(
    [Parameter(Mandatory)] [string]$Tag,
    [string]$Env = "",
    [int]$Seconds = 150,
    [string]$Worktree = "E:\SecondSonPC-native-recon",
    [string]$Out = "E:\research\native-recon\runs",
    [string]$Save = "E:\research\native-recon\saves\p0_orig",
    [string]$ShaderCache = "E:\SecondSonPC\cache\shaders",
    [string]$Press = "space@12,space@22,w@60:3,return@75",
    [string]$Owner = "native-recon"
)
$ErrorActionPreference = "Stop"
$bench = "E:\research\sspc_bench.lock"
$lock = "E:\research\sspc_game.lock"
$exe = "$Worktree\build\bin\SecondSonPC.exe"
$run = Join-Path $Out $Tag
New-Item -ItemType Directory -Force $run | Out-Null

# the shared-machine rules: no FPS series running, the game lock taken atomically, no other game process
while ($true) {
    if (Test-Path $bench) { Start-Sleep -Seconds 180; continue }
    try { New-Item -ItemType Directory $lock -ErrorAction Stop | Out-Null } catch { Start-Sleep -Seconds 120; continue }
    if (Test-Path $bench) { Remove-Item $lock -Recurse -Force; continue } # lost the race to a series
    if (Get-Process SecondSonPC -ErrorAction SilentlyContinue) { Remove-Item $lock -Recurse -Force; Start-Sleep 120; continue }
    break
}
"$Owner $(Get-Date -Format 'HH:mm:ss') $Tag" | Set-Content -Encoding ASCII "$lock\owner.txt"
try {
    $save = Join-Path $run "save"
    if (Test-Path $save) { Remove-Item $save -Recurse -Force }
    Copy-Item $Save $save -Recurse
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
    $extra = "--loader sspc --user-data $save --shader-cache $ShaderCache --press $Press"
    & "$Worktree\tools\run_phase0.ps1" -Seconds $Seconds -Tag $Tag -NoReport -ExtraArgs $extra -Exe $exe `
        -LogsDir "E:\research\native-recon\logs" | Tee-Object "$run\console.txt"
    foreach ($f in "SecondSonPC.log", "gpu_frames.csv", "gpu_native.txt", "bridge_shadps4.log") {
        if (Test-Path "$Worktree\logs\$f") { Copy-Item "$Worktree\logs\$f" $run }
    }
    if (Test-Path "$Worktree\logs\native_engine") { Copy-Item "$Worktree\logs\native_engine" $run -Recurse }
    $rj = "E:\research\native-recon\logs\runs\$Tag\run.json"
    if (Test-Path $rj) { Copy-Item $rj $run }
    if (Test-Path "E:\research\native-recon\logs\runs\$Tag\screen.png") { Copy-Item "E:\research\native-recon\logs\runs\$Tag\screen.png" $run }
} finally {
    foreach ($k in @("SSPC_NATIVE_ENGINE", "SSPC_NATIVE_ENGINE_FUNCS", "SSPC_NATIVE_ENGINE_SAMPLE",
                     "SSPC_NATIVE_ENGINE_MEASURE", "SSPC_NATIVE_ENGINE_RECOMP")) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    Remove-Item $lock -Recurse -Force -ErrorAction SilentlyContinue
}
"done -> $run"
