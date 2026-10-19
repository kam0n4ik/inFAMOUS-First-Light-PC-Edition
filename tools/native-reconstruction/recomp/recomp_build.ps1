# Static recompilation PoC: eboot functions -> Ghidra P-code -> C++ -> native DLL (docs/NATIVE_ENGINE_RECONSTRUCTION.md §9).
#   powershell -File recomp_build.ps1 [-Out E:\research\native-recon\recomp] [-Functions 1f0870,61e260,61f430,11c890]
# The generated C++ is derived from the game's code: it is written to -Out (outside the repository), never committed.
# Needs Ghidra (pyghidra, the read-only scripts project) and SecondSonPC's clang-cl toolchain (tools\env.ps1).
param(
    [string]$Out = "E:\research\native-recon\recomp",
    [string[]]$Functions = @("1f0870", "61e260", "61f430", "11c890")
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Resolve-Path "$here\..\..\.."
New-Item -ItemType Directory -Force $Out | Out-Null
$json = Join-Path $Out "pcode.json"
$cpp = Join-Path $Out "recomp_gen.cpp"
$report = Join-Path $Out "recomp_report.txt"

# 1. decoder + IR: Ghidra SLEIGH P-code of the functions
$env:GHIDRA_INSTALL_DIR = "E:\tools\ghidra"
$env:JAVA_HOME = "E:\tools\jdk21"
if (-not $env:SSPC_GHIDRA_PROJECT_DIR) { $env:SSPC_GHIDRA_PROJECT_DIR = "E:\tools\ghidra-projects\scripts" }
Push-Location "$root\tools\native-reconstruction\ghidra"
& E:\tools\ghidra-mcp\.venv\Scripts\python.exe -X utf8 run.py "$here\pcode_dump.py" $json @Functions 2>&1 |
    Where-Object { $_ -notmatch "^(INFO|WARN)" }
Pop-Location
if (-not (Test-Path $json)) { throw "no P-code dump" }

# 2. transformations + C++ emission
& py -I -X utf8 "$here\pcode2c.py" $json $cpp --report $report
if ($LASTEXITCODE -ne 0) { throw "pcode2c failed" }

# 3. native object: clang-cl -O2 (no FMA contraction, AVX like the guest code), linked as a DLL
. "$root\tools\env.ps1"
$env:TEMP = Join-Path $Out "tmp"; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
Push-Location $Out
cmd /c "clang-cl /nologo /O2 /arch:AVX -clang:-ffp-contract=off /EHsc /LD recomp_gen.cpp /Fe:recomp_poc.dll 2>&1"
$rc = $LASTEXITCODE
Pop-Location
if ($rc -ne 0) { throw "clang-cl failed ($rc)" }
Get-Item (Join-Path $Out "recomp_poc.dll") | Select-Object FullName, Length, LastWriteTime
