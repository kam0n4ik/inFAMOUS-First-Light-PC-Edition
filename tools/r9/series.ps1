# R9 stand series (docs/R9_GAMEPLAY_PERFORMANCE.md §4): configurations of one exe, rounds in ABBA order, each run a
# tools\r9\r9run.ps1 of the same route and scene; the background checked before every run (gate) and after it (a run
# measured with more than $Dirty % of the CPUs used by other processes is renamed <tag>_dirty<k> and run again, at most
# $Tries times - as E:\research\deep_perf\tools\batch10.ps1). No profiler unless a configuration sets it. Nothing is closed.
#   powershell -File tools\r9\series.ps1 -Name <series> -Route traverse -Scene city -Rounds 6
#       -Configs "base|#f1|SSPC_R9_PCACHE=1"      (name|env;env ... configurations separated by #)
param([Parameter(Mandatory)] [string]$Name, [Parameter(Mandatory)] [string]$Configs, [string]$Route = "traverse",
      [ValidateSet("city", "user")] [string]$Scene = "city", [int]$Rounds = 6, [double]$Gate = 8, [double]$Dirty = 10,
      [int]$Tries = 3, [int]$GateMinutes = 15, [string]$Out = "E:\research\r9\runs")
$ErrorActionPreference = "Continue"
$list = @($Configs.Split('#', [StringSplitOptions]::RemoveEmptyEntries) | ForEach-Object {
    $n, $e = $_.Split('|', 2); [pscustomobject]@{ Name = $n.Trim(); Env = "$e".Trim() } })
$dir = Join-Path $Out $Name
New-Item -ItemType Directory -Force $dir | Out-Null
"series $Name, route $Route, scene $Scene, rounds $Rounds, gate $Gate%, dirty over $Dirty% ($Tries tries), started $(Get-Date -Format 's')" |
    Add-Content "$dir\series.txt"
$list | Format-Table -AutoSize | Out-String -Width 400 | Add-Content "$dir\series.txt"

function Measure-Background {
    $c = New-Object System.Diagnostics.PerformanceCounter("Processor", "% Processor Time", "_Total")
    [void]$c.NextValue(); $v = @()
    for ($i = 0; $i -lt 10; $i++) { Start-Sleep -Seconds 1; $v += $c.NextValue() }
    return [math]::Round(($v | Measure-Object -Average).Average, 1)
}

for ($r = 1; $r -le $Rounds; $r++) {
    $order = @($list)
    if ($r % 2 -eq 0) { $order = @($list)[($list.Count - 1)..0] }
    foreach ($c in $order) {
        $tag = "$($c.Name)_$r"
        if (Test-Path "$dir\$tag\stats.json") { continue } # (a series run again goes on where it stopped)
        if (Test-Path "$dir\$tag") { Rename-Item "$dir\$tag" "$($tag)_aborted$(Get-Date -Format 'HHmmss')" }
        for ($k = 1; $k -le $Tries; $k++) {
            $bg = Measure-Background
            $deadline = (Get-Date).AddMinutes($GateMinutes)
            while ($bg -ge $Gate -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 20; $bg = Measure-Background }
            "$(Get-Date -Format 'HH:mm:ss') round $r $tag" | Add-Content "$dir\series.txt"
            "  gate: background $bg%$(if ($bg -ge $Gate) { " (over the gate after $GateMinutes min, run anyway)" })" |
                Add-Content "$dir\series.txt"
            & "$PSScriptRoot\r9run.ps1" -Tag $tag -Route $Route -Scene $Scene -Env $c.Env -Out $dir | Select-Object -Last 1
            $during = [double](& python -I "E:\research\deep_perf\tools\bgof.py" "$dir\$tag")
            if ($during -le $Dirty -and $during -ge 0) {
                "  measured with background $during%" | Add-Content "$dir\series.txt"
                break
            }
            Rename-Item "$dir\$tag" "$($tag)_dirty$k"
            if ($k -eq $Tries) {
                "  background $during% while measured - renamed $($tag)_dirty$k, the last try: left out" |
                    Add-Content "$dir\series.txt"
                break
            }
            "  background $during% while measured - renamed $($tag)_dirty$k, again" | Add-Content "$dir\series.txt"
        }
    }
}
"$(Get-Date -Format 'HH:mm:ss') done" | Add-Content "$dir\series.txt"
$summary = & python -I -X utf8 "$PSScriptRoot\ab_summary.py" $dir
$summary | Set-Content -Encoding UTF8 "$dir\summary.txt" # (Tee-Object of PowerShell 5.1 writes UTF-16)
$summary
