# SecondSonPC P5.3D.3: interleaved A/B performance series of the native render path.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\p53_perf_ab.ps1 -Tag <name>
#       [-Scenes beach,early] [-Repeats 3] [-Seconds 90] [-Configs base,apply9,apply19,apply19s,par_base,par_apply19s]
# Every repeat runs every scene with every configuration, in turn (base, then the native variants, then again), so
# slow drift of the machine falls on all of them alike. Measurement runs: no Vulkan validation, the native path
# without its checks (SSPC_P53_NATIVE_VERIFY=0), no periodic native counters (SSPC_P53_NATIVE_DRAW_STATS=0). The
# process's CPU time and working set are sampled every second (cpu_mem.csv next to the run). Each run's frame window
# (62..88 s) is in stats.json (tools\frame_stats.py); tools\tests\p53_perf_summary.py sums the series up.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Scenes = "beach,early",
    [int]$Repeats = 3,
    [int]$Seconds = 90,
    [string]$Configs = "base,apply9,apply19,apply19s,par_base,par_apply19s",
    [string]$Exe = "", # a copy of SecondSonPC.exe for the whole series (the build may change while it runs)
    [string]$ExeB = "", # a second exe: a configuration named <config>_B runs <config> with it (an A/B of two builds)
    [string]$Press = "" # FrameBreak: scripted input for p46_runs.ps1 (its default route if empty)
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$quiet = "SSPC_P53_NATIVE_VERIFY=0;SSPC_P53_NATIVE_DRAW_STATS=0"
$defs = @{
    # (the single-thread processor named explicitly: the parallel one is the default since P5.3D)
    base         = "SSPC_P410_PARALLEL_CP=0"
    apply9       = "SSPC_P410_PARALLEL_CP=0;SSPC_P53_NATIVE_DRAW=apply:9;$quiet"
    apply19      = "SSPC_P410_PARALLEL_CP=0;SSPC_P53_NATIVE_DRAW=apply:1,9;$quiet"
    apply19s     = "SSPC_P410_PARALLEL_CP=0;SSPC_P53_NATIVE_DRAW=apply:1,9;SSPC_P53_NATIVE_STATE=apply;$quiet"
    par_base     = "SSPC_P410_PARALLEL_CP=1"
    par_apply19s = "SSPC_P410_PARALLEL_CP=1;SSPC_P53_NATIVE_DRAW=apply:1,9;SSPC_P53_NATIVE_STATE=apply;$quiet"
    # P5.3D: the shader frontend on its own thread (parallel_cp.h, Options::frontend_thread)
    par_ft       = "SSPC_P410_PARALLEL_CP=1;SSPC_P53_FRONTEND_THREAD=1"
    # SecondSon Native CP, CP-0: the existing options of the parallel processor measured again
    par_acq      = "SSPC_P411_ASYNC_ACQUIRE=1"
    # CP-5 experiment: a queue's packets per round of the processor (4096 by default)
    round16k     = "SSPC_CP_ROUND_PACKETS=16384"
    round64k     = "SSPC_CP_ROUND_PACKETS=65536"
    round64k_acq = "SSPC_CP_ROUND_PACKETS=65536;SSPC_P411_ASYNC_ACQUIRE=1"
    # CP-2: shader code trusted by write tracking, read again once in 8 frames
    trust_s      = "SSPC_P52_CODE_TRUST=sampled"
    # Native CP modes (--cp-mode / SSPC_CP_MODE; runtime/gpu/cp_mode.h) and their candidate changes
    cp_legacy    = "SSPC_CP_MODE=legacy"
    cp_parallel  = "SSPC_CP_MODE=parallel"
    cp_native    = "SSPC_CP_MODE=native"
    native_j     = "SSPC_CP_MODE=native;SSPC_CP_JOURNAL_CHANGES=1"
    native_srt   = "SSPC_CP_MODE=native;SSPC_CP_SRT_LINES=1"
    native_flip  = "SSPC_CP_MODE=native;SSPC_CP_ASYNC_FLIP=1"
    # GPU optimization stage: P5.4 early frame submission on top of Native (frame_trace.cpp, SSPC_P54_EARLY_SUBMIT)
    native_early = "SSPC_CP_MODE=native;SSPC_P54_EARLY_SUBMIT=1"
    early_cfirst = "SSPC_CP_MODE=native;SSPC_P54_EARLY_SUBMIT=1;SSPC_DIAG_QUEUE_ORDER=compute-first"
    # FrameBreak FB-2 (docs/FRAMEBREAK.md): the present pacing - console (a flip at the 59.94 Hz vblank, as before),
    # free with a cap, unlimited
    fb_console   = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=console"
    fb_unl       = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=unlimited"
    fb_60        = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=60"
    fb_90        = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=90"
    fb_120       = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=120"
    fb_144       = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=144"
    fb_30        = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=30"
    # FB-6: CP-5 "both" measured again without the vblank (a round of 65536 packets, met memory waits without a drain)
    # FB-6: unchanged draw stages not copied into the records (parallel_cp.cpp ReuseView)
    fb_reuse     = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=unlimited;SSPC_FB_STAGE_REUSE=1"
    fb_reuse_v   = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=unlimited;SSPC_FB_STAGE_REUSE=verify"
    fb_unl_both  = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=unlimited;SSPC_CP_ROUND_PACKETS=65536;SSPC_P411_ASYNC_ACQUIRE=1"
    # FB-7 (village GPU): MEASUREMENT ONLY - the write-back shader's changed words not stored to the host (its PCIe
    # stores' share of the frame; the host's copy is wrong)
    fb7_nohost   = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=unlimited;SSPC_DIAG_MEASURE=wb_nohost"    # FB-3: the same with the game's clock probe (logs\game_clock.csv; the rate: tools\framebreak\clock_rate.py)
    fbc_console  = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=console;SSPC_FB_CLOCK_PROBE=1"
    fbc_30       = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=30;SSPC_FB_CLOCK_PROBE=1"
    fbc_60       = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=60;SSPC_FB_CLOCK_PROBE=1"
    fbc_unl      = "SSPC_CP_MODE=native;SSPC_FB_FRAME_LIMIT=unlimited;SSPC_FB_CLOCK_PROBE=1"
    # GPU-6: written bindings in video memory shadowed again as before (vk_executor.cpp PrepareShadows)
    native_shkeep = "SSPC_CP_MODE=native;SSPC_GPU_SHADOW_VRAM=keep"
    # the frame lifecycle trace (tools\p54\p54_lifecycle.py, compute scheduling): not for FPS
    early_trace  = "SSPC_CP_MODE=native;SSPC_P54_EARLY_SUBMIT=1;SSPC_P54_FRAME_TRACE=1;SSPC_P412_STALL_TRACE=deep"
    # profiles (not for FPS): CPU samples of both GPU threads after 60 s; the Performance Inspector (GPU time per
    # effect category from timestamps at category changes, logs\performance_inspector\<time>)
    native_prof  = "SSPC_CP_MODE=native;SSPC_DIAG_SAMPLE=1;SSPC_DIAG_SAMPLE_GAME=GpuRecord;SSPC_DIAG_SAMPLE_AFTER=60"
    native_insp  = "SSPC_CP_MODE=native;SSPC_INSPECTOR=light,sample=0"
    early_insp   = "SSPC_CP_MODE=native;SSPC_P54_EARLY_SUBMIT=1;SSPC_INSPECTOR=light,sample=0"
    # P5.3.1-3 render modes (the parallel processor, the default): LEGACY / the first native pass (G-buffer, pass 1:
    # commit, draw, state, and the Native Render Frontend's cache) / that pass without the cache / HYBRID / NATIVE_EXP.
    legacy       = ""
    p1           = "SSPC_P53_NATIVE_DRAW=apply:1;SSPC_P53_NATIVE_STATE=apply;SSPC_P53_NATIVE_FRONTEND=1;$quiet"
    p1_nofe      = "SSPC_P53_NATIVE_DRAW=apply:1;SSPC_P53_NATIVE_STATE=apply;$quiet"
    hybrid       = "SSPC_RENDER_MODE=hybrid;SSPC_P53_NATIVE_DRAW_STATS=0"
    native       = "SSPC_RENDER_MODE=native_experimental;SSPC_P53_NATIVE_DRAW_STATS=0"
    # P5.3 marker transport A/B: HYBRID with its operations as PM4 markers / through the direct path (no markers:
    # native stream + descriptions in the draws' NOPs, SSPC_P53_DIRECT=1); both with the transport counters every 10 s
    hybrid_m     = "SSPC_RENDER_MODE=hybrid;SSPC_P53_NATIVE_DRAW_STATS=10"
    hybrid_d     = "SSPC_RENDER_MODE=hybrid;SSPC_P53_DIRECT=1;SSPC_P53_NATIVE_DRAW_STATS=10"
}
$out = "$root\logs\p53\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
"p53 perf series '$Tag': scenes $Scenes, repeats $Repeats, $Seconds s, configs $Configs, exe '$Exe'" | Out-File "$out\series.txt" -Encoding utf8
for ($rep = 1; $rep -le $Repeats; $rep++) {
    foreach ($scene in ($Scenes -split ',' | Where-Object { $_ })) {
        foreach ($cfg in ($Configs -split ',' | Where-Object { $_ })) {
            $cfgEnv = if ($cfg.EndsWith("_B")) { $cfg.Substring(0, $cfg.Length - 2) } else { $cfg }
            $cfgExe = if ($cfg.EndsWith("_B")) { $ExeB } else { $Exe }
            if (-not $defs.ContainsKey($cfgEnv)) { throw "unknown config '$cfg'" }
            $runTag = "${cfg}_r$rep"
            if (Test-Path "$out\$runTag\${scene}_1\stats.json") {
                continue # done in an earlier (interrupted) invocation of the same series: resumed, not repeated
            }
            # CPU time and working set of the game's process, once a second, while the run lasts
            $sampler = Start-Job -ArgumentList "$out\$runTag", ($Seconds + 40) -ScriptBlock {
                param($dir, $limit)
                New-Item -ItemType Directory -Force $dir | Out-Null
                $rows = @("t_s,pid,cpu_s,working_set_mib,private_mib")
                # FB-7 (the machine's slow mode): the CPU's clock, system context switches and each core's load (perf.csv;
                # WMI: the performance counters' names are localized in a background job)
                $perf = @("t_s,perf_pct,freq_mhz,ctx_per_s,cores_pct")
                $t0 = Get-Date
                while (((Get-Date) - $t0).TotalSeconds -lt $limit) {
                    $pi = Get-CimInstance Win32_PerfFormattedData_Counters_ProcessorInformation -ErrorAction SilentlyContinue
                    $tot = $pi | Where-Object Name -eq "_Total"
                    $sys = Get-CimInstance Win32_PerfFormattedData_PerfOS_System -ErrorAction SilentlyContinue
                    if ($tot -and $sys) {
                        $cores = ($pi | Where-Object { $_.Name -notmatch "_Total" } | ForEach-Object { $_.PercentProcessorTime }) -join " "
                        $perf += [string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:F1},{1},{2},{3},{4}",
                            ((Get-Date) - $t0).TotalSeconds, $tot.PercentProcessorPerformance, $tot.ProcessorFrequency,
                            $sys.ContextSwitchesPersec, $cores)
                    }
                    # (the game's process: the SecondSonPC with the most private memory - not a helper of the same name)
                    $p = Get-Process SecondSonPC -ErrorAction SilentlyContinue | Sort-Object PrivateMemorySize64 -Descending |
                        Select-Object -First 1
                    if ($p) {
                        $rows += [string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:F1},{1},{2:F2},{3:F0},{4:F0}",
                            ((Get-Date) - $t0).TotalSeconds, $p.Id, $p.TotalProcessorTime.TotalSeconds,
                            ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB))
                    }
                    Start-Sleep -Seconds 1
                }
                $rows | Out-File "$dir\cpu_mem_raw.csv" -Encoding ascii
                $perf | Out-File "$dir\perf_raw.csv" -Encoding ascii
            }
            $perfCsv = "$out\$runTag\perf_raw.csv"
            $runArgs = @("-ExecutionPolicy", "Bypass", "-File", "$root\tools\tests\p46_runs.ps1", "-Tag", $runTag,
                "-Scenes", $scene, "-Repeats", "1", "-Seconds", "$Seconds", "-OutRoot", $out)
            if ($defs[$cfgEnv]) { $runArgs += @("-Env", $defs[$cfgEnv]) } # (base: no variable at all)
            if ($cfgExe) { $runArgs += @("-Exe", $cfgExe) }
            if ($Press) { $runArgs += @("-Press", $Press) }
            powershell @runArgs | Out-Null
            if (-not (Test-Path "$out\$runTag\${scene}_1\stats.json")) {
                "$(Get-Date -Format s) $runTag $scene FAILED: no stats.json" | Out-File "$out\series.txt" -Encoding utf8 -Append
            }
            Wait-Job $sampler -Timeout 60 | Out-Null
            Stop-Job $sampler -ErrorAction SilentlyContinue
            Remove-Job $sampler -Force -ErrorAction SilentlyContinue
            if (Test-Path "$out\$runTag\cpu_mem_raw.csv") {
                Move-Item "$out\$runTag\cpu_mem_raw.csv" "$out\$runTag\${scene}_1\cpu_mem.csv" -Force
            }
            if (Test-Path $perfCsv) {
                Move-Item $perfCsv "$out\$runTag\${scene}_1\perf.csv" -Force
            }
            "$(Get-Date -Format s) $runTag $scene done" | Out-File "$out\series.txt" -Encoding utf8 -Append
        }
    }
}
py -I -X utf8 "$root\tools\tests\p53_perf_summary.py" $out
