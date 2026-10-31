param(
    [string]$ProjectPath = 'D:\CODEX SPACE\infei\edgi-workspace\Edgi_Talk_M55_EdgeDoctor',
    [switch]$Clean,
    [ValidateRange(1, 32)]
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$resolvedBsp = Resolve-Path $ProjectPath -ErrorAction SilentlyContinue
if (-not $resolvedBsp -or -not (Test-Path (Join-Path $resolvedBsp 'SConstruct'))) {
    throw "No EdgeDoctor BSP SConstruct found at '$ProjectPath'."
}

$toolchain = Get-ChildItem (Join-Path $projectRoot '..\..\toolchains') `
    -Recurse -Filter 'arm-none-eabi-gcc.exe' -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $toolchain) {
    throw 'Arm GNU toolchain was not found under the sibling toolchains directory.'
}

$toolchainBinLong = Split-Path $toolchain.FullName -Parent
$toolchainRootLong = Split-Path $toolchainBinLong -Parent
# SCons' Windows command launcher fails to preserve GCC's multilib discovery
# when the executable prefix contains spaces. Use the filesystem's short path
# for the compiler only; source paths and vendor files remain unchanged.
$toolchainRoot = (& cmd.exe /d /c "for %I in (`"$toolchainRootLong`") do @echo %~sI").Trim()
if (-not $toolchainRoot) {
    throw "Unable to resolve a space-free toolchain path for '$toolchainRootLong'."
}
$toolchainBin = Join-Path $toolchainRoot 'bin'
$cxxBase = Join-Path $toolchainRoot 'arm-none-eabi\include\c++\13.3.1\arm-none-eabi'
$multilib = & (Join-Path $toolchainBin 'arm-none-eabi-g++.exe') `
    -mcpu=cortex-m55 -mthumb -mfpu=auto -mfloat-abi=hard -print-multi-directory
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to resolve the Cortex-M55 C++ multilib directory.'
}
$cxxInclude = Join-Path $cxxBase $multilib.Trim()
if (-not (Test-Path (Join-Path $cxxInclude 'bits\c++config.h'))) {
    throw "Cortex-M55 C++ configuration header not found at '$cxxInclude'."
}

# SCons enters through the BSP's application copy even when APP_DIR points at
# this checkout. Keep that entry current so it cannot load stale dependencies.
$bspApplication = Join-Path $resolvedBsp 'applications\edgedoctor'
if (-not (Test-Path $bspApplication)) {
    throw "No EdgeDoctor application integration found at '$bspApplication'."
}
$sourceEntry = Join-Path $projectRoot 'firmware\m55\SConscript'
$bspEntry = Join-Path $bspApplication 'SConscript'
if ((Get-FileHash $sourceEntry).Hash -ne (Get-FileHash $bspEntry).Hash) {
    $backupDirectory = Join-Path $resolvedBsp 'build\edgedoctor-entry-backup'
    New-Item -ItemType Directory -Force -Path $backupDirectory | Out-Null
    $backupEntry = Join-Path $backupDirectory 'SConscript'
    if (-not (Test-Path $backupEntry)) { Copy-Item -LiteralPath $bspEntry -Destination $backupEntry }
    Copy-Item -LiteralPath $sourceEntry -Destination $bspEntry -Force
}

$previousApp = $env:EDGEDOCTOR_APP_DIR
$previousCxx = $env:EDGEDOCTOR_CXX_INCLUDE
$previousExec = $env:RTT_EXEC_PATH
$env:EDGEDOCTOR_APP_DIR = Join-Path $projectRoot 'firmware\m55'
$env:EDGEDOCTOR_CXX_INCLUDE = $cxxInclude
$env:RTT_EXEC_PATH = $toolchainBin

Push-Location $resolvedBsp
try {
    if ($Clean) {
        scons -c
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    scons -j $Jobs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
    $env:EDGEDOCTOR_APP_DIR = $previousApp
    $env:EDGEDOCTOR_CXX_INCLUDE = $previousCxx
    $env:RTT_EXEC_PATH = $previousExec
}
