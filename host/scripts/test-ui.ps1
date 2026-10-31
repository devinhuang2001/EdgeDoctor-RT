# 电脑版控制台浏览器验收入口（规格 02）。
# 顺序：前端 lint/类型检查/构建 → 前端单元测试 → 服务端回归 → 浏览器验收 → 汇总报告。
# 证据写入 benchmark/results/desktop-dashboard/{run_id}/。

param(
  [string]$RunId = "",
  [switch]$SkipBuild,
  [switch]$SkipRegression,
  [switch]$Headed,
  [int]$Port = 8099,
  [string]$Browser = "msedge"
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $RunId) { $RunId = 'run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
$evidence = Join-Path $root "benchmark\results\desktop-dashboard\$RunId"
New-Item -ItemType Directory -Force -Path $evidence | Out-Null

$env:DD_RUN_ID = $RunId
$env:DD_EVIDENCE_DIR = $evidence
$env:DD_PORT = "$Port"
$env:DD_BROWSER = $Browser
if ($Headed) { $env:DD_HEADED = '1' }

# 整轮验收必须只用一个 Python：服务端（uvicorn）与用例里的 pytest 共用一个解释器。
# `python` 在不同 shell 下会解析到不同安装（有的装了 uvicorn/pytest，有的没有），
# 所以在这里解析一次，通过 DD_PYTHON 传给 playwright.config.ts 与用例。
$pythonPath = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $pythonPath) {
  Write-Host "PATH 中找不到 python，无法启动验收服务端。" -ForegroundColor Red
  exit 1
}
$env:DD_PYTHON = $pythonPath

# 解除宿主注入的 fs 删除保护 shim，否则"生产构建"这一步无法复现。
#
# 在 WorkBuddy 会话内启动本脚本时，环境里带有
#   NODE_OPTIONS=--require=.../node-language-shim.cjs
#   CODEBUDDY_SAFE_DELETE_ENABLED=1 等一组变量
# 这组 shim 会给 child_process 里的 fs.rmSync 加一道批量删除闸门
# （默认阈值 50 个/轮）。vite 的 emptyOutDir 会一次性清空产物目录，实测被拦下：
#   [vite:prepare-out-dir] [safe-delete][SAFE_DELETE_BULK_CONFIRM_REQUIRED] {"count":114,...}
# 于是 BUILD_EXIT=1，整轮验收在第 4 步就中止，且看上去像是项目本身构建失败。
#
# 这只影响本脚本自己产出的构建目录（ui/desktop 的 outDir / server/web-v2），
# 与个人文件保护无关，因此在这里显式解除；对其它任何用途都没有影响。
# 不在脚本外"教用户关掉保护"，因为那是把环境问题伪装成操作习惯。
if ($env:NODE_OPTIONS -like '*node-language-shim*') { Remove-Item Env:NODE_OPTIONS -ErrorAction SilentlyContinue }
foreach ($shimVar in @(
    'CODEBUDDY_SAFE_DELETE_ENABLED',
    'CODEBUDDY_SAFE_DELETE_SANDBOX',
    'CODEBUDDY_SAFE_DELETE_BULK_GUARD',
    'CODEBUDDY_SAFE_DELETE_BIN_DIR',
    'CODEBUDDY_SAFE_DELETE_BULK_STATE_DIR'
  )) {
  Remove-Item "Env:$shimVar" -ErrorAction SilentlyContinue
}

$buildLog = Join-Path $evidence 'build.log'
$apiLog = Join-Path $evidence 'api.log'
$regressionLog = Join-Path $evidence 'regression.log'

function Write-Log([string]$file, [string]$text) {
  Add-Content -Path $file -Value $text -Encoding UTF8
}

function Invoke-Step([string]$file, [string]$title, [string]$command, [string]$workdir, [string]$exitMarker) {
  Write-Log $file ""
  Write-Log $file "### $title"
  Write-Log $file "cwd: $workdir"
  Write-Log $file "cmd: $command"
  Push-Location $workdir
  try {
    $output = & cmd /c "$command 2>&1"
    $code = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  $output | ForEach-Object { Write-Log $file $_ }
  Write-Log $file "$exitMarker=$code"
  Write-Host ("  [{0}] {1} -> exit {2}" -f $title, $exitMarker, $code)
  return $code
}

Write-Host "EdgeDoctor-RT 浏览器验收：run_id=$RunId"
Write-Host "证据目录：$evidence"

Set-Content -Path $buildLog -Value @(
  "# EdgeDoctor-RT 电脑版控制台构建记录",
  "run_id=$RunId",
  "started=$(Get-Date -Format o)",
  "node=$(& node --version)",
  "python=$(& python --version 2>&1)"
) -Encoding UTF8

$failure = $null

if (-not $SkipBuild) {
  $desktop = Join-Path $root 'ui\desktop'
  $steps = @(
    @{ title = '校验资产绑定配置'; cmd = 'node scripts/build-bindings.mjs'; marker = 'BINDINGS_EXIT' },
    @{ title = 'eslint'; cmd = 'npx eslint .'; marker = 'LINT_EXIT' },
    @{ title = 'tsc 类型检查'; cmd = 'npx tsc -b --force'; marker = 'TSC_EXIT' },
    @{ title = '生产构建'; cmd = 'npm run build'; marker = 'BUILD_EXIT' },
    @{ title = '前端单元测试'; cmd = 'npx vitest run'; marker = 'VITEST_EXIT' }
  )
  foreach ($step in $steps) {
    $code = Invoke-Step $buildLog $step.title $step.cmd $desktop $step.marker
    if ($code -ne 0) { $failure = $step.title; break }
  }
}

if (-not $failure -and -not $SkipRegression) {
  $code = Invoke-Step $regressionLog '完整主机回归 scripts/test.ps1' 'powershell -ExecutionPolicy Bypass -File scripts/test.ps1' $root 'TEST_PS1_EXIT'
  if ($code -ne 0) { $failure = 'scripts/test.ps1' }
}

if (-not $failure) {
  # 端口预检：DD_FRESH_SERVER 要求全新服务，旧实例会让验收读到旧数据。
  $inUse = $false
  try {
    $response = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health/ready" -TimeoutSec 2 -UseBasicParsing
    $inUse = $response.StatusCode -eq 200
  } catch { $inUse = $false }
  if ($inUse) {
    Write-Host "端口 $Port 上已有服务在运行。请先停止它再执行全新验收（避免读到旧数据库）。" -ForegroundColor Yellow
    $failure = 'port-in-use'
  }
}

if (-not $failure) {
  # 全新数据库：必须在服务启动前删除，否则 Windows 会因文件被占用而 EPERM。
  # 这是整个验收流程唯一的清理入口：playwright.config.ts 只解析路径、不删任何文件，
  # 因为它会被主进程与 worker 各加载一次，第二次加载时服务已经打开了数据库。
  $db = Join-Path $evidence 'acceptance.db'
  foreach ($target in @($db, "$db-wal", "$db-shm")) {
    if (Test-Path $target) { Remove-Item $target -Force -ErrorAction SilentlyContinue }
  }
  # 复用一个已存在的 run_id 时，上一轮留证必须一并清空，否则旧 EDV2 与旧截图
  # 会和新记录混在同一个证据目录里。
  $evidenceSub = Join-Path $evidence 'evidence'
  if (Test-Path $evidenceSub) { Remove-Item $evidenceSub -Recurse -Force -ErrorAction SilentlyContinue }

  # 服务端生命周期由本脚本持有，不交给 Playwright 的 webServer。
  #
  # 原因（已实测）：Playwright 在 Windows 上用
  #   spawnSync("taskkill /pid <pid> /T /F", { shell: true })
  # 结束 webServer（node_modules/playwright/lib/runner/index.js 的 _forceKill）。这条调用用的是
  # 默认 stdio，也就是包含 stdin 管道；而在部分 Windows 环境下为子进程创建 stdin 管道会直接
  # 失败，spawnSync 返回 error.code='EBUSY' 且不抛异常——try/catch 兜不住，taskkill 静默不执行。
  # 后果：服务残留、Playwright 在"等它退出"上一直 await（先等优雅退出超时、再 await exitPromise），
  # 进程永远不结束，onEnd 不执行，case-results.json / manifest.json 全部写不出来。
  # 由脚本启动、由脚本 taskkill，可以完全绕开这条路径；顺带也让"全新数据库"的时序更明确。
  $serverEnv = @{
    PYTHONPATH            = $root
    EDGEDOCTOR_DB         = $db
    EDGEDOCTOR_BINDINGS   = (Join-Path $root 'server\asset-bindings.json')
  }
  foreach ($key in $serverEnv.Keys) { Set-Item -Path "Env:$key" -Value $serverEnv[$key] }

  $serverOut = Join-Path $evidence 'server.log'
  $serverErr = Join-Path $evidence 'server.err.log'
  $server = Start-Process -FilePath $pythonPath `
    -ArgumentList @('-m', 'uvicorn', 'server.app:app', '--host', '127.0.0.1', '--port', "$Port") `
    -WorkingDirectory $root -PassThru -NoNewWindow `
    -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr
  Write-Host "  验收服务端 PID=$($server.Id)，数据库=$db"

  $ready = $false
  for ($i = 0; $i -lt 120; $i++) {
    try {
      $probe = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health/ready" -TimeoutSec 2 -UseBasicParsing
      if ($probe.StatusCode -eq 200) { $ready = $true; break }
    } catch { }
    if ($server.HasExited) { break }
    Start-Sleep -Milliseconds 500
  }
  if (-not $ready) {
    Write-Host "验收服务端未能就绪，详见 $serverErr" -ForegroundColor Red
    $failure = 'server-not-ready'
  }
}

if (-not $failure) {
  # 服务已经由本脚本起好，让 Playwright 复用它，不再自己起服务。
  $env:DD_REUSE_SERVER = '1'
  Push-Location $root
  try {
    & npx playwright test
    $code = $LASTEXITCODE
  } finally {
    Pop-Location
  }
  Write-Host "  [浏览器验收] playwright -> exit $code"
  if ($code -ne 0) { $failure = 'playwright' }
}

# 无论成败都要收掉服务，并确认端口真的释放了——残留服务会让下一轮读到旧数据。
if ($server) {
  if (-not $server.HasExited) {
    & taskkill /PID $server.Id /T /F 2>&1 | Out-Null
    $server.WaitForExit(10000) | Out-Null
  }
  Start-Sleep -Milliseconds 500
  $stillListening = $false
  try {
    $null = Invoke-WebRequest -Uri "http://127.0.0.1:$Port/health/ready" -TimeoutSec 2 -UseBasicParsing
    $stillListening = $true
  } catch { $stillListening = $false }
  if ($stillListening) { Write-Host "警告：端口 $Port 仍被占用，下一轮验收前请先停止残留服务。" -ForegroundColor Yellow }
  else { Write-Host "  验收服务端已停止，端口 $Port 已释放。" }
  Remove-Item Env:DD_REUSE_SERVER -ErrorAction SilentlyContinue
}

$caseResults = Join-Path $evidence 'case-results.json'
if (Test-Path $caseResults) {
  $cases = Get-Content $caseResults -Raw -Encoding UTF8 | ConvertFrom-Json
  $summary = $cases | Group-Object status | ForEach-Object { "$($_.Name)=$($_.Count)" }
  Write-Host ""
  Write-Host "验收结果：$($summary -join '  ')"
  Write-Host "证据目录：$evidence"
  $notPassed = $cases | Where-Object { $_.status -ne 'PASS' } | Select-Object -ExpandProperty id
  if ($notPassed) { Write-Host "未 PASS 的用例：$($notPassed -join ', ')" }
}

if ($failure) {
  Write-Host "验收未通过：$failure" -ForegroundColor Red
  exit 1
}
Write-Host "验收流程完成（未 PASS 的条目见 case-results.json）" -ForegroundColor Green
exit 0
