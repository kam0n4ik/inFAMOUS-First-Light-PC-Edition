# P0 FPS investigation: the same short driven run in several native/bridge modes, measured alike.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\perf_runs.ps1 [-Modes A,B,C,D] [-Seconds 100]
#       [-Press "space@12,space@22,shot@60,shot@95"] [-ExtraArgs "..."] [-Session name] [-DryRun]
# Modes (sema / event flags):  A native/native   B native/bridge (--bridge kernel-eventflag)
#                              C bridge/native (--bridge kernel-sema)   D bridge/bridge (both)
# Phase 2.2.3 groups:          N0 none of them (as before the phase)  N1 + Tier 1 (kernel-util,
#                              net-util, libc-mem)  N2 + kernel-mutex (mutexes, condvars)  N3 + kernel-rwlock
# Phase 2.3.1 memory:          M0 default (bridge direct memory)  M1 --native kernel-dmem (experimental)
# Phase 2.3.4 memory manager:  M2 --native memory (SecondSonPC's manager, commit upfront as the bridge)
#                              M3 --native memory --memory-commit on-demand
# A mode may be listed more than once (M0,M1,M1,M0): its runs go to <mode>_1, <mode>_2, ...
# Two builds in one session (P2.3.2: before/after a refactoring): -Exes "before=E:\...\SecondSonPC.exe"
# names extra builds; a mode written <name>:<mode> (before:M0) runs that build, a plain mode runs
# build\bin. Its runs go to <name>-<mode>[_N]. Alternate them: -Modes "before:M0,M0,M0,before:M0".
# Every run: same exe, same SecondSonPC.toml, a fresh copy of the same save snapshot (--user-data),
# the same key presses ("Continue" twice -> forest gameplay from ~30 s). tools\perf_sampler.ps1
# samples the process once a second while it runs.
# Safety: the real saves (userdata\home) are only read (copied once; SHA-256 compared before/after);
# refuses to start while SecondSonPC.exe runs or when free system commit < -MinFreeCommitGiB;
# every run is ended by tools\run_phase0.ps1 (WM_CLOSE to the game window of its own pid).
# Results: logs\performance\<session>\<mode>\ (logs of the run, samples.csv, frametimes.csv from
# --frame-log, memory_map.txt from --memory-map N, summary.json) and tools\perf_report.py's
# report.md/.json/.csv in <session>\.
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
    [string]$Exes = "",                           # extra builds: "name=path[,name=path]" (see above)
    [string]$Saves = "",                          # the saves to snapshot (a "home" folder); default the
                                                  # real ones, userdata\home. P2.5 scenes: a frozen copy
                                                  # (logs\p25\scene_*\home), so play in between does not
                                                  # move the scene
    [switch]$Soak,                                # P3.2: one long run allowed (-Seconds up to 2400)
    [switch]$DryRun
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$logs = "$root\logs"
$saves = if ($Saves) { $Saves.TrimEnd('\') } else { "$root\userdata\home" }
if (-not (Test-Path "$saves\1000")) { "perf_runs: -Saves $saves is not a saves 'home' folder (no 1000\)"; exit 1 }
$modeArgs = @{
    A = ""; B = "--bridge kernel-eventflag"; C = "--bridge kernel-sema"
    D = "--bridge kernel-sema --bridge kernel-eventflag"
    # Phase 2.2.3 groups, added one at a time: N0 = state before the phase (NATIVE 79) ... N3 = all.
    N0 = "--bridge kernel-util --bridge net-util --bridge libc-mem --bridge kernel-mutex --bridge kernel-rwlock"
    N1 = "--bridge kernel-mutex --bridge kernel-rwlock"
    N2 = "--bridge kernel-rwlock"
    N3 = ""
    # Phase 2.3.1: M0 = default (direct memory accounted by the bridge), M1 = experimental native
    # direct memory. Alternate them (-Modes M0,M1,M1,M0): a mode listed twice gets M0_1, M0_2, ...
    M0 = ""; M1 = "--native kernel-dmem"
    # Phase 2.3.4: the guest memory on SecondSonPC's manager (M2), with commit on demand (M3).
    M2 = "--native memory"; M3 = "--native memory --memory-commit on-demand"
    # P2.4 shader cache (patch 0013): C0 no persistent cache (every run compiles everything), C1 the
    # test cache logs\p24\sc_test (cold on its first run, warm after it). The user's cache is not used.
    C0 = "--no-shader-cache"; C1 = "--shader-cache E:\SecondSonPC\logs\p24\sc_test"
    # P2.4 threads: T = SecondSonPC's thread manager (bridge memory), TM = with SecondSonPC's memory
    # manager too (commit on demand). With M0 (bridge/bridge) and M3 (native memory, bridge threads)
    # these are the four combinations A-D of docs/PHASE2_P24_NATIVE_THREADS.md.
    T = "--native threads"; TM = "--native threads --native memory --memory-commit on-demand"
    # P2.5 bridge performance changes, one build: PO0 every change off, PO1 the defaults (all on that
    # passed their A/B: barrier-memo, image-memo, stream-host, stream-reads). PO0 = PO1 of the build
    # before P2.5.
    PO0 = "--no-perf-opt barrier-memo --no-perf-opt image-memo --no-perf-opt stream-host --no-perf-opt stream-reads"
    PO1 = ""
    # One change under test against the defaults. PI = + image-memo (its A/B, p25_imemo_ab; now a
    # default, so PI = PO1).
    PI = "--perf-opt image-memo"
    # PS = + stream-host (the stream buffer in system memory, patch 0019; its A/B p25_shost_ab, now a
    # default, so PS = PO1).
    PS = "--perf-opt stream-host"
    # PR = + stream-reads (no read ranges for the stream buffer, patch 0020; its A/B p25_stream2_ab, now
    # a default, so PR = PO1). That session's PC (stream buffer in cached system memory) was rejected and
    # removed.
    PR = "--perf-opt stream-reads"
    # P3.3: F = SecondSonPC's FS patcher (load-time patches, TCB slot of its own; docs\PHASE3_INDEPENDENT_RUNTIME.md
    # §5), TMF = with SecondSonPC's threads and memory too (the most native runtime so far). PO1 = the bridge's
    # patcher (the TCB slot is SecondSonPC's in every mode).
    F = "--native fs-patch"; TMF = "--native fs-patch --native threads --native memory --memory-commit on-demand"
    # P3.2 matrix (docs\PHASE3_INDEPENDENT_RUNTIME.md §10): A = PO1 (bridge memory, threads, FS patcher),
    # B = MF (SecondSonPC's memory + FS patcher, the bridge's threads), C = TF (SecondSonPC's threads + FS
    # patcher, the bridge's memory), D = TMF; TM = SecondSonPC's memory + threads with the bridge's FS patcher.
    MF = "--native fs-patch --native memory --memory-commit on-demand"; TF = "--native fs-patch --native threads"
    # P3.4 (docs\PHASE3_P34_EXCEPTIONS_CPU.md): X = SecondSonPC's exception dispatcher (the bridge's CPU patcher
    # behind it), FX = dispatcher + SecondSonPC's CPU patcher, FXE = FX with SSE4a emulated on this AMD CPU (the
    # Intel path: 23 trampolines, 73 sites through the illegal-instruction exception), TMFX = everything native.
    X = "--native exceptions"; FX = "--native cpu --native exceptions"
    FXE = "--native cpu --native exceptions --cpu-emulate sse4a"
    TMFX = "--native cpu --native exceptions --native threads --native memory --memory-commit on-demand"
    # P3.5 (docs\PHASE3_P35_KERNEL_HLE.md): K = SecondSonPC's guest errno and process clock (the bridge's memory and
    # threads), KM = + SecondSonPC's memory manager with libkernel's memory functions on it, KTM = KM + SecondSonPC's
    # threads, TMFXK = everything native (P3.2-P3.5): TMFX + errno, clock and the memory functions.
    K = "--native errno --native clock"
    KM = "--native errno --native clock --native memory --memory-commit on-demand --native kernel-memory"
    KTM = "--native errno --native clock --native memory --memory-commit on-demand --native kernel-memory --native threads"
    TMFXK = "--native cpu --native exceptions --native threads --native memory --memory-commit on-demand --native errno --native clock --native kernel-memory"
    # P3.6: TMFXKE = TMFXK + the guest's _exit through SecondSonPC's orderly exit (everything experimental).
    TMFXKE = "--native cpu --native exceptions --native threads --native memory --memory-commit on-demand --native errno --native clock --native kernel-memory --native exit"
    # P4.2 (docs\PHASE4_P42_COMMAND_PROCESSOR.md): SH = SecondSonPC's command processor in the bridge's shadow
    # (a diagnostic mode: not for FPS comparisons with plain runs); ST/ST2 = the shadow recording 3 frames from 40 s
    # (the beach) / 75 s (the foliage) for --gpu-replay; FP = the flip timeline (SSPC-015).
    SH = "--gpu-shadow"; ST = "--gpu-trace 40:3"; ST2 = "--gpu-trace 75:3"; FP = "--frame-pacing"
    # P4.3 (docs\PHASE4_P43_SHADER_TRANSLATION.md): SS = SecondSonPC's shader frontend in the shadow, compared with the
    # bridge's pipelines (diagnostic); SC = the bridge's translations captured, cold (no persistent shader cache).
    SS = "--gpu-shaders"; SC = "--no-shader-cache --gpu-shader-capture"
}
$defaultExe = "$root\build\bin\SecondSonPC.exe"
$exeMap = @{}
foreach ($pair in @($Exes.Split(',', [StringSplitOptions]::RemoveEmptyEntries))) {
    $name, $path = $pair.Split('=', 2)
    $exeMap[$name.Trim()] = "$path".Trim()
}
$entries = @($Modes.Split(',', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object {
    $mode = $_.Trim(); $build = ""
    if ($mode.Contains(':')) { $build, $mode = $mode.Split(':', 2); $build = $build.Trim() }
    $mode = $mode.Trim().ToUpper()
    [pscustomobject]@{ Build = $build; Mode = $mode; Key = $(if ($build) { "$build-$mode" } else { $mode }) }
})
$modeList = @($entries | ForEach-Object { $_.Mode })
$repeated = @($entries | Group-Object Key | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
$seen = @{}
$runList = foreach ($e in $entries) {
    $seen[$e.Key] = 1 + [int]$seen[$e.Key]
    [pscustomobject]@{
        Mode = $e.Mode; Build = $e.Build; Exe = $(if ($e.Build) { $exeMap[$e.Build] } else { $defaultExe })
        Label = $(if ($repeated -contains $e.Key) { "$($e.Key)_$($seen[$e.Key])" } else { $e.Key })
    }
}

function Fail([string]$Message) { "perf_runs: $Message"; exit 1 }
function Get-SaveHashes {
    $map = [ordered]@{}
    Get-ChildItem $saves -Recurse -File | Sort-Object FullName | ForEach-Object {
        $map[$_.FullName.Substring($saves.Length + 1)] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
    }
    $map
}

if (-not $modeList.Count -or @($modeList | Where-Object { -not $modeArgs.ContainsKey($_) }).Count) {
    Fail "-Modes must list A, B, C, D (sema/event flags), N0..N3 (Phase 2.2.3 groups) and/or M0..M3 (P2.3 memory)"
}
$maxSeconds = if ($Soak) { 2400 } else { 300 }
if ($Seconds -lt 30 -or $Seconds -gt $maxSeconds) { Fail "-Seconds must be 30..$maxSeconds (-Soak for one long run)" }
if ($Soak -and @($runList).Count -ne 1) { Fail "-Soak takes one run" }
if ($Press -match '\s') { Fail "-Press must not contain spaces" }
if (-not (Test-Path $defaultExe)) { Fail "not built" }
foreach ($r in $runList) {
    if (-not $r.Exe -or -not (Test-Path $r.Exe)) { Fail "run $($r.Label): build '$($r.Build)' is not in -Exes or does not exist" }
}
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
    foreach ($r in $runList) { $m = $r.Mode; "  run $($r.Label) ($($r.Exe)): --loader sspc --user-data $out\profile --vram-log 5$(if (-not $NoFrameLog) { ' --frame-log' }) --press $Press $($modeArgs[$m]) $ExtraArgs" }
    exit 0
}

New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item $saves "$out\snapshot\home" -Recurse
$hashesBefore = Get-SaveHashes
$meta = [ordered]@{
    session = $Session; started = (Get-Date).ToString("s"); seconds = $Seconds; press = $Press; extra_args = $ExtraArgs
    exe = $exe.FullName; exe_time = $exe.LastWriteTime.ToString("s"); exe_size = $exe.Length
    git_head = (& git -C $root rev-parse --short HEAD); git_dirty = [bool](& git -C $root status --porcelain -- src third_party patches CMakeLists.txt)
    free_commit_gib_at_start = $freeCommitGiB; commit_limit_gib = $limitGiB; modes = @($runList | ForEach-Object { $_.Label })
    saves = $saves
    exes = [ordered]@{}
}
foreach ($name in @("") + @($exeMap.Keys | Sort-Object)) {
    $file = Get-Item $(if ($name) { $exeMap[$name] } else { $defaultExe })
    $meta.exes[$(if ($name) { $name } else { "default" })] = [ordered]@{
        path = $file.FullName; time = $file.LastWriteTime.ToString("s"); size = $file.Length
        sha256 = (Get-FileHash $file.FullName -Algorithm SHA256).Hash
    }
}
$meta | ConvertTo-Json | Set-Content -Encoding UTF8 "$out\session.json"

foreach ($r in $runList) {
    $m = $r.Mode
    $tag = "perf_${Session}_$($r.Label)"
    $profileDir = "$out\profile"
    if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
    Copy-Item "$out\snapshot" $profileDir -Recurse
    $modeDir = "$out\$($r.Label)"
    New-Item -ItemType Directory -Force $modeDir | Out-Null

    $sampler = Start-Process powershell -WindowStyle Hidden -PassThru -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "$PSScriptRoot\perf_sampler.ps1", "-Out", "$modeDir\samples.csv",
        "-MaxSeconds", "$([math]::Max(900, $Seconds + 120))")
    Remove-Item "$logs\frametimes.csv", "$logs\memory_map.txt", "$logs\gpu_shadow.txt", "$logs\frame_pacing.csv",
        "$logs\frame_pacing.txt" -ErrorAction SilentlyContinue
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
    $text = & "$PSScriptRoot\run_phase0.ps1" -Seconds $Seconds -Tag $tag -NoReport -ExtraArgs $gameArgs -Exe $r.Exe 2>&1 | Out-String
    [void]$sampler.WaitForExit(30000)
    if ($snap) { [void]$snap.WaitForExit(30000) }
    Set-Content -Encoding UTF8 "$modeDir\run_phase0.txt" $text
    if (Test-Path "$logs\frametimes.csv") { Move-Item "$logs\frametimes.csv" "$modeDir\frametimes.csv" -Force }
    if (Test-Path "$logs\memory_map.txt") { Move-Item "$logs\memory_map.txt" "$modeDir\memory_map.txt" -Force } # --memory-map N
    # P4.2: the shadow's report and traces, the flip timeline
    foreach ($f in "gpu_shadow.txt", "frame_pacing.csv", "frame_pacing.txt") {
        if (Test-Path "$logs\$f") { Move-Item "$logs\$f" "$modeDir\$f" -Force }
    }
    if (Test-Path "$logs\gpu_trace") {
        Get-ChildItem "$logs\gpu_trace" -Filter *.sspctrace | Move-Item -Destination $modeDir -Force
    }

    $runDir = "$logs\runs\$tag"
    if (-not (Test-Path "$runDir\run.json")) { "mode ${m}: run_phase0.ps1 wrote no run.json"; $text; exit 1 }
    Get-ChildItem $runDir -File | Copy-Item -Destination $modeDir -Force
    if (Test-Path "$runDir\shots") { Copy-Item "$runDir\shots" "$modeDir\shots" -Recurse -Force }
    $line = & python -I "$PSScriptRoot\analyze_run.py" $runDir
    Set-Content -Encoding UTF8 "$modeDir\summary.json" $line
    $s = $line | ConvertFrom-Json
    $run = Get-Content -Raw -Encoding UTF8 "$runDir\run.json" | ConvertFrom-Json
    "run $($r.Label) ($($modeArgs[$m]); $(if ($r.Build) { $r.Build } else { 'build\bin' })): $($run.how) exit $($run.exit_hex), frames $($s.frames), series $(($s.frames_series | ForEach-Object { $_[1] }) -join ' '), first-chance $($s.first_chance_faults), crash: $(if ($s.crash) { $s.crash } else { 'none' })"
}
if (Test-Path "$out\profile") { Remove-Item "$out\profile" -Recurse -Force }

$hashesAfter = Get-SaveHashes
$changed = @(@($hashesBefore.Keys) + @($hashesAfter.Keys) | Sort-Object -Unique | Where-Object { $hashesBefore[$_] -ne $hashesAfter[$_] })
"saves $saves unchanged: $(if ($changed.Count) { "NO - $($changed -join ', ')" } else { 'yes' })"
if (Test-Path "$PSScriptRoot\perf_report.py") { & python -I "$PSScriptRoot\perf_report.py" $out }
