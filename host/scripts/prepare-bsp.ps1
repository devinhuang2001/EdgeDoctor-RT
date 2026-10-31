param(
    [Parameter(Mandatory = $true)]
    [string]$SdkPath,
    [Parameter(Mandatory = $true)]
    [string]$AppPath,
    [Parameter(Mandatory = $true)]
    [string]$OutputPath,
    [string]$IntegrationProjectPath,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$sdk = (Resolve-Path $SdkPath).Path
$app = (Resolve-Path $AppPath).Path
$output = [IO.Path]::GetFullPath($OutputPath)
$official = Join-Path $sdk 'projects\Edgi_Talk_M55_XiaoZhi'
$appM55 = Join-Path $app 'firmware\m55'
$appCommon = Join-Path $app 'firmware\common'

if (-not $IntegrationProjectPath) {
    $sdkParent = Split-Path $sdk -Parent
    $candidates = @(
        (Join-Path $sdk 'projects\Edgi_Talk_M55_EdgeDoctor'),
        (Join-Path $sdkParent 'Edgi_Talk_M55_EdgeDoctor'),
        (Join-Path $sdkParent 'projects\Edgi_Talk_M55_EdgeDoctor'),
        (Join-Path $sdkParent 'edgi-workspace\Edgi_Talk_M55_EdgeDoctor')
    )
    $IntegrationProjectPath = $candidates |
        Where-Object { Test-Path (Join-Path $_ 'applications\main.c') } |
        Select-Object -First 1
}
$integration = if ($IntegrationProjectPath) { (Resolve-Path $IntegrationProjectPath).Path } else { $null }

if (-not (Test-Path (Join-Path $official 'project.uvprojx')) -and
    -not (Test-Path (Join-Path $official 'SConstruct'))) {
    throw "Official Edgi_Talk_M55_XiaoZhi project was not found under $sdk"
}
if (-not (Test-Path $appM55) -or -not (Test-Path $appCommon)) {
    throw "AppPath must contain firmware\m55 and firmware\common"
}
if (-not $integration) {
    throw 'A patched EdgeDoctor BSP integration project was not found. Pass -IntegrationProjectPath explicitly.'
}
$integrationLibs = Join-Path $integration 'libs'
if (-not (Test-Path $integrationLibs)) {
    throw "Integration project is missing the board support libs directory: $integrationLibs"
}
if (Test-Path $output) {
    if (-not $Force) {
        throw "OutputPath already exists. Use -Force only for a directory you own: $output"
    }
    Remove-Item -LiteralPath $output -Recurse -Force
}

New-Item -ItemType Directory -Path $output -Force | Out-Null
Copy-Item -LiteralPath $official -Destination $output -Recurse
$outputProject = Join-Path $output 'Edgi_Talk_M55_XiaoZhi'

# Keep the generated project relocatable. The official SConstruct normally
# assumes that a project lives two levels below an RT-Thread checkout. A
# prepared output may live elsewhere, so let the build caller point it at the
# fixed v1.4.0 SDK without modifying the SDK source tree.
$generatedSconstruct = Join-Path $outputProject 'SConstruct'
$sconstructText = Get-Content -LiteralPath $generatedSconstruct -Raw
$sconstructText = $sconstructText.Replace(
    "if os.path.exists('rt-thread'):",
    "configured_rtt_root = os.environ.get('EDGEDOCTOR_RTT_ROOT')`nif configured_rtt_root:`n    RTT_ROOT = os.path.abspath(configured_rtt_root)`nelif os.path.exists('rt-thread'):")
$sconstructText = $sconstructText.Replace(
    "SDK_ROOT = os.path.abspath('./')",
    "SDK_ROOT = os.path.abspath(os.environ.get('EDGEDOCTOR_SDK_ROOT', './'))")
# The project SConstruct references SDK libraries relative to the checkout.
# Rewrite those references in the generated copy so an output directory can
# use the pinned SDK libraries through EDGEDOCTOR_LIBRARIES_ROOT.
$sconstructText = [regex]::Replace(
    $sconstructText,
    "'\.\./\.\./libraries/([^']+)'",
    'os.path.join(os.environ.get(''EDGEDOCTOR_LIBRARIES_ROOT'', ''../libraries''), ''$1'')')
$sconstructText = [regex]::Replace(
    $sconstructText,
    "'libraries/([^']+)'",
    'os.path.join(os.environ.get(''EDGEDOCTOR_LIBRARIES_ROOT'', ''libraries''), ''$1'')')
Set-Content -LiteralPath $generatedSconstruct -Value $sconstructText -Encoding UTF8

$outputApp = Join-Path $outputProject 'applications\edgedoctor'
$outputCommon = Join-Path $outputProject 'firmware\common'
$outputLibs = Join-Path $outputProject 'libs'
New-Item -ItemType Directory -Path $outputApp -Force | Out-Null
New-Item -ItemType Directory -Path $outputCommon -Force | Out-Null
Copy-Item -Path (Join-Path $appM55 '*') -Destination $outputApp -Recurse -Force
Copy-Item -Path (Join-Path $appCommon '*') -Destination $outputCommon -Recurse -Force
Copy-Item -LiteralPath $integrationLibs -Destination $outputLibs -Recurse -Force

$integrationFiles = @(
    'applications\xiaozhi\ui\xiaozhi_ui.h',
    'applications\xiaozhi\ui\SConscript',
    'applications\xiaozhi\SConscript',
    'applications\xiaozhi\xiaozhi.h'
)
foreach ($relative in $integrationFiles) {
    $source = Join-Path $integration $relative
    if (-not (Test-Path $source)) { throw "Missing integration file: $source" }
    $destination = Join-Path $outputProject $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

# Make the vendor-side integration self-contained in the generated copy.  The
# source BSP may still be a pristine checkout; these two small variant patches
# keep the official XiaoZhi project buildable after its 3D demo is excluded.
$xiaozhiSconscript = Join-Path $outputProject 'applications\xiaozhi\SConscript'
$xiaozhiSconscriptText = Get-Content -LiteralPath $xiaozhiSconscript -Raw
if ($xiaozhiSconscriptText -notmatch 'applications/edgedoctor/ui') {
    $xiaozhiSconscriptText = $xiaozhiSconscriptText.Replace(
        "path += [cwd + '/ui', cwd + '/webnet', cwd + '/iot', cwd + '/mcp']",
        "path += [cwd + '/ui', cwd + '/webnet', cwd + '/iot', cwd + '/mcp']`npath += [cwd + '/../../applications/edgedoctor/ui']")
}
if ($xiaozhiSconscriptText -notmatch 'CPPDEFINES') {
    $xiaozhiSconscriptText = $xiaozhiSconscriptText.Replace(
        "group = DefineGroup('xiaozhi', src, depend = [''], CPPPATH = path)",
        "group = DefineGroup('xiaozhi', src, depend = [''], CPPPATH = path, CPPDEFINES=['EDGEDOCTOR_RT'])")
}
Set-Content -LiteralPath $xiaozhiSconscript -Value $xiaozhiSconscriptText -Encoding UTF8

$xiaozhiHeader = Join-Path $outputProject 'applications\xiaozhi\xiaozhi.h'
$xiaozhiHeaderText = Get-Content -LiteralPath $xiaozhiHeader -Raw
$legacyEmojiInclude = '#include "lv_example_virtual3d_animated_emoji.h"'
if ($xiaozhiHeaderText -match [regex]::Escape($legacyEmojiInclude) -and
    $xiaozhiHeaderText -notmatch 'edgedoctor_emoji_compat.h') {
    $newline = [Environment]::NewLine
    $emojiCompatInclude = '#ifdef EDGEDOCTOR_RT' + $newline +
        '#include "edgedoctor_emoji_compat.h"' + $newline +
        '#else' + $newline + $legacyEmojiInclude + $newline + '#endif'
    $xiaozhiHeaderText = $xiaozhiHeaderText.Replace(
        $legacyEmojiInclude,
        $emojiCompatInclude)
}
Set-Content -LiteralPath $xiaozhiHeader -Value $xiaozhiHeaderText -Encoding UTF8

python (Join-Path $PSScriptRoot 'apply-edgedoctor-variant.py') $outputProject
if ($LASTEXITCODE -ne 0) { throw 'EdgeDoctor build variant patch failed' }

$manifest = [ordered]@{
    schema_version = 1
    generated_at = [DateTime]::UtcNow.ToString('o')
    sdk_path = $sdk
    official_project = 'projects/Edgi_Talk_M55_XiaoZhi'
    application_source = $app
    output_project = $outputProject
    application_mode = 'EDGEDOCTOR_HMI single LVGL owner'
    excluded_runtime = @('XiaoZhi wake_word', 'Edge Impulse model', 'TensorFlow Lite Micro', 'CMSIS-NN')
    variant_build_files = @('edge-impulse/SConscript', 'applications/xiaozhi/wake_word/SConscript')
    integration_source = $integration
    generated_paths = @('applications/edgedoctor', 'firmware/common', 'libs', 'applications/edgedoctor/ui/edgedoctor_ui_owner.c', 'applications/edgedoctor/ui/edgedoctor_emoji_compat.h', 'applications/edgedoctor/ui/edgedoctor_vendor_compat.c', 'applications/xiaozhi/ui/SConscript', 'applications/xiaozhi/ui/xiaozhi_ui.h', 'applications/xiaozhi/SConscript', 'applications/xiaozhi/xiaozhi.h', 'README_EdgeDoctor.md')
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outputProject 'edgedoctor-manifest.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $app 'docs\BSP-entry-EdgeDoctor.md') `
    -Destination (Join-Path $outputProject 'README_EdgeDoctor.md') -Force
Write-Output "Prepared regular-copy BSP at $outputProject"
