# 主机回归：数据管线、C 公共契约与局域网服务测试。
# 前端构建、前端单测与浏览器验收由 scripts/test-ui.ps1 负责，其中也包含本脚本。
$ErrorActionPreference = 'Stop'

& (Join-Path $PSScriptRoot 'test-data.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& (Join-Path $PSScriptRoot 'test-common-c.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$env:PYTHONPATH = $projectRoot
& python -m pytest (Join-Path $projectRoot 'tests\server') -q
exit $LASTEXITCODE
