# SSPC-001 reproducer (docs/KNOWN_ISSUES.md): a non-empty sceSystemServiceParamGetString(SystemName)
# makes the game crash ~20-30 s after start (after it writes /savedata0/dis.dat).
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\repro_sspc001.ps1 [-Loader sspc|shadps4] [-Seconds 90] [-Name PS4-SSPC001]
# Runs SecondSonPC.exe once with --system-name <Name> (SecondSonPC.toml is not touched; the default
# empty name stays the workaround) and reports REPRODUCED (the game died before -Seconds) or
# NOT REPRODUCED (still running, closed by the script). Evidence: logs\runs\sspc001_<loader>.
param(
    [ValidateSet("sspc", "shadps4")][string]$Loader = "shadps4",
    [int]$Seconds = 90,
    [string]$Name = "PS4-SSPC001"
)
$ErrorActionPreference = "Stop"
if ($Name -match '\s') { throw "-Name must not contain spaces" }
$tag = "sspc001_$Loader"
$out = & powershell -ExecutionPolicy Bypass -File "$PSScriptRoot\run_phase0.ps1" -Seconds $Seconds -Tag $tag `
    -ExtraArgs "--loader $Loader --system-name $Name"
$out | ForEach-Object { $_ }
$crashed = ($out | Select-String -SimpleMatch "exited by itself") -ne $null
$log = "E:\SecondSonPC\logs\runs\$tag\SecondSonPC.log"
$name_line = if (Test-Path $log) { Select-String -Path $log -SimpleMatch "ParamGetString(SystemName" | Select-Object -First 1 } else { $null }
""
"SystemName returned to the game: $(if ($name_line) { $name_line.Line } else { '(no ParamGetString call logged)' })"
if ($crashed) {
    "SSPC-001: REPRODUCED (loader $Loader): the game exited before $Seconds s; evidence in logs\runs\$tag"
    exit 1
}
"SSPC-001: NOT REPRODUCED (loader $Loader): the game ran $Seconds s with SystemName '$Name'"
exit 0
