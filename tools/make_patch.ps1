# Generates the next bridge patch (patches\shadps4\NNNN-<name>.patch) from local changes of
# third_party\shadPS4: the diff of the given files between "HEAD + every existing patch" (built in a
# temporary git index, as cmake\shadps4_bootstrap.cmake checks the chain) and the working tree. Then
# checks the chain: HEAD + all patches (the new one included) must give exactly the working tree's src\.
#   powershell -ExecutionPolicy Bypass -File E:\SecondSonPC\tools\make_patch.ps1 -Name external-exception-dispatch `
#       -Files src/core/signals.h,src/core/signals.cpp
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string[]]$Files
)
$ErrorActionPreference = "Stop"
$Files = @($Files | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = "E:\SecondSonPC"
$shad = "$root\third_party\shadPS4"
$patchDir = "$root\patches\shadps4"
$tmp = "$root\.tmp"
New-Item -ItemType Directory -Force $tmp | Out-Null

$existing = @(Get-ChildItem $patchDir -Filter *.patch | Sort-Object Name)
$next = 1 + [int](($existing | Select-Object -Last 1).Name.Substring(0, 4))
$out = Join-Path $patchDir ("{0:D4}-{1}.patch" -f $next, $Name)

$index = Join-Path $tmp "make_patch.index"
Remove-Item $index -ErrorAction SilentlyContinue
$env:GIT_INDEX_FILE = $index
try {
    git -C $shad read-tree HEAD
    if ($LASTEXITCODE -ne 0) { throw "git read-tree failed" }
    foreach ($p in $existing) {
        git -C $shad apply --cached $p.FullName
        if ($LASTEXITCODE -ne 0) { throw "existing patch $($p.Name) does not apply" }
    }
    foreach ($f in $Files) {
        # a new file: make it known to the index so that git diff shows it whole
        if (-not (git -C $shad ls-files --cached -- $f)) { git -C $shad add -N -- $f }
    }
    $diff = git -C $shad diff --no-color -- @Files
    if ($LASTEXITCODE -ne 0) { throw "git diff failed" }
} finally {
    Remove-Item Env:\GIT_INDEX_FILE
}
if (-not $diff) { throw "no changes in $($Files -join ', ')" }
# LF line endings, as the existing patches
[System.IO.File]::WriteAllText($out, (($diff -join "`n") + "`n"))

# Chain check: HEAD + every patch == working tree src\
$env:GIT_INDEX_FILE = $index
try {
    git -C $shad read-tree HEAD
    foreach ($p in @(Get-ChildItem $patchDir -Filter *.patch | Sort-Object Name)) {
        git -C $shad apply --cached $p.FullName
        if ($LASTEXITCODE -ne 0) { throw "patch $($p.Name) does not apply in the chain" }
    }
    $expected = git -C $shad write-tree --prefix=src/
    git -C $shad read-tree HEAD
    git -C $shad add -A -- src
    $checkout = git -C $shad write-tree --prefix=src/
} finally {
    Remove-Item Env:\GIT_INDEX_FILE
    Remove-Item $index -ErrorAction SilentlyContinue
}
if ($expected -ne $checkout) {
    throw "chain mismatch: HEAD + patches $expected, working tree $checkout (other local changes?)"
}
Write-Output "written $out; chain HEAD + $($existing.Count + 1) patches == working tree ($expected)"
