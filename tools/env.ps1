# Build environment for SecondSonPC. Everything lives on E: (no installed Windows SDK, no vcvars).
#   . E:\SecondSonPC\tools\env.ps1      (dot-source into the current PowerShell session)
$ErrorActionPreference = "Stop"

$SSPC_Root    = "E:\SecondSonPC"
$SSPC_TC      = "$SSPC_Root\toolchain"
$SSPC_MSVC    = (Get-ChildItem "E:\BuildTools\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1).FullName
$SSPC_SdkVer  = "10.0.26100.0"
$SSPC_SdkInc  = "$SSPC_TC\winsdk\cpp\c\Include\$SSPC_SdkVer"
$SSPC_SdkLib  = "$SSPC_TC\winsdk\cpp.x64\c"
$SSPC_SdkBin  = "$SSPC_TC\winsdk\buildtools\bin\$SSPC_SdkVer\x64"
$SSPC_CMake   = "E:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$SSPC_Ninja   = "E:\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
$SSPC_LLVM    = "$SSPC_TC\LLVM\bin"

foreach ($p in @($SSPC_MSVC, $SSPC_SdkInc, $SSPC_SdkLib, $SSPC_SdkBin, $SSPC_CMake, $SSPC_Ninja, $SSPC_LLVM)) {
    if (-not (Test-Path $p)) { throw "toolchain path missing: $p" }
}

$env:INCLUDE = @(
    "$SSPC_MSVC\include",
    "$SSPC_SdkInc\ucrt", "$SSPC_SdkInc\um", "$SSPC_SdkInc\shared", "$SSPC_SdkInc\winrt", "$SSPC_SdkInc\cppwinrt"
) -join ";"
$env:LIB = @("$SSPC_MSVC\lib\x64", "$SSPC_SdkLib\ucrt\x64", "$SSPC_SdkLib\um\x64") -join ";"
$env:PATH = @($SSPC_LLVM, $SSPC_CMake, $SSPC_Ninja, $SSPC_SdkBin, "$SSPC_MSVC\bin\Hostx64\x64", $env:PATH) -join ";"
# Keep compiler/CMake temp files on E:
$env:TEMP = "$SSPC_Root\.tmp"; $env:TMP = "$SSPC_Root\.tmp"
New-Item -ItemType Directory -Force $env:TEMP | Out-Null
