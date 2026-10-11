# SecondSonPC P4.8 Performance Inspector: a session with the inspector on, then its report (docs\PHASE4_P48_PERFORMANCE_INSPECTOR.md).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\performance_inspector.ps1 [-Mode Light|Deep]
#       [-Scene beach|early|user1022] [-DurationSeconds 90] [-Session <name>] [-GpuEvery 1] [-SampleHz 250]
#       [-Phase "name=from-to"] [-Exe <SecondSonPC.exe>] [-Compare <other session dir>] [-NoReport]
# Without -Scene: a play session like play_native.cmd (own loader, --gpu-backend sspc, --vram-log 10; the saves are
#   copied to userdata\savedata_backups first - copy only), until the game window is closed. Ctrl+F12 in the game
#   marks a moment (it shows in the report). Any length: the data streams to disk once a second.
# With -Scene: the measurement route of tools\tests\p46_runs.ps1 (a fresh copy of that scene's save, title ->
#   Continue, three camera turns at 56..60 s), closed after -DurationSeconds; the report compares 30-55 s with
#   62 s..end (before / after the camera turn).
# Results: logs\performance_inspector\<session>\ - report.html, summary.json and the raw files (frames.csv, ...).
# play.cmd, play_native.cmd and tools\manual_run.ps1 are not touched. Saved as UTF-8 with BOM (Windows PowerShell 5.1).
param(
    [ValidateSet("Light", "Deep")][string]$Mode = "Light",
    [ValidateSet("", "beach", "early", "user1022")][string]$Scene = "",
    [int]$DurationSeconds = 90,
    [string]$Session = "",
    [int]$GpuEvery = 1,
    [int]$SampleHz = 250,
    [string[]]$Phase = @(),
    [string]$Exe = "E:\SecondSonPC\build\bin\SecondSonPC.exe",
    [string]$Compare = "",
    [switch]$NoReport
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$stamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
if (-not $Session) { $Session = if ($Scene) { "${stamp}_$Scene" } else { "${stamp}_play" } }
$dir = "$root\logs\performance_inspector\$Session"
New-Item -ItemType Directory -Force $dir | Out-Null
if ($Mode -eq "Deep") {
    "DEEP-захват появится в P4.8.2; сейчас - режим Light."
}
$spec = "light,gpu=$GpuEvery,sample=$SampleHz"

if ($Scene) {
    $env_list = "SSPC_INSPECTOR=$spec;SSPC_INSPECTOR_DIR=$dir"
    "Сцена $Scene, $DurationSeconds с, Performance Inspector ($spec) -> $dir"
    $ErrorActionPreference = "Continue"
    powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\tests\p46_runs.ps1" -Tag "runs" -OutRoot $dir `
        -Scenes $Scene -Repeats 1 -Seconds $DurationSeconds -Env $env_list -Exe $Exe | Select-Object -Last 1
    $ErrorActionPreference = "Stop"
    $run = "$dir\runs\${Scene}_1"
    foreach ($f in "SecondSonPC.log", "screen.png", "stats.json") {
        if (Test-Path "$run\$f") { Copy-Item "$run\$f" $dir -Force }
    }
    if ($Phase.Count -eq 0) { $Phase = @("до поворота камеры=30-55", "после поворота камеры=62-$($DurationSeconds - 2)") }
} else {
    $saves = "$root\userdata\home"
    if (Test-Path $saves) {
        $backup = "$root\userdata\savedata_backups\${stamp}_before_inspector"
        New-Item -ItemType Directory -Force $backup | Out-Null
        Copy-Item "$saves\*" $backup -Recurse
        "Сохранения скопированы в $backup"
    }
    $argLine = "--loader sspc --vram-log 10 --gpu-backend sspc $env:SSPC_EXTRA".Trim()
    $env:SSPC_INSPECTOR = $spec
    $env:SSPC_INSPECTOR_DIR = $dir
    "Запуск: SecondSonPC.exe $argLine"
    "Performance Inspector ($spec) -> $dir"
    "Играйте как обычно; Ctrl+F12 - отметить момент. Закройте окно игры, чтобы завершить сессию и получить отчёт."
    $p = Start-Process -FilePath $Exe -ArgumentList $argLine -WorkingDirectory $root -PassThru
    $p.WaitForExit()
    Remove-Item Env:\SSPC_INSPECTOR, Env:\SSPC_INSPECTOR_DIR -ErrorAction SilentlyContinue
    "Игра завершилась с кодом $($p.ExitCode)"
    foreach ($f in "SecondSonPC.log", "gpu_frames.csv") {
        if ((Test-Path "$root\logs\$f") -and (Get-Item "$root\logs\$f").LastWriteTime -ge $p.StartTime) {
            Copy-Item "$root\logs\$f" $dir -Force
        }
    }
    if ($Phase.Count -eq 0) { $Phase = @("вся сессия без загрузки=20-") }
}

if (-not (Test-Path "$dir\frames.csv")) {
    "Нет $dir\frames.csv: инспектор не запустился (нужен --gpu-backend sspc; см. SecondSonPC.log)."
    exit 1
}
if (-not $NoReport) {
    $pargs = @("$root\tools\performance_inspector.py", "report", $dir)
    foreach ($ph in $Phase) { $pargs += @("--phase", $ph) }
    $ErrorActionPreference = "Continue"
    py -I @pargs
    if ($Compare) { py -I "$root\tools\performance_inspector.py" compare $Compare $dir }
    $ErrorActionPreference = "Stop"
    "Отчёт: $dir\report.html"
}
