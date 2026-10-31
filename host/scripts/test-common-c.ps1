$ErrorActionPreference = 'Stop'

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$buildDirectory = Join-Path $projectRoot 'build\host'
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
$output = Join-Path $buildDirectory 'edgedoctor_common_test.exe'
$include = Join-Path $projectRoot 'firmware\common\include'
$sources = @(
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_edgepack.c'),
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_backend.c'),
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_csv.c'),
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_sync.c'),
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_profiler.c'),
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_ipc.c'),
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_window.c'),
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_snapshot.c'),
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_onboard_model.c'),
    (Join-Path $projectRoot 'firmware\common\src\edgedoctor_feature_contract.c'),
    (Join-Path $projectRoot 'tests\c\test_common.c')
)

& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" @sources -lm -o $output
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
& $output
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$monitorOutput = Join-Path $buildDirectory 'ed_monitor_test.exe'
$monitorSources = @(
    (Join-Path $projectRoot 'firmware\common\src\ed_monitor_types.c'),
    (Join-Path $projectRoot 'firmware\common\src\ed_quality.c'),
    (Join-Path $projectRoot 'firmware\common\src\ed_alarm.c'),
    (Join-Path $projectRoot 'tests\c\test_monitor_core.c')
)
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" @monitorSources -lm -o $monitorOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $monitorOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$featureOutput = Join-Path $buildDirectory 'ed_lf_features_test.exe'
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" (Join-Path $projectRoot 'firmware\common\src\ed_lf_features.c') (Join-Path $projectRoot 'tests\c\test_lf_features.c') -lm -o $featureOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $featureOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$windowOutput = Join-Path $buildDirectory 'ed_lf_window_test.exe'
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" (Join-Path $projectRoot 'firmware\common\src\ed_lf_window.c') (Join-Path $projectRoot 'firmware\common\src\ed_lf_features.c') (Join-Path $projectRoot 'tests\c\test_lf_window.c') -lm -o $windowOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $windowOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$baselineOutput = Join-Path $buildDirectory 'ed_baseline_test.exe'
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" (Join-Path $projectRoot 'firmware\common\src\ed_baseline.c') (Join-Path $projectRoot 'tests\c\test_baseline.c') -lm -o $baselineOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $baselineOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$imuProfileOutput = Join-Path $buildDirectory 'ed_imu_profile_test.exe'
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" (Join-Path $projectRoot 'firmware\common\src\ed_imu_profile.c') (Join-Path $projectRoot 'tests\c\test_imu_profile.c') -lm -o $imuProfileOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $imuProfileOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$mqttOutput = Join-Path $buildDirectory 'ed_mqtt_contract_test.exe'
$mqttInclude = Join-Path $projectRoot 'firmware\m55\protocol'
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" "-I$mqttInclude" (Join-Path $projectRoot 'firmware\m55\protocol\ed_mqtt_client.c') (Join-Path $projectRoot 'tests\c\test_mqtt_contract.c') -lm -o $mqttOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $mqttOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$voiceOutput = Join-Path $buildDirectory 'ed_voice_guidance_test.exe'
$runtimeOutput = Join-Path $buildDirectory 'ed_monitor_runtime_test.exe'
$runtimeIncludes = @('tests\c\rt_stubs', 'firmware\common\include', 'firmware\m55\runtime', 'firmware\m55\logger', 'firmware\m55\sensor', 'firmware\m55\protocol') | ForEach-Object { '-I' + (Join-Path $projectRoot $_) }
$runtimeSources = @('ed_alarm','ed_quality','ed_baseline','ed_lf_features','ed_lf_window','ed_imu_profile','ed_monitor_types') | ForEach-Object { Join-Path $projectRoot "firmware\common\src\$_.c" }
& gcc -std=c11 -Wall -Wextra -Werror -O2 @runtimeIncludes (Join-Path $projectRoot 'tests\c\test_monitor_runtime.c') @runtimeSources -lm -o $runtimeOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $runtimeOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $runtimeOutput sensor
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$journalOutput = Join-Path $buildDirectory 'ed_journal_test.exe'
$rtStubs = Join-Path $projectRoot 'tests\c\rt_stubs'
& gcc -std=c11 -Wall -Wextra -Werror -O2 "-I$include" "-I$rtStubs" (Join-Path $projectRoot 'tests\c\test_journal_recovery.c') -o $journalOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Push-Location $buildDirectory
try {
    & $journalOutput
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & $journalOutput boot
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally { Pop-Location }
$retryOutput = Join-Path $buildDirectory 'ed_mqtt_retry_test.exe'
$serviceOutput = Join-Path $buildDirectory 'ed_mqtt_service_test.exe'
$captureOutput = Join-Path $buildDirectory 'ed_evidence_capture_test.exe'
& gcc -std=c11 -Wall -Wextra -Werror -O2 "-I$include" "-I$rtStubs" (Join-Path $projectRoot 'tests\c\test_evidence_capture.c') (Join-Path $projectRoot 'firmware\common\src\ed_evidence_format.c') -o $captureOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $captureOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" "-I$mqttInclude" (Join-Path $projectRoot 'firmware\m55\protocol\ed_mqtt_client.c') (Join-Path $projectRoot 'tests\c\test_mqtt_service.c') -o $serviceOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $serviceOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$connectOutput = Join-Path $buildDirectory 'ed_mqtt_connect_test.exe'
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" "-I$mqttInclude" (Join-Path $projectRoot 'firmware\m55\protocol\ed_mqtt_client.c') (Join-Path $projectRoot 'tests\c\test_mqtt_connect.c') -o $connectOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $connectOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" "-I$mqttInclude" (Join-Path $projectRoot 'firmware\m55\protocol\ed_mqtt_client.c') (Join-Path $projectRoot 'tests\c\test_mqtt_retry.c') -o $retryOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $retryOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2 "-I$include" (Join-Path $projectRoot 'firmware\common\src\ed_voice_guidance.c') (Join-Path $projectRoot 'tests\c\test_voice_guidance.c') -o $voiceOutput
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $voiceOutput
exit $LASTEXITCODE
