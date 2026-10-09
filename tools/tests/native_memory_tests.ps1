# P2.3.4 (docs/PHASE2_P234_NATIVE_MEMORY.md): repeated start-ups of SecondSonPC's memory manager.
# Runs `--selftest --native memory` -Runs times with each commit mode (upfront, on-demand) and `--selftest` (the bridge,
# the default) once: every run must exit 0 with "TOTAL ... - PASS". Each run is a fresh process: the reservation of
# the game's address space, the section, the selection and the bridge's libraries start over each time.
#   powershell -ExecutionPolicy Bypass -File tools\tests\native_memory_tests.ps1 [-Runs 3] [-Exe build\bin\SecondSonPC.exe]
param(
    [int]$Runs = 3,
    [string]$Exe = "$PSScriptRoot\..\..\build\bin\SecondSonPC.exe"
)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\..\.."
$Exe = (Resolve-Path $Exe).Path
if (Get-Process SecondSonPC -ErrorAction SilentlyContinue) {
    Write-Host "SecondSonPC is running: not starting a second one"; exit 2
}
$os = Get-CimInstance Win32_OperatingSystem
$freeGiB = $os.FreeVirtualMemory / 1MB
if ($freeGiB -lt 14) {
    Write-Host ("free commit {0:N1} GiB < 14 GiB (upfront takes 8.4 GiB, the game scenario on demand 4.6 GiB)" -f $freeGiB); exit 2
}

$results = @()
function Invoke-SelfTest([string]$label, [string]$arguments, [string]$report) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Exe -ArgumentList $arguments -WorkingDirectory $root -PassThru -Wait -WindowStyle Minimized
    $total = Get-Content (Join-Path $root "logs\$report") | Select-String -Pattern '^TOTAL' | Select-Object -Last 1
    $ok = $p.ExitCode -eq 0 -and $null -ne $total -and $total.Line -match 'PASS$'
    $line = "{0,-34} exit {1}, {2,3} s, {3}" -f $label, $p.ExitCode, [int]$sw.Elapsed.TotalSeconds, $(if ($total) { $total.Line } else { 'no TOTAL line' })
    Write-Host "$(if ($ok) { 'PASS' } else { 'FAIL' })  $line"
    return [pscustomobject]@{ Label = $label; Ok = $ok; Line = $line }
}

for ($i = 1; $i -le $Runs; $i++) {
    $results += Invoke-SelfTest "native upfront #$i" "--selftest --native memory" "selftest_native_memory.txt"
    $results += Invoke-SelfTest "native on-demand #$i" "--selftest --native memory --memory-commit on-demand" "selftest_native_memory.txt"
}
$results += Invoke-SelfTest "bridge (default)" "--selftest" "selftest.txt"

$failed = @($results | Where-Object { -not $_.Ok })
Write-Host ("{0} runs, {1} failed" -f $results.Count, $failed.Count)
exit $(if ($failed.Count -eq 0) { 0 } else { 1 })
