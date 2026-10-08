# Series of automated game runs with a per-run summary (SSPC-001 verification, smoke tests).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\smoke_runs.ps1 -Loader sspc -Runs 3 [-Seconds 150] [-Prefix smoke] [-ExtraArgs "..."]
# Each run: SHA-256 of the isolated SecondSonPC saves before/after (read only), tools\run_phase0.ps1
# (WM_CLOSE to the game window of the started pid at the end, terminate only after a 15 s timeout;
# its logs\runs\<tag>\run.json gives "how" and the exit code), tools\analyze_run.py -> one JSON line
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
    # run_phase0.ps1 deletes the run's old run.json first, so a missing file means it failed.
    $runJson = "$root\logs\runs\$tag\run.json"
    $how = "?"; $code = $null
    if (Test-Path $runJson) {
        $run = Get-Content -Raw -Encoding UTF8 $runJson | ConvertFrom-Json
        $how = $run.how
        $code = $run.exit_code
    } else {
        "${tag}: run_phase0.ps1 wrote no run.json; its output:"
        $out
    }
    $pyArgs = @("$root\logs\runs\$tag", "--how", $how, "--save-before", "$tmp\$tag.before.json", "--save-after", "$tmp\$tag.after.json")
    if ($null -ne $code) { $pyArgs += @("--exit", "$code") }
    $line = & python -I "$root\tools\analyze_run.py" @pyArgs
    Add-Content -Encoding UTF8 $summary $line
    $s = $line | ConvertFrom-Json
    "{0}: {1} s, {2} exit={3} {4}, alive={5}, frames={6}, crash in logs={7}, VRAM max {8} MiB, commit max {9}/{10} MiB, pipelines {11}" -f $tag, $s.duration_s, $s.how, $s.exit, $s.exit_hex, $s.alive_at_end, $s.frames, $s.crash, $s.vram_local_max_mib, $s.commit_max_mib, $s.commit_limit_mib, $s.pipelines
}
