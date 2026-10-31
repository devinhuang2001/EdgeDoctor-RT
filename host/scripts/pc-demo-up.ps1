param(
    [int]$BrokerPort = 18883,
    [int]$ApiPort = 8080,
    [string]$DbPath = ''
)

$ErrorActionPreference = 'Stop'

$edgeRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$logRoot = Join-Path $edgeRoot 'tmp\pc-demo'
New-Item -ItemType Directory -Force $logRoot | Out-Null
if (-not $DbPath) { $DbPath = Join-Path $logRoot 'edgedoctor.db' }

$env:PYTHONPATH = $edgeRoot
$env:EDGEDOCTOR_DB = $DbPath
$env:EDGEDOCTOR_MQTT_TLS = '0'
$env:EDGEDOCTOR_MQTT_HOST = '127.0.0.1'
$env:EDGEDOCTOR_MQTT_PORT = "$BrokerPort"
$env:EDGEDOCTOR_EVENTS_TOPIC = 'edgedoctor/+/+/#'
$env:EDGEDOCTOR_CLIENT_ID = 'edgedoctor-service-local'
# Must match the site the board was provisioned with (`edmqtt config <site>`),
# otherwise queued commands are published to a topic the board never subscribed.
$env:EDGEDOCTOR_SITE_ID = 'site_1'

$broker = $null
$api = $null
$brokerListener = Get-NetTCPConnection -State Listen -LocalPort $BrokerPort -ErrorAction SilentlyContinue
if (-not $brokerListener) {
    $broker = Start-Process python -ArgumentList 'tools/mqtt_dev_broker.py', '--host', '0.0.0.0', '--port', "$BrokerPort" -WorkingDirectory $edgeRoot -RedirectStandardOutput (Join-Path $logRoot 'broker.log') -RedirectStandardError (Join-Path $logRoot 'broker.err') -PassThru
}
$apiListener = Get-NetTCPConnection -State Listen -LocalPort $ApiPort -ErrorAction SilentlyContinue
if (-not $apiListener) {
    $api = Start-Process python -ArgumentList '-m', 'uvicorn', 'server.app:app', '--host', '0.0.0.0', '--port', "$ApiPort" -WorkingDirectory $edgeRoot -RedirectStandardOutput (Join-Path $logRoot 'api.log') -RedirectStandardError (Join-Path $logRoot 'api.err') -PassThru
}
$ingest = $null
$ingestProcess = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match 'server\.mqtt_ingest' } | Select-Object -First 1
if (-not $ingestProcess) {
    $ingest = Start-Process python -ArgumentList '-m', 'server.mqtt_ingest' -WorkingDirectory $edgeRoot -RedirectStandardOutput (Join-Path $logRoot 'ingest.log') -RedirectStandardError (Join-Path $logRoot 'ingest.err') -PassThru
}

Start-Sleep -Seconds 2
Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:$ApiPort/health/ready" | Out-Null
Write-Host "EdgeDoctor PC stack ready: http://127.0.0.1:$ApiPort/dashboard/"
Write-Host "MQTT broker: 0.0.0.0:$BrokerPort (development plaintext)"
Write-Host "PIDs: broker=$($broker.Id) api=$($api.Id) ingest=$($ingest.Id) (existing services are reused)"
