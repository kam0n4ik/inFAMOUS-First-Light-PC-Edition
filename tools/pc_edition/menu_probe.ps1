# PC Edition UI research: a scripted run that opens and closes the game's pause menu (Esc) and snapshots the game's
# writable image in each state (SSPC_DIAG_MEMSNAP) -> logs\pc_edition\<Tag>\ (memdiff.py finds the state's flags).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\pc_edition\menu_probe.ps1 -Tag t1
#       [-Press "<--press script>"] [-Snaps "50,58,..."] [-Seconds 90]
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Press = "space@12,space@22,esc@50,shot@54,esc@58,shot@62,esc@66,shot@70,esc@74,shot@78",
    [string]$Snaps = "48,55,63,71,79",
    [int]$Seconds = 85,
    [string]$Save = "E:\SecondSonPC\logs\performance\p43_ab\snapshot\home",
    [string]$ExtraArgs = ""
)
$ErrorActionPreference = "Stop"
$root = "E:\SecondSonPC"
$out = "$root\logs\pc_edition\$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$profileDir = "$root\logs\pc_edition\profile"
if (Test-Path $profileDir) { Remove-Item $profileDir -Recurse -Force }
New-Item -ItemType Directory -Force $profileDir | Out-Null
Copy-Item $Save "$profileDir\home" -Recurse
$env:SSPC_DIAG_MEMSNAP = $Snaps
$env:SSPC_DIAG_MEMSNAP_DIR = $out
$gameArgs = "--loader sspc --gpu-backend sspc --user-data $profileDir --press $Press"
if ($ExtraArgs) { $gameArgs += " $ExtraArgs" }
powershell -ExecutionPolicy Bypass -File "$root\tools\run_phase0.ps1" -Seconds $Seconds -Tag "pced_$Tag" -NoReport -ExtraArgs $gameArgs 2>&1 | Out-File "$out\run_console.txt"
Copy-Item "$root\logs\SecondSonPC.log" $out -ErrorAction SilentlyContinue
if (Test-Path "$root\logs\shots") { Copy-Item "$root\logs\shots\*" $out -ErrorAction SilentlyContinue }
Get-ChildItem $out | Select-Object Name, Length
