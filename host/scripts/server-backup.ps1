param([ValidateSet('backup','restore')][string]$Action='backup',[string]$Archive)
$ErrorActionPreference = 'Stop'
$serverRoot = Resolve-Path (Join-Path $PSScriptRoot '..\server')
$data = Join-Path $serverRoot 'data'
if ($Action -eq 'backup') {
    if (-not $Archive) { $Archive = Join-Path $serverRoot ('backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.zip') }
    $db = Join-Path $data 'edgedoctor.db'
    if (Test-Path $db) { python -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); c.execute('PRAGMA wal_checkpoint(FULL)'); c.execute('PRAGMA integrity_check'); print(c.execute('PRAGMA integrity_check').fetchone()[0]); c.close()" $db }
    Compress-Archive -Path (Join-Path $data '*') -DestinationPath $Archive -Force
    Write-Host "Backup written to $Archive"
} else {
    if (-not $Archive -or -not (Test-Path $Archive)) { throw 'restore requires an existing -Archive' }
    New-Item -ItemType Directory -Force -Path $data | Out-Null
    Expand-Archive -Path $Archive -DestinationPath $data -Force
    Write-Host "Restore completed into $data"
}
