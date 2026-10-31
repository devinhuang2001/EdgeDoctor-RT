$ErrorActionPreference = 'Stop'
$serverRoot = Resolve-Path (Join-Path $PSScriptRoot '..\server')
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw 'docker CLI not found' }
docker info *> $null
if ($LASTEXITCODE -ne 0) { throw 'Docker daemon is not reachable' }
$passwd = Join-Path $serverRoot 'passwd'
if (-not (Test-Path $passwd -PathType Leaf)) { throw "Missing $passwd; run ..\scripts\mqtt-provision.ps1 -User edgedoctor first" }
Push-Location $serverRoot
try {
    if (-not $env:EDGEDOCTOR_API_TOKEN) { Write-Warning 'EDGEDOCTOR_API_TOKEN is unset; API mutations are unauthenticated' }
    docker compose up -d --build
    if ($LASTEXITCODE -ne 0) { throw 'docker compose failed' }
    Invoke-WebRequest -UseBasicParsing http://127.0.0.1:8080/health/ready | Out-Null
    Write-Host 'EdgeDoctor service is ready at http://127.0.0.1:8080/dashboard/'
} finally { Pop-Location }
