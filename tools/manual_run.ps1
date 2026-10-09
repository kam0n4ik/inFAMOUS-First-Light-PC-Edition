# Manual play session (docs\MANUAL_TEST_PLAN.md). Started by E:\SecondSonPC\play.cmd.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\manual_run.ps1 [-Loader sspc|shadps4] [-NativeMemory]
# 1. Copies the isolated SecondSonPC saves to userdata\savedata_backups\<time>_before_manual_<loader>
#    (copy only; nothing is deleted or overwritten).
# 2. Runs SecondSonPC.exe with --vram-log 10 and waits until the game window is closed (no time limit).
#    -NativeMemory adds --native kernel-dmem (experimental native direct memory, P2.3.1; sspc only).
# 3. Copies this session's logs to logs\manual\<time>_<loader>\ (the next start overwrites logs\*.log)
#    and prints a short summary (tools\analyze_run.py).
# Saved as UTF-8 with BOM: Windows PowerShell 5.1 reads BOM-less files in the ANSI code page.
param(
    [ValidateSet("sspc", "shadps4")] [string]$Loader = "sspc",
    [switch]$NativeMemory
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$stamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
$label = if ($NativeMemory) { "$Loader-dmem" } else { $Loader }
$gameArgs = @("--loader", $Loader, "--vram-log", "10")
if ($NativeMemory) { $gameArgs += @("--native", "kernel-dmem") }

$saves = "$root\userdata\home"
if (Test-Path $saves) {
    $backup = "$root\userdata\savedata_backups\${stamp}_before_manual_$label"
    New-Item -ItemType Directory -Force $backup | Out-Null
    Copy-Item "$saves\*" $backup -Recurse
    "Сохранения скопированы в $backup"
}

"Запуск: SecondSonPC.exe $($gameArgs -join ' ') (закройте окно игры, чтобы завершить сессию)"
$p = Start-Process -FilePath "$root\build\bin\SecondSonPC.exe" -ArgumentList $gameArgs -WorkingDirectory $root -PassThru
$p.WaitForExit()
$code = $p.ExitCode

$out = "$root\logs\manual\${stamp}_$label"
New-Item -ItemType Directory -Force $out | Out-Null
foreach ($f in "SecondSonPC.log", "bridge_shadps4.log", "calls.json", "call_order.txt") {
    if (Test-Path "$root\logs\$f") { Copy-Item "$root\logs\$f" "$out\$f" -Force }
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
"Логи этой сессии: $out"
