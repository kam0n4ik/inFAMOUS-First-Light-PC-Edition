# Runs SecondSonPC's self-tests in the configurations each stage re-checks, one after another (they share
# logs\SecondSonPC.log and the report files), copies each report to -Out and prints one line per run.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\tests\selftest_matrix.ps1 -Out <dir> [-Exe <exe>] [-Only a,b]
# Names: default, exc, full, exit, gpu, fault, patchrace, boundary. Exit code 0 only if every selected run passed.
param(
    [string]$Exe = "E:\SecondSonPC\build\bin\SecondSonPC.exe",
    [Parameter(Mandatory = $true)][string]$Out,
    [string[]]$Only = @()
)
$ErrorActionPreference = "Continue" # native stderr is output here, not an error (PowerShell 5.1)
$root = "E:\SecondSonPC"
$logs = "$root\logs"
New-Item -ItemType Directory -Force $Out | Out-Null

$full = @("--native", "cpu", "--native", "exceptions", "--native", "threads", "--native", "memory",
          "--memory-commit", "on-demand", "--native", "errno", "--native", "clock", "--native", "kernel-memory",
          "--native", "exit")
$runs = @(
    @{ Name = "default";   Args = @("--selftest");                                                         Report = "selftest.txt";  Pass = "^TOTAL .* - PASS" },
    @{ Name = "exc";       Args = @("--selftest", "--native", "exceptions", "--native", "errno", "--native", "clock"); Report = "selftest.txt"; Pass = "^TOTAL .* - PASS" },
    # with --native memory the self-test is SecondSonPC's memory manager started as for the game
    @{ Name = "full";      Args = @("--selftest") + $full;                                                 Report = "selftest_native_memory.txt"; Pass = "^TOTAL .* - PASS" },
    @{ Name = "exit";      Args = @("--selftest", "--native", "exit");                                     Report = "selftest.txt";  Pass = "^TOTAL .* - PASS" },
    @{ Name = "fault";     Args = @("--fault-test");                                                       Report = "faulttest.txt"; Pass = "^fault test: .* - PASS" },
    @{ Name = "patchrace"; Args = @("--patch-race-test");                                                  Report = "patchrace.txt"; Pass = "; PASS$" }
)

$failed = 0
foreach ($r in $runs) {
    if ($Only.Count -gt 0 -and $Only -notcontains $r.Name) { continue }
    $report = Join-Path $logs $r.Report
    if (Test-Path $report) { Remove-Item -Force $report }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Exe -ArgumentList $r.Args -WorkingDirectory $root -PassThru -Wait -WindowStyle Hidden
    $sw.Stop()
    $line = "(no report)"
    $ok = $false
    # the log always (a run that ends without its report is the one whose log is needed)
    Copy-Item -Force "$logs\SecondSonPC.log" (Join-Path $Out ("{0}_SecondSonPC.log" -f $r.Name)) -ErrorAction SilentlyContinue
    if (Test-Path $report) {
        Copy-Item -Force $report (Join-Path $Out ("{0}_{1}" -f $r.Name, $r.Report))
        $hit = @(Select-String -Path $report -Pattern $r.Pass)
        $last = @(Get-Content $report -Tail 1)
        $line = if ($hit.Count -gt 0) { $hit[-1].Line } elseif ($last.Count -gt 0) { $last[0] } else { "(empty report)" }
        $ok = $hit.Count -gt 0 -and $p.ExitCode -eq 0
    }
    if (-not $ok) { $failed++ }
    "{0,-10} exit {1,3}  {2,6:N1} s  {3}  {4}" -f $r.Name, $p.ExitCode, $sw.Elapsed.TotalSeconds, $(if ($ok) { "PASS" } else { "FAIL" }), $line
}

if ($Only.Count -eq 0 -or $Only -contains "boundary") {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $txt = & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\tests\boundary_check_tests.ps1" 2>&1 | Out-String
    $code = $LASTEXITCODE
    $sw.Stop()
    Set-Content -Path (Join-Path $Out "boundary.txt") -Value $txt -Encoding utf8
    $last = ($txt.Trim() -split "`r?`n")[-1]
    if ($code -ne 0) { $failed++ }
    "{0,-10} exit {1,3}  {2,6:N1} s  {3}  {4}" -f "boundary", $code, $sw.Elapsed.TotalSeconds, $(if ($code -eq 0) { "PASS" } else { "FAIL" }), $last
}
exit $(if ($failed -eq 0) { 0 } else { 1 })
