# Series of automated game runs with a per-run summary (SSPC-001 verification, smoke tests).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\smoke_runs.ps1 -Loader sspc -Runs 3 [-Seconds 150] [-Prefix smoke] [-ExtraArgs "..."]
# Each run: SHA-256 of the isolated SecondSonPC saves before/after (read only), tools\run_phase0.ps1
# (WM_CLOSE at the end, kill only after a 15 s timeout), tools\analyze_run.py -> one JSON line
# appended to logs\runs\<Prefix>_summary.jsonl. Saves are never modified or deleted by this script.
param(
    [ValidateSet("sspc", "shadps4")] [string]$Loader = "sspc",
    [int]$Runs = 1,
    [int]$Seconds = 150,
    [string]$Prefix = "smoke",
    [string]$ExtraArgs = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$saves = "$root\userdata\home"
$summary = "$root\logs\runs\${Prefix}_summary.jsonl"
$tmp = "$root\logs\runs\_hash"
New-Item -ItemType Directory -Force $tmp | Out-Null

function Save-Hashes([string]$file) {
    $map = [ordered]@{}
    if (Test-Path $saves) {
        Get-ChildItem $saves -Recurse -File | Where-Object { $_.FullName -match '\\savedata\\' } | ForEach-Object {
            $map[$_.FullName.Substring($saves.Length + 1)] = (Get-FileHash $_.FullName -Algorithm SHA256).Hash
        }
    }
    $map | ConvertTo-Json -Compress | Set-Content -Encoding UTF8 $file
}

for ($i = 1; $i -le $Runs; ++$i) {
    $tag = "{0}_{1}_{2}" -f $Prefix, $Loader, $i
    Save-Hashes "$tmp\$tag.before.json"
    $runArgs = "--loader $Loader --vram-log 5"
    if ($ExtraArgs) { $runArgs = "$runArgs $ExtraArgs" }
    $out = powershell -ExecutionPolicy Bypass -File "$root\tools\run_phase0.ps1" -Seconds $Seconds -Tag $tag -NoReport -ExtraArgs $runArgs 2>&1 | Out-String
    Save-Hashes "$tmp\$tag.after.json"
    $how = if ($out -match "exited by itself") { "exited" } elseif ($out -match "killed") { "killed" } elseif ($out -match "closed gracefully") { "closed" } else { "?" }
    $code = if ($out -match "exit=(-?\d+)") { $Matches[1] } else { "" }
    $pyArgs = @("$root\logs\runs\$tag", "--how", $how, "--save-before", "$tmp\$tag.before.json", "--save-after", "$tmp\$tag.after.json")
    if ($code) { $pyArgs += @("--exit", $code) }
    $line = & python -I "$root\tools\analyze_run.py" @pyArgs
    Add-Content -Encoding UTF8 $summary $line
    $s = $line | ConvertFrom-Json
    "{0}: {1} s, {2} exit={3}, alive={4}, frames={5}, crash={6}, VRAM max {7} MiB, commit max {8}/{9} MiB, pipelines {10}" -f $tag, $s.duration_s, $s.how, $s.exit, $s.alive_at_end, $s.frames, $s.crash, $s.vram_local_max_mib, $s.commit_max_mib, $s.commit_limit_mib, $s.pipelines
}
