// 浏览器验收测试配置（规格 02 的 BROWSER / VIS / GPU 层）。
// 证据目录：benchmark/results/desktop-dashboard/{run_id}/，由 DD_EVIDENCE_DIR 指定。

import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from '@playwright/test';

const root = path.dirname(fileURLToPath(import.meta.url));

function stamp(): string {
  const now = new Date();
  const pad = (value: number) => String(value).padStart(2, '0');
  return `${now.getFullYear()}${pad(now.getMonth() + 1)}${pad(now.getDate())}-${pad(now.getHours())}${pad(now.getMinutes())}${pad(now.getSeconds())}`;
}

export const runId = process.env.DD_RUN_ID ?? `run-${stamp()}`;
process.env.DD_RUN_ID = runId;
export const evidenceDir = process.env.DD_EVIDENCE_DIR
  ?? path.join(root, 'benchmark', 'results', 'desktop-dashboard', runId);
process.env.DD_EVIDENCE_DIR = evidenceDir;

export const port = Number(process.env.DD_PORT ?? 8099);
export const baseURL = `http://127.0.0.1:${port}`;
export const browserChannel = process.env.DD_BROWSER ?? 'msedge';
process.env.DD_BASE_URL = baseURL;

// 整轮验收只允许用一个 Python：服务端与用例里的 pytest 必须是同一个解释器。
// `python` 在不同 shell 下会解析到不同安装，因此由 scripts/test-ui.ps1 解析后经
// DD_PYTHON 传入；直接跑 npx playwright test 时这里退回 `python`。
export const pythonBin = process.env.DD_PYTHON ?? 'python';

// 每次验收使用隔离且全新的 SQLite 库，保证同一 run 内可复现。
// 清理只在 scripts/test-ui.ps1 的服务启动前进行，那是唯一入口。
// 本配置会被主进程与 worker 各加载一次：如果在这里删除数据库，第二次加载发生在
// 服务已经打开该文件之后——Windows 下要么 EPERM，要么删掉服务正在使用的库，而后者
// 更坏：服务持有已 unlink 的 inode，新的 acceptance.db 会被重新创建，验收数据就落进
// 一个界面上看不见的库里。因此本文件只解析路径，绝不产生副作用。
const acceptanceDb = path.join(evidenceDir, 'acceptance.db');

export default defineConfig({
  testDir: path.join(root, 'tests', 'ui'),
  outputDir: path.join(evidenceDir, 'artifacts'),
  timeout: 150_000,
  expect: { timeout: 20_000 },
  fullyParallel: false,
  workers: 1,
  retries: 0,
  reporter: [
    ['list'],
    [path.join(root, 'tests', 'ui', 'acceptance-reporter.ts')],
  ],
  use: {
    baseURL,
    channel: browserChannel,
    headless: process.env.DD_HEADED !== '1',
    viewport: { width: 1920, height: 1080 },
    screenshot: 'only-on-failure',
    video: 'retain-on-failure',
    trace: 'retain-on-failure',
    locale: 'zh-CN',
    timezoneId: 'Asia/Shanghai',
    launchOptions: { args: process.env.DD_HEADED === '1' ? ['--ignore-gpu-blocklist'] : ['--ignore-gpu-blocklist', '--enable-unsafe-swiftshader'] },
  },
  webServer: {
    // 正常验收由 scripts/test-ui.ps1 先起好服务、再置 DD_REUSE_SERVER=1，下面的
    // command 只在直接跑 `npx playwright test` 时兜底。
    //
    // 注意：不要依赖 Playwright 自己收掉这个服务。它在 Windows 上用
    // spawnSync("taskkill ...", { shell: true }) 结束 webServer，而默认 stdio 含 stdin
    // 管道；这条管道在部分 Windows 环境下创建即失败（EBUSY）且 spawnSync 不抛异常，
    // 于是 taskkill 静默不执行，Playwright 会一直等一个不会退出的进程，onEnd 永不执行。
    // 见 scripts/test-ui.ps1 里服务端生命周期的处理。
    command: `"${pythonBin}" -m uvicorn server.app:app --host 127.0.0.1 --port ${port}`,
    cwd: root,
    url: `${baseURL}/health/ready`,
    // 默认必须自建服务。复用端口上未知的服务端意味着复用未知的数据库：
    // 旧服务持有的 EDGEDOCTOR_DB 可能已被清掉，写入会以
    // "attempt to write a readonly database" 的形式随机失败。
    // 只有显式 DD_REUSE_SERVER=1 才允许复用（scripts/test-ui.ps1 就是这么做的）。
    reuseExistingServer: process.env.DD_REUSE_SERVER === '1',
    timeout: 90_000,
    stdout: 'pipe',
    stderr: 'pipe',
    env: {
      PYTHONPATH: root,
      EDGEDOCTOR_DB: acceptanceDb,
      EDGEDOCTOR_BINDINGS: path.join(root, 'server', 'asset-bindings.json'),
    },
  },
});
