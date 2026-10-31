$ErrorActionPreference = 'Stop'

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$previousPythonPath = $env:PYTHONPATH
$env:PYTHONPATH = Join-Path $projectRoot 'tools'

Push-Location $projectRoot
try {
    python -m unittest discover -s tests/python -p 'test_*.py' -v
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
    $env:PYTHONPATH = $previousPythonPath
}

