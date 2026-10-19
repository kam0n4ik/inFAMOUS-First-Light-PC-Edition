# Manual play session (docs\MANUAL_TEST_PLAN.md). Started by E:\SecondSonPC\play.cmd.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\manual_run.ps1 [-Mode sspc|shadps4|dmem|full|gpu|all]
#   (older form: [-Loader sspc|shadps4] [-NativeMemory]); further flags for SecondSonPC.exe: environment SSPC_EXTRA
# 1. Copies the isolated SecondSonPC saves to userdata\savedata_backups\<time>_before_manual_<mode>
#    (copy only; nothing is deleted or overwritten).
# 2. Runs SecondSonPC.exe with --vram-log 10 and the mode's flags and waits until the game window is closed (no time
#    limit). dmem: --native kernel-dmem (P2.3.1); full: every native subsystem of Phase 3 (the set of the P3.6 game
#    runs, perf_runs TMFXKE); gpu: --gpu-shaders (P4.2 shadow + P4.3 shader frontend) --frame-pacing; all: both.
# 3. Copies this session's logs to logs\manual\<time>_<mode>\ (the next start overwrites logs\*.log)
#    and prints a short summary (tools\analyze_run.py; with the shadow, its main lines).
# Saved as UTF-8 with BOM: Windows PowerShell 5.1 reads BOM-less files in the ANSI code page.
param(
    [ValidateSet("", "sspc", "shadps4", "dmem", "full", "gpu", "all")] [string]$Mode = "",
    [ValidateSet("sspc", "shadps4")] [string]$Loader = "sspc",
    [switch]$NativeMemory
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$stamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
if ($Mode -eq "") { $Mode = if ($NativeMemory) { "dmem" } else { $Loader } }
$full = "--native cpu --native exceptions --native threads --native memory --memory-commit on-demand " +
        "--native errno --native clock --native kernel-memory --native exit"
$gpu = "--gpu-shaders --frame-pacing" # --gpu-shaders turns the shadow (--gpu-shadow) on as well
$preset = switch ($Mode) { "dmem" { "--native kernel-dmem" } "full" { $full } "gpu" { $gpu } "all" { "$full $gpu" } default { "" } }
$Loader = if ($Mode -eq "shadps4") { "shadps4" } else { "sspc" }
$extra = "$env:SSPC_EXTRA".Trim()
$label = if ($extra) { "$Mode-extra" } else { $Mode }
$argLine = "--loader $Loader --vram-log 10 $preset $extra".Trim() -replace '\s+', ' '

$saves = "$root\userdata\home"
if (Test-Path $saves) {
    $backup = "$root\userdata\savedata_backups\${stamp}_before_manual_$label"
    New-Item -ItemType Directory -Force $backup | Out-Null
    Copy-Item "$saves\*" $backup -Recurse
    "Сохранения скопированы в $backup"
}

# the own GCN translator is the default (SSPC_OWN_SHADERS=bridge: the bridge's): its cache seeded from the main one when
# the translator changed, so that no module of an earlier own translator is used (docs\OWN_SHADER_TRANSLATOR.md)
& python -I "$root\tools\seed_own_shader_cache.py"
if ($LASTEXITCODE -ne 0) { throw "tools\seed_own_shader_cache.py failed ($LASTEXITCODE) - not started" }

"Запуск: SecondSonPC.exe $argLine (закройте окно игры, чтобы завершить сессию)"
$p = Start-Process -FilePath "$root\build\bin\SecondSonPC.exe" -ArgumentList $argLine -WorkingDirectory $root -PassThru
$p.WaitForExit()
$code = $p.ExitCode

$out = "$root\logs\manual\${stamp}_$label"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($f in "SecondSonPC.log", "bridge_shadps4.log", "calls.json", "call_order.txt", "gpu_shadow.txt",
               "frame_pacing.csv", "gpu_stats.txt", "gpu_frames.csv", "gpu_stats_windows.csv", "memory_map.txt") {
    if ((Test-Path "$root\logs\$f") -and (Get-Item "$root\logs\$f").LastWriteTime -ge $p.StartTime) {
        Copy-Item "$root\logs\$f" "$out\$f" -Force
    }
}
$line = & python -I "$root\tools\analyze_run.py" $out --exit $code --how exited
$line | Set-Content -Encoding UTF8 "$out\summary.json"
$s = $line | ConvertFrom-Json
$fps = "?"
if ($s.frames_series.Count -ge 2) {
    $a = $s.frames_series[0]; $b = $s.frames_series[-1]
    if ($b[0] -gt $a[0]) { $fps = [math]::Round(($b[1] - $a[1]) / ($b[0] - $a[0]), 1) }
}
""
"Сессия завершена. Код выхода: $code (0 = окно закрыто штатно)"
"Длительность: $($s.duration_s) с; кадров: $($s.frames); средний FPS: $fps"
"Сбой: $(if ($s.crash) { $s.crash } else { 'нет' })"
"VRAM (пик): $($s.vram_local_max_mib) МиБ из $($s.vram_budget_mib); память системы (commit, пик): $($s.commit_max_mib) из $($s.commit_limit_mib) МиБ"
$native = @(Select-String -Path "$out\SecondSonPC.log" -Pattern 'EXPERIMENTAL: --native (\S+)' -ErrorAction SilentlyContinue |
            ForEach-Object { $_.Matches[0].Groups[1].Value })
if ($native.Count -gt 0) { "Свои подсистемы SecondSonPC (--native): $($native -join ', ')" }
if (Test-Path "$out\gpu_shadow.txt") {
    ""
    "Тень GPU (P4.2/P4.3), полный отчёт: $out\gpu_shadow.txt"
    Select-String -Path "$out\gpu_shadow.txt" -Pattern '^SecondSonPC command processor shadow|^Divergences:|^draws \d+, dispatches|^translations:|^time per draw|^Against the bridge|^CLEAR_STATE / PS' |
        ForEach-Object { "  " + $_.Line }
}
"Логи этой сессии: $out"
