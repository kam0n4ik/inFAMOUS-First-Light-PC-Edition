# SecondSonPC Phase 4 smoke research: the Smoke Benchmark (docs\PHASE4_SMOKE_PIPELINE.md §A).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\smoke_bench.ps1 -Tag <name>
#       [-Mode perf|flicker] [-Exe <SecondSonPC.exe>] [-Scene beach] [-Repeats 1] [-Env "A=b;C=d"]
#       [-CaptureFrom 40] [-Count 300] [-Down 2] [-Ref <tag of a flicker run to compare with>]
# Scene and camera: tools\tests\p46_runs.ps1 (a fresh copy of the save; title -> Continue at 12 s / 22 s; three camera
# turns at 56..60 s; WM_CLOSE at the end). Resolution: the game's 1920x1080 (VideoOut buffers), native backend.
#   perf     90 s; FPS / frame time / 1% low / CP / GPU over 62..88 s (stats.json) - the smoke column is in view on the
#            beach before the camera turns and the village's fires after them.
#   flicker  110 s; frames from second -CaptureFrom (`t` start of SSPC_DIAG_CAPTURE_FRAMES: the first flip at or after
#            it), -Count consecutive flips, downsampled by -Down; tools\imgcmp\smoke_stats.py over the smoke column
#            (static camera only), counts per 100 frames and per second of game time (gpu_frames.csv flip times).
# Results: logs\smoke_research\<Tag>\ - manifest.json (exe, its SHA-256 and time, git HEAD and the tree's changed
# source files, scene, route, windows, env), the run directories of p46_runs.ps1, frames\, smoke.json, smoke_series.csv.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [ValidateSet("perf", "flicker")][string]$Mode = "flicker",
    [string]$Exe = "E:\SecondSonPC\build\bin\SecondSonPC.exe",
    [string]$Scene = "beach",
    [int]$Repeats = 1,
    [string]$Env = "",
    [int]$CaptureFrom = 40,
    [int]$Count = 300,
    [int]$Down = 2,
    [string]$Ref = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$outRoot = "$root\logs\smoke_research"
$out = "$outRoot\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$seconds = if ($Mode -eq "perf") { 90 } else { 110 }
$envs = $Env
if ($Mode -eq "flicker") {
    $cap = "SSPC_DIAG_CAPTURE_FRAMES=t${CaptureFrom}:${Count}:1:${Down}"
    $envs = if ($envs) { "$cap;$envs" } else { $cap }
}
$head = (git -C $root rev-parse --short HEAD)
$dirty = @(git -C $root status --porcelain -- src tools/tests tools/imgcmp | ForEach-Object { $_.Trim() })
$exeItem = Get-Item $Exe
$manifest = [ordered]@{
    tag          = $Tag
    mode         = $Mode
    started      = (Get-Date).ToString("s")
    exe          = $exeItem.FullName
    exe_sha256   = (Get-FileHash $Exe -Algorithm SHA256).Hash
    exe_time     = $exeItem.LastWriteTime.ToString("s")
    git_head     = $head
    git_changed  = $dirty
    scene        = $Scene
    route        = "p46_runs.ps1: space@12, space@22, camera turns move:-3000:0 at 56/58/60 s, $seconds s"
    resolution   = "1920x1080 (game), native backend --gpu-backend sspc"
    perf_window  = "62..88 s (stats.json), warm-up 30..55 s (warmup.json)"
    capture      = if ($Mode -eq "flicker") { "from ${CaptureFrom} s, $Count flips, 1/$Down" } else { $null }
    env          = $envs
    repeats      = $Repeats
}
$manifest | ConvertTo-Json -Depth 4 | Out-File "$out\manifest.json" -Encoding utf8
$frames = "$root\logs\p45\frames"
if ($Mode -eq "flicker" -and (Test-Path $frames)) { Remove-Item "$frames\flip_*.png" -ErrorAction SilentlyContinue }
powershell -ExecutionPolicy Bypass -File "$root\tools\tests\p46_runs.ps1" -Tag "runs" -OutRoot $out -Scenes $Scene `
    -Repeats $Repeats -Seconds $seconds -Env $envs -Exe $Exe | Select-Object -First $Repeats
if ($Mode -eq "flicker") {
    $dst = "$out\frames"
    New-Item -ItemType Directory -Force $dst | Out-Null
    Move-Item "$frames\flip_*.png" $dst -ErrorAction SilentlyContinue
    "frames: " + (Get-ChildItem $dst -Filter flip_*.png | Measure-Object).Count
    $csv = "$out\runs\${Scene}_1\gpu_frames.csv"
    $args2 = @("$root\tools\imgcmp\smoke_stats.py", $dst, "--json", "$out\smoke.json", "--series", "$out\smoke_series.csv")
    if (Test-Path $csv) { $args2 += @("--csv", $csv) }
    if ($Ref) { $args2 += @("--ref", "$outRoot\$Ref\frames") }
    $ErrorActionPreference = "Continue"
    py -I @args2 | Out-Null
    $ErrorActionPreference = "Stop"
    if (Test-Path "$out\smoke.json") {
        $s = Get-Content "$out\smoke.json" -Raw | ConvertFrom-Json
        "smoke: static {0} | mass {1:N4} cv {2:N3} | dips/100 {3:N2} blink {4:N3}% jumps/100 {5:N1} | dips/s {6} jumps/s {7} | ratio vs ref {8}" -f `
            $s.static_frames, $s.mass_mean, $s.mass_cv, $s.dips_per_100, $s.blink_mean_pct, $s.jumps_per_100,
            $s.dips_per_s, $s.jumps_per_s, $s.mass_ratio_vs_ref
    }
}
