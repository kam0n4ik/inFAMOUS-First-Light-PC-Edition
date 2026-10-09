# P2.5: kernel ETW trace (xperf) of one SecondSonPC.exe run, for tools\etw_profile.py.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\etw_capture.ps1 -Out <dir> [-Delay 50] [-Duration 20]
#       [-Profile 1000] [-NoStacks] [-WaitSeconds 60]
# Waits for SecondSonPC.exe to start (start it separately, e.g. tools\perf_runs.ps1), then -Delay s after
# the process start records -Duration s of: CPU samples (-Profile Hz, call stacks unless -NoStacks),
# context switches and ready-thread events (no stacks), process/thread/image rundown and thread names.
# Writes <dir>\trace.etl and <dir>\capture.json (pid, start/stop times relative to the process start).
# Needs an elevated shell (kernel logger). Read-only towards the game: it never touches the process.
# Cost: the trace window's FPS is compared with the rest of the run by tools\etw_profile.py.
param(
    [Parameter(Mandatory)][string]$Out,
    [int]$Delay = 50,
    [int]$Duration = 20,
    [int]$Profile = 1000,
    [switch]$NoStacks,
    [int]$WaitSeconds = 60,
    [string]$ProcessName = "SecondSonPC"
)
$ErrorActionPreference = "Stop"
$xperf = "C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe"
if (-not (Test-Path $xperf)) { "etw_capture: xperf.exe not found ($xperf)"; exit 1 }
if ($Profile -lt 100 -or $Profile -gt 8000) { "etw_capture: -Profile must be 100..8000 Hz"; exit 1 }
New-Item -ItemType Directory -Force $Out | Out-Null
$kernel = Join-Path $Out "kernel.etl"
$trace = Join-Path $Out "trace.etl"

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$proc = $null
while (-not $proc -and (Get-Date) -lt $deadline) {
    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $proc) { Start-Sleep -Milliseconds 200 }
}
if (-not $proc) { "etw_capture: $ProcessName did not start within $WaitSeconds s"; exit 1 }
$started = $proc.StartTime
$wait = ($started.AddSeconds($Delay) - (Get-Date)).TotalMilliseconds
if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]$wait) }
if ($proc.HasExited) { "etw_capture: $ProcessName exited before the trace window"; exit 1 }

# 100 ns units: 10000 = 1 ms.
$interval = [int](10000000 / $Profile)
$stackArgs = if ($NoStacks) { @() } else { @("-stackwalk", "Profile") }
$t0 = ((Get-Date) - $started).TotalSeconds
& $xperf -on "PROC_THREAD+LOADER+PROFILE+CSWITCH+DISPATCHER" @stackArgs -SetProfInt $interval `
    -BufferSize 1024 -MinBuffers 256 -MaxBuffers 2048 -f $kernel | Out-Null
if ($LASTEXITCODE -ne 0) { "etw_capture: xperf -on failed ($LASTEXITCODE)"; exit 1 }
$t1 = ((Get-Date) - $started).TotalSeconds
Start-Sleep -Seconds $Duration
$t2 = ((Get-Date) - $started).TotalSeconds
& $xperf -d $trace | Out-Null
$code = $LASTEXITCODE
$t3 = ((Get-Date) - $started).TotalSeconds
Remove-Item $kernel -ErrorAction SilentlyContinue
[ordered]@{
    pid = $proc.Id; process_start = $started.ToString("o"); profile_hz = $Profile; stacks = -not $NoStacks
    on_called_s = [math]::Round($t0, 2); on_done_s = [math]::Round($t1, 2)
    stop_called_s = [math]::Round($t2, 2); merged_s = [math]::Round($t3, 2); xperf_exit = $code
} | ConvertTo-Json | Set-Content -Encoding ASCII (Join-Path $Out "capture.json")
"etw_capture: pid $($proc.Id), trace $([math]::Round($t1,1))..$([math]::Round($t2,1)) s after start -> $trace (xperf exit $code)"
