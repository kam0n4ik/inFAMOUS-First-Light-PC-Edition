# Runs FirstLightPC.exe --predict-shaders until it completes: a translation that brings the process down (an assertion
# in the bridge's recompiler) puts the programs it had started and not finished on the skip list, and the next run
# resumes (modules already in the cache are read, not translated again).
#   powershell -File predict_loop.ps1 <templates> <cache dir> [max runs]
param([string]$Templates, [string]$Cache, [int]$MaxRuns = 40, [string]$SeedSkip = '', [string]$Out = '')
$logs = 'E:\inFAMOUS-First-Light-PC-Edition\logs'
$trace = "$logs\static_predict_trace.txt"
$skip = "$logs\static_predict_skip.txt"
Remove-Item "$logs\static_predict_modules.tsv", $skip -ErrorAction SilentlyContinue
if ($SeedSkip) { Copy-Item $SeedSkip $skip }
$env:SSPC_STATIC_TRACE = '1'
for ($i = 1; $i -le $MaxRuns; $i++) {
    $p = Start-Process -FilePath E:\inFAMOUS-First-Light-PC-Edition\build\bin\FirstLightPC.exe -ArgumentList "--gpu-backend sspc --shader-cache $Cache --predict-shaders $Templates" -WorkingDirectory E:\inFAMOUS-First-Light-PC-Edition -PassThru -NoNewWindow -RedirectStandardOutput "$logs\static_predict_stdout.txt"
    $p.WaitForExit()
    if ($p.ExitCode -eq 0) { "run ${i}: done"; Get-Content "$logs\static_predict_stdout.txt"; break }
    $open = @{}
    foreach ($l in Get-Content $trace) {
        $f = $l.Split(' ')
        if ($f[0] -eq 'start') { $open[$f[1]] = 1 + [int]$open[$f[1]] }
        elseif ($f[0] -eq 'done') { $open[$f[1]] = [int]$open[$f[1]] - 1 }
    }
    $bad = @($open.Keys | Where-Object { $open[$_] -gt 0 })
    "run ${i}: exit $($p.ExitCode), skip $($bad -join ',')"
    if ($bad.Count -eq 0) { 'no unfinished translation: stopping'; break }
    Add-Content $skip $bad
}
if ($Out) { Copy-Item "$logs\static_predict_modules.tsv" $Out -Force }
