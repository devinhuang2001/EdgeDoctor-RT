param([Parameter(Mandatory=$true)][string]$User)
$ErrorActionPreference = 'Stop'
$serverRoot = Resolve-Path (Join-Path $PSScriptRoot '..\server')
$config = Join-Path $serverRoot 'mosquitto.conf'
$passwd = Join-Path $serverRoot 'passwd'
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { throw 'docker CLI not found' }
if (-not (Test-Path $passwd)) { New-Item -ItemType File -Path $passwd | Out-Null }
$password = Read-Host "Password for MQTT user $User" -AsSecureString
$ptr=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($password)
try { $plain=[Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
docker run --rm -i -v "${serverRoot}:/mosquitto/config" eclipse-mosquitto:2.0.20 mosquitto_passwd -b /mosquitto/config/passwd $User $plain
if ($LASTEXITCODE -ne 0) { throw 'mosquitto_passwd failed' }
$lines=Get-Content $config
if (-not ($lines -match '^password_file ')) { Add-Content -Path $config -Value "password_file /mosquitto/config/passwd" }
Write-Host "Provisioned MQTT user '$User'. Credentials remain local in server/passwd."
