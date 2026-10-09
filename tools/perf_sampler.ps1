# Performance sampler for one SecondSonPC.exe run (P0 FPS investigation, logs\performance\).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\perf_sampler.ps1 -Out <csv> [-WaitSeconds 30] [-MaxSeconds 900]
# Waits for SecondSonPC.exe to start, then samples Windows performance counters once a second until
# the process exits. Read-only: it never touches the game process. Columns (one row per second):
#   t_s                 seconds since the process was found
#   cpu_pct             process CPU, % of ONE logical core (100 = one core busy)
#   cpu_total_pct       process CPU, % of all logical cores
#   threads             process thread count
#   private_mib         process private bytes (committed private memory)
#   ws_private_mib      process private working set (resident private memory)
#   virtual_mib         process virtual size (reserved + committed address space)
#   sys_commit_mib / sys_commit_limit_mib / avail_mib   system commit charge, its limit, free RAM
#   hard_faults_s       \Memory\Pages Input/sec (pages read from disk)
#   ctx_switch_s        \System\Context Switches/sec (whole system)
#   gpu3d_pct           utilisation of the process's busiest GPU engine type (GPU Engine counters,
#                       summed per engtype; Task Manager's "GPU" column shows the same maximum)
#   gpu_dedicated_mib / gpu_shared_mib   GPU memory of the process (GPU Process Memory counters)
param(
    [Parameter(Mandatory)][string]$Out,
    [int]$WaitSeconds = 30,
    [int]$MaxSeconds = 900,
    [string]$ProcessName = "SecondSonPC"
)
$ErrorActionPreference = "Stop"
# Started hidden by tools\perf_runs.ps1: messages and errors go to <csv>.log.
$diag = "$Out.log"
function Note([string]$Text) { Add-Content -Encoding UTF8 $diag ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $Text) }
trap { Note "error: $($_ | Out-String)"; break }
# PDH takes English counter names only while the thread's UI language is English. A hidden window
# (perf_runs.ps1) gets a new console whose code page makes Windows pick the Russian UI language:
# every \Process/\Memory/\System counter then silently fails (GPU counters have no localized names).
Add-Type -Namespace SspcPerf -Name Native -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern ushort SetThreadUILanguage(ushort langId);
'@
[void][SspcPerf.Native]::SetThreadUILanguage(0x0409)
[System.Threading.Thread]::CurrentThread.CurrentUICulture = [Globalization.CultureInfo]::GetCultureInfo("en-US")

$deadline = (Get-Date).AddSeconds($WaitSeconds)
$proc = $null
while (-not $proc -and (Get-Date) -lt $deadline) {
    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $proc) { Start-Sleep -Milliseconds 200 }
}
if (-not $proc) { "perf_sampler: $ProcessName did not start within $WaitSeconds s"; exit 1 }
$procId = $proc.Id
$cores = [Environment]::ProcessorCount

# PDH caches the instance list of a process's first query: instances that appear later (a game
# started after this script, its GPU engines) never match a wildcard. -ListSet re-reads the list, so
# the instances are resolved here, by name, once they exist.
function Resolve-Instances([string]$Set, [string]$Pattern) {
    try {
        @((Get-Counter -ListSet $Set -ErrorAction Stop).PathsWithInstances | Where-Object { $_ -like $Pattern })
    } catch { @() }
}
$instance = $null
$gpuPaths = @()
# The first query of this process fixes PDH's view: wait until the game has its GPU device.
try { $startedAt = $proc.StartTime } catch { $startedAt = Get-Date }
$ready = $startedAt.AddSeconds(8)
while ((Get-Date) -lt $ready -and -not $proc.HasExited) { Start-Sleep -Milliseconds 250 }
$resolveUntil = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $resolveUntil -and -not $proc.HasExited) {
    if (-not $instance) {
        foreach ($s in (Get-Counter '\Process(*)\ID Process' -ErrorAction SilentlyContinue).CounterSamples) {
            if ([int]$s.CookedValue -eq $procId) { $instance = $s.InstanceName; break }
        }
    }
    $gpuPaths = @(Resolve-Instances "GPU Engine" "*(pid_${procId}_*)\Utilization Percentage") +
                @(Resolve-Instances "GPU Process Memory" "*(pid_${procId}_*)\Dedicated Usage") +
                @(Resolve-Instances "GPU Process Memory" "*(pid_${procId}_*)\Shared Usage")
    if ($instance -and $gpuPaths.Count) { break }
    Start-Sleep -Milliseconds 1000
}
if (-not $instance) { $instance = $ProcessName.ToLower() }

$counters = @(
    "\Process($instance)\% Processor Time",
    "\Process($instance)\Thread Count",
    "\Process($instance)\Private Bytes",
    "\Process($instance)\Working Set - Private",
    "\Process($instance)\Virtual Bytes",
    "\Memory\Committed Bytes",
    "\Memory\Commit Limit",
    "\Memory\Available MBytes",
    "\Memory\Pages Input/sec",
    "\System\Context Switches/sec"
) + $gpuPaths

$dir = Split-Path -Parent $Out
if ($dir) { New-Item -ItemType Directory -Force $dir | Out-Null }
"t_s,cpu_pct,cpu_total_pct,threads,private_mib,ws_private_mib,virtual_mib,sys_commit_mib,sys_commit_limit_mib,avail_mib,hard_faults_s,ctx_switch_s,gpu3d_pct,gpu_dedicated_mib,gpu_shared_mib" |
    Set-Content -Encoding ASCII $Out
"perf_sampler: pid $procId (counter instance '$instance', $($gpuPaths.Count) GPU counters), $cores logical cores -> $Out"
Note "pid $procId, instance '$instance', $($counters.Count) counters"

$inv = [Globalization.CultureInfo]::InvariantCulture
$start = Get-Date
while (-not $proc.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $MaxSeconds) {
    $samples = $null
    try {
        $samples = (Get-Counter -Counter $counters -SampleInterval 1 -MaxSamples 1 -ErrorAction SilentlyContinue).CounterSamples
    } catch { Note "Get-Counter: $($_.Exception.Message)" }
    if (-not $samples) {
        if ($proc.HasExited) { break }
        Start-Sleep -Milliseconds 500
        continue
    }
    $v = @{}
    $engines = @{}; $ded = 0.0; $shared = 0.0
    foreach ($s in $samples) {
        $p = $s.Path.ToLower()
        if ($p -like "*\gpu engine(*") {
            # Busiest engine type of the process (Vulkan's graphics queue is not always "3D").
            $type = if ($p -match 'engtype_([^)]*)\)') { $Matches[1] } else { "?" }
            $engines[$type] = [double]$engines[$type] + $s.CookedValue
            continue
        }
        if ($p -like "*\gpu process memory(*\dedicated usage") { $ded += $s.CookedValue; continue }
        if ($p -like "*\gpu process memory(*\shared usage") { $shared += $s.CookedValue; continue }
        $v[$p.Substring($p.LastIndexOf('\') + 1)] = $s.CookedValue
    }
    if ($null -eq $v['% processor time']) {
        if (-not $noted) { $noted = $true; Note "no '% processor time' among: $(($samples | ForEach-Object { $_.Path }) -join ' | ')" }
        if ($proc.HasExited) { break } else { continue }
    }
    $gpu3d = 0.0
    foreach ($e in $engines.Values) { if ($e -gt $gpu3d) { $gpu3d = $e } }
    $t = ((Get-Date) - $start).TotalSeconds
    $row = [string]::Format($inv, "{0:F1},{1:F1},{2:F1},{3:F0},{4:F0},{5:F0},{6:F0},{7:F0},{8:F0},{9:F0},{10:F1},{11:F0},{12:F1},{13:F0},{14:F0}",
        $t, $v['% processor time'], ($v['% processor time'] / $cores), $v['thread count'],
        ($v['private bytes'] / 1MB), ($v['working set - private'] / 1MB), ($v['virtual bytes'] / 1MB),
        ($v['committed bytes'] / 1MB), ($v['commit limit'] / 1MB), $v['available mbytes'],
        $v['pages input/sec'], $v['context switches/sec'], $gpu3d, ($ded / 1MB), ($shared / 1MB))
    Add-Content -Encoding ASCII $Out $row
}
"perf_sampler: done after $([math]::Round(((Get-Date) - $start).TotalSeconds)) s"
