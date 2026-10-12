# SecondSonPC P4.12: the stall investigation's runs - configurations S0..S4 x scenes, each run through p46_runs.ps1 (the
# P4.6..P4.11 route: save -> title -> Continue, camera turns at 56..60 s, WM_CLOSE at 90 s), the stall trace's files kept.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p412_runs.ps1 -Tag <name>
#       [-Configs S0,S1,S4] [-Scenes user1022,beach,early] [-Order forward|abba] [-Trace off|light|deep]
#       [-DeepWindow auto|<a>:<b>] [-Extra "A=b;C=d"] [-Exe <path>]
# Configurations (docs/PHASE4_P412_FULL_STALL_ANALYSIS.md):
#   S0 single thread        SSPC_P410_PARALLEL_CP=0
#   S1 parallel             SSPC_P410_PARALLEL_CP=1
#   S2 parallel + B         ... + SSPC_P49_BLOCK_UPLOAD=1
#   S3 parallel + async     ... + SSPC_P411_ASYNC_ACQUIRE=1
#   S4 all                  ... + both
# -Order abba: the configurations forward, then backward (each one twice, pairs balanced against drift).
# -Trace: SSPC_P412_STALL_TRACE (off: not set); deep's window by scene unless -DeepWindow: the village (user1022) 30:42,
# the beach and the harbour (early) 70:82 - inside the A/B windows (25..50 and 62..88 s).
# Out: logs\p412\<Tag>\<config>_<scene>_<n>\ (p46_runs' files + p412_light.json, p412_frames.csv, p412_trace.bin) and
# logs\p412\<Tag>\summary.txt (one line per run).
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Configs = "S0,S1,S4",
    [string]$Scenes = "user1022,beach,early",
    [ValidateSet("forward", "abba")][string]$Order = "forward",
    [ValidateSet("off", "light", "deep")][string]$Trace = "light",
    [string]$DeepWindow = "auto",
    [string]$Extra = "",
    [string]$Exe = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$out = "$root\logs\p412\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$summary = "$out\summary.txt"
$envOf = @{
    S0 = "SSPC_P410_PARALLEL_CP=0"
    S1 = "SSPC_P410_PARALLEL_CP=1"
    S2 = "SSPC_P410_PARALLEL_CP=1;SSPC_P49_BLOCK_UPLOAD=1"
    S3 = "SSPC_P410_PARALLEL_CP=1;SSPC_P411_ASYNC_ACQUIRE=1"
    S4 = "SSPC_P410_PARALLEL_CP=1;SSPC_P49_BLOCK_UPLOAD=1;SSPC_P411_ASYNC_ACQUIRE=1"
}
$windowOf = @{ user1022 = @(25, 50); beach = @(62, 88); early = @(62, 88) }
$deepOf = @{ user1022 = "30:42"; beach = "70:82"; early = "70:82" }
$cfgList = @($Configs -split ',' | Where-Object { $_ })
if ($Order -eq "abba") {
    $rev = @($cfgList)
    [array]::Reverse($rev)
    $cfgList = $cfgList + $rev
}
"p412 series '$Tag': configs $($cfgList -join ' '), scenes $Scenes, trace $Trace, extra '$Extra', exe '$Exe' ($(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))" |
    Out-File $summary -Encoding utf8 -Append
$count = @{}
foreach ($scene in ($Scenes -split ',' | Where-Object { $_ })) {
    foreach ($cfg in $cfgList) {
        if (-not $envOf.ContainsKey($cfg)) { throw "unknown configuration '$cfg'" }
        if (Get-Process -Name SecondSonPC -ErrorAction SilentlyContinue) {
            "p412: SecondSonPC.exe is running (the owner's game?) - the series stops" | Tee-Object -FilePath $summary -Append
            exit 2
        }
        $key = "${cfg}_$scene"
        $count[$key] = 1 + [int]$count[$key]
        $name = "${key}_$($count[$key])"
        $env = $envOf[$cfg]
        if ($Trace -eq "light") { $env += ";SSPC_P412_STALL_TRACE=light" }
        elseif ($Trace -eq "deep") {
            $w = if ($DeepWindow -eq "auto") { $deepOf[$scene] } else { $DeepWindow }
            $env += ";SSPC_P412_STALL_TRACE=deep,window=$w"
        } else { $env += ";SSPC_P412_STALL_TRACE=" }
        if ($Extra) { $env += ";$Extra" }
        foreach ($f in "p412_light.json", "p412_frames.csv", "p412_trace.bin") {
            Remove-Item "$root\logs\$f" -ErrorAction SilentlyContinue
        }
        $start = Get-Date -Format 'HH:mm:ss'
        $args46 = @("-ExecutionPolicy", "Bypass", "-File", "$root\tools\tests\p46_runs.ps1", "-Tag", $name, "-Scenes", $scene,
                    "-Repeats", "1", "-OutRoot", $out, "-Env", $env)
        if ($Exe) { $args46 += @("-Exe", $Exe) }
        $ErrorActionPreference = "Continue"
        & powershell @args46 2>&1 | Out-Null
        $ErrorActionPreference = "Stop"
        $dir = "$out\$name\${scene}_1"
        foreach ($f in "p412_light.json", "p412_frames.csv", "p412_trace.bin") {
            if (Test-Path "$root\logs\$f") { Move-Item "$root\logs\$f" $dir -Force }
        }
        $w = $windowOf[$scene]
        $line = "{0} {1,-18} env '{2}'" -f $start, $name, $env
        if (Test-Path "$dir\gpu_frames.csv") {
            $ErrorActionPreference = "Continue"
            py -I "$root\tools\frame_stats.py" "$dir\gpu_frames.csv" --from $w[0] --to $w[1] --json "$dir\window.json" 2>&1 | Out-Null
            $ErrorActionPreference = "Stop"
            if (Test-Path "$dir\window.json") {
                $s = Get-Content "$dir\window.json" -Raw | ConvertFrom-Json
                $line += " | fps {0:N2} low1 {1:N2} p95 {2:N1} p99 {3:N1} | cp {4:N2} gpu {5:N2} wait {6:N2} empty {7:N2} tail {8:N2} | draws {9:N0} disp {10:N0}" -f `
                    $s.avg_fps, $s.low1_fps, $s.ft_p95_ms, $s.ft_p99_ms, $s.mean_cp_ms, $s.mean_gpu_busy_ms,
                    $s.mean_par_wait_ms, $s.mean_empty_ms, $s.gpu_tail_ms, $s.mean_draws, $s.mean_dispatches
            }
        }
        $status = (Select-String -Path "$dir\gpu_native.txt" -Pattern "^STATUS" -ErrorAction SilentlyContinue | Select-Object -First 1).Line
        $errs = (Select-String -Path "$dir\SecondSonPC.log" -Pattern "\[Error\]" -ErrorAction SilentlyContinue | Measure-Object).Count
        $line += " | [Error] $errs | $status"
        $line | Out-File $summary -Append -Encoding utf8
        $line
    }
}
"summary: $summary"
