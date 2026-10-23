# R9 stand: one run of a worktree's SecondSonPC.exe with a scripted gameplay route (docs/R9_GAMEPLAY_PERFORMANCE.md §3):
# a fresh copy of the scene's save, the stand preset + the launcher's switches (play_native_gb.cmd) + this run's, the
# route's input (--press, with mark:<phase> events for the gameplay profiler), tools\run_phase0.ps1, the run's logs
# copied out, the background CPU load of other processes and nvidia-smi sampled. Derived from
# E:\research\deep_perf\tools\run1.ps1 (the R6-R8 stand: same lock, same preset, same exit).
#   powershell -File tools\r9\r9run.ps1 -Tag <tag> -Route fight|traverse|calm|<press spec> [-Scene city|user]
#       [-Env "K=V;K=V"] [-NoLauncher] [-Seconds 150] [-Out E:\research\r9\runs\<series>] [-Worktree E:\SecondSonPC-r9]
#       [-Shots "40,50,..."|route] (window screenshots at those seconds - tools\r9\shots.ps1; "route": the route's own)
param(
    [Parameter(Mandatory)] [string]$Tag,
    [string]$Route = "calm",
    [ValidateSet("city", "user")] [string]$Scene = "city",
    [string]$Env = "",
    [switch]$NoLauncher,
    [int]$Seconds = 0,
    [string]$Out = "E:\research\r9\runs",
    [string]$Worktree = "E:\SecondSonPC-r9",
    [string]$ShaderCache = "E:\SecondSonPC\cache\shaders",
    [string]$MoreArgs = "",
    [string]$Shots = "",
    [switch]$KeepSave
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\routes.ps1"
$scenes = @{ # save (a "home" folder)
    city = "E:\SecondSonPC\logs\hl\p0\home"         # Seattle Park, early game (Smoke), the R6-R8 city stand's save
    user = "E:\research\r9\saves\user_1022\home"    # the owner's save of 2026-10-22 21:18 (frozen copy)
}
$r = Get-R9Route $Route
if ($Seconds -le 0) { $Seconds = $r.Seconds }
$exe = "$Worktree\build\bin\SecondSonPC.exe"
$run = Join-Path $Out $Tag
New-Item -ItemType Directory -Force $run | Out-Null
$lock = "E:\research\deep_perf\game.lock"
# (another run of a stand: its game, or its lock until it copied its logs - a lock older than 15 min is stale)
while ((Get-Process SecondSonPC -ErrorAction SilentlyContinue) -or ((Test-Path $lock) -and
       ((Get-Date) - (Get-Item $lock).LastWriteTime).TotalMinutes -lt 15)) { Start-Sleep -Seconds 10 }
New-Item -ItemType Directory -Force $lock | Out-Null
"$(Get-Date -Format 'HH:mm:ss') r9 $Tag" | Set-Content -Encoding ASCII "$lock\owner.txt"
$saved = [ordered]@{}
$bg = $null
$smi = $null
try {
    $saveCopy = Join-Path $run "save"
    if (Test-Path $saveCopy) { Remove-Item $saveCopy -Recurse -Force }
    New-Item -ItemType Directory -Force $saveCopy | Out-Null
    Copy-Item $scenes[$Scene] "$saveCopy\home" -Recurse
    "scene $Scene, save $($scenes[$Scene]), route $Route ($($r.Name)), $Seconds s, exe $exe ($((Get-Item $exe).LastWriteTime.ToString('s')))" |
        Set-Content -Encoding UTF8 "$run\scene.txt"
    "press $($r.Press)" | Add-Content -Encoding UTF8 "$run\scene.txt"
    $vars = [ordered]@{ SSPC_CP_MODE = "native"; SSPC_REA_MERGE_RUNS = "1"; SSPC_CP_ROUND_NODRAIN = "1"
        SSPC_P411_ASYNC_ACQUIRE = "1"; SSPC_ASYNC_PIPELINES = "1"; SSPC_CITY_PERFORMANCE = "1" }
    if (-not $NoLauncher) { # E:\SecondSonPC\play_native_gb.cmd (R8: what the owner plays)
        foreach ($kv in "SSPC_P53_FRONTEND_THREAD=1", "SSPC_CP_ASYNC_WRITES=1", "SSPC_CP_ASYNC_FLIP=1",
                        "SSPC_VK_EMIT_THREAD=1", "SSPC_VK_EMIT_BATCH=1", "SSPC_CP_RING=128", "SSPC_NATIVE_GBUFFER=native") {
            $k, $v = $kv.Split('=', 2); $vars[$k] = $v
        }
    }
    foreach ($kv in $Env.Split(';', [StringSplitOptions]::RemoveEmptyEntries)) {
        $k, $v = $kv.Split('=', 2); $vars[$k.Trim()] = $v
    }
    foreach ($k in $vars.Keys) {
        if (-not $saved.Contains($k)) { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
        Set-Item "env:$k" $vars[$k]
    }
    Get-ChildItem env: | Where-Object { $_.Name -like "SSPC_*" } | Sort-Object Name |
        ForEach-Object { "$($_.Name)=$($_.Value)" } | Set-Content -Encoding UTF8 "$run\env_effective.txt"
    $logs = "$Worktree\logs"
    New-Item -ItemType Directory -Force $logs | Out-Null
    $files = "gpu_frames.csv", "SecondSonPC.log", "gpu_native.txt", "r8_cp_profile.csv", "r8_cp_prep_samples.csv",
             "r9_gprof.bin", "cpu_samples_raw.txt", "gpu_profile_full.txt", "vk_validation.log", "gpu_device_lost.txt",
             "gpu_cpu_profile.txt", "gpu_cpu_profile_game.txt", "p412_light.json", "p412_frames.csv", "p412_trace.bin"
    foreach ($f in $files) { Remove-Item "$logs\$f" -ErrorAction SilentlyContinue }
    $bg = Start-Job -ArgumentList $Seconds -ScriptBlock {
        param($secs)
        $c = New-Object System.Diagnostics.PerformanceCounter("Processor", "% Processor Time", "_Total")
        [void]$c.NextValue(); $out = @()
        for ($i = 0; $i -lt $secs + 20; $i++) {
            Start-Sleep -Seconds 1
            $total = $c.NextValue()
            $g = Get-Process SecondSonPC -ErrorAction SilentlyContinue | Select-Object -First 1
            $os = Get-CimInstance Win32_OperatingSystem -Property FreePhysicalMemory, TotalVirtualMemorySize, FreeVirtualMemory
            $out += [pscustomobject]@{ t = $i; total = [math]::Round($total, 1)
                game_cpu_s = if ($g) { $g.CPU } else { $null }
                private_mb = if ($g) { [math]::Round($g.PrivateMemorySize64 / 1MB) } else { $null }
                ws_mb = if ($g) { [math]::Round($g.WorkingSet64 / 1MB) } else { $null }
                threads = if ($g) { $g.Threads.Count } else { $null }
                free_ram_mb = [math]::Round($os.FreePhysicalMemory / 1KB)
                commit_free_mb = [math]::Round($os.FreeVirtualMemory / 1KB) }
        }
        $out
    }
    $smi = Start-Process -FilePath nvidia-smi -ArgumentList "dmon -s pumct -d 1 -c $($Seconds + 15) -f `"$run\nvsmi_dmon.txt`"" `
        -WindowStyle Hidden -PassThru
    $shotJob = $null
    if ($Shots -eq "route") { $Shots = $r.Shots }
    if ($Shots) { $shotJob = Start-Job -FilePath "$PSScriptRoot\shots.ps1" -ArgumentList "$run\shots", $Shots, 180, "$PSScriptRoot\..\game_process.psm1" }
    $extra = "--loader sspc --user-data $saveCopy --shader-cache $ShaderCache --press $($r.Press) $MoreArgs".Trim()
    & "$Worktree\tools\run_phase0.ps1" -Seconds $Seconds -Tag $Tag -NoReport -ExtraArgs $extra -Exe $exe `
        -LogsDir "E:\research\r9\logs\p0" | Tee-Object "$run\console.txt"
    foreach ($f in $files) { if (Test-Path "$logs\$f") { Copy-Item "$logs\$f" $run } }
    if ($shotJob) { Receive-Job $shotJob -Wait -AutoRemoveJob | Add-Content -Encoding UTF8 "$run\console.txt" }
    $rj = "E:\research\r9\logs\p0\runs\$Tag\run.json"
    if (Test-Path $rj) { Copy-Item $rj $run }
    if (Test-Path "E:\research\r9\logs\p0\runs\$Tag\screen.png") { Copy-Item "E:\research\r9\logs\p0\runs\$Tag\screen.png" $run }
    if (Test-Path "$run\gpu_frames.csv") {
        $w = $r.Window.Split(':')
        & python -I "$Worktree\tools\frame_stats.py" "$run\gpu_frames.csv" --from $w[0] --to $w[1] --json "$run\stats.json" | Out-Null
    }
} finally {
    if ($bg) {
        $samples = Receive-Job $bg -Wait -AutoRemoveJob
        $samples | ConvertTo-Csv -NoTypeInformation | Set-Content -Encoding ASCII "$run\cpu_bg.csv"
    }
    if ($smi -and -not $smi.HasExited) { Stop-Process -Id $smi.Id -ErrorAction SilentlyContinue }
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    if (-not $KeepSave) { Remove-Item "$run\save" -Recurse -Force -ErrorAction SilentlyContinue }
    Remove-Item $lock -Recurse -Force -ErrorAction SilentlyContinue
}
"done -> $run"
