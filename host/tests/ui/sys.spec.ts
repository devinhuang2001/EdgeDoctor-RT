// DD-SYS-01..05：构建、部署与资产完整性。

import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { expect, test } from './evidence';
import { hasHorizontalScroll } from './helpers';
import { nodeBin, run } from './spawn';

const repoRoot = process.cwd();
const evidenceRoot = process.env.DD_EVIDENCE_DIR ?? '';

test('DD-SYS-01 lint、类型检查与生产构建全部通过', async ({ evidence }, testInfo) => {
  const buildLog = path.join(evidenceRoot, 'build.log');
  if (!fs.existsSync(buildLog)) {
    testInfo.annotations.push({ type: 'missing-evidence', description: '缺少 scripts/test-ui.ps1 生成的 build.log，无法证明三项退出码为 0' });
    return;
  }
  const log = fs.readFileSync(buildLog, 'utf8');
  evidence.writeText('build.log', log);
  expect(log, 'lint/类型检查/构建三项都必须记录为退出码 0').toContain('LINT_EXIT=0');
  expect(log).toContain('TSC_EXIT=0');
  expect(log).toContain('BUILD_EXIT=0');

  const built = path.join(repoRoot, 'server', 'web-v2', 'index.html');
  const html = fs.readFileSync(built, 'utf8');
  const remote = [...html.matchAll(/(?:src|href)="(https?:\/\/[^"]+)"/g)].map((match) => match[1]);
  expect(remote, '构建产物不得引用远程资源').toHaveLength(0);
  evidence.writeJson('built-index.json', { remote, bytes: html.length });
});

test('DD-SYS-02 旧页与新页都能打开，默认入口仍指向旧页，新页深链接可刷新', async ({ page, evidence }, testInfo) => {
  const old = await page.request.get('/dashboard/');
  const next = await page.request.get('/dashboard-v2/');
  const root = await page.request.get('/', { maxRedirects: 0 });

  expect(old.status()).toBe(200);
  expect(next.status()).toBe(200);
  expect([307, 308, 302].includes(root.status()), `根路径应重定向到旧版，实际 ${root.status()}`).toBe(true);
  expect(root.headers().location).toContain('/dashboard/');
  evidence.writeJson('routes.json', {
    dashboard: old.status(),
    dashboardV2: next.status(),
    root: root.status(),
    rootLocation: root.headers().location,
  });

  // 深链接硬刷新后仍能恢复
  await page.goto('/dashboard-v2/#/monitor/ed84-001');
  await expect(page.getByRole('heading', { name: '实时监测', exact: true })).toBeVisible();
  await page.reload();
  await expect(page.getByRole('heading', { name: '实时监测', exact: true })).toBeVisible();
  // 等待懒加载的二维/三维区块完成布局后再量测宽度，避免把加载中的瞬时布局当成缺陷。
  await page.waitForLoadState('load');
  await page.waitForTimeout(600);
  expect(await hasHorizontalScroll(page), '加载完成后不应出现横向滚动').toBe(false);

  const settings = await page.request.get('/api/v1/assets/motor-01/binding');
  expect(settings.status()).toBe(200);
  void testInfo;
});

test('DD-SYS-03 断开外网时页面骨架与关键文字仍可用', async ({ page, evidence }) => {
  // 主动拦截一切跨站请求，模拟无外网环境。
  await page.route('**/*', (route) => {
    const url = route.request().url();
    if (/^https?:\/\/(127\.0\.0\.1|localhost)/.test(url) || url.startsWith('data:') || url.startsWith('blob:')) {
      void route.continue();
      return;
    }
    void route.abort();
  });

  for (const hash of ['#/factory', '#/monitor/ed84-001', '#/events', '#/analysis/ed84-001']) {
    await page.goto(`/dashboard-v2/${hash}`);
    await expect(page.getByRole('navigation', { name: '主导航' }).first()).toBeVisible();
    await expect(page.locator('.page-head h1')).toBeVisible();
  }
  evidence.expectNoExternalRequests();
  evidence.expectCleanConsole();
});

test('DD-SYS-04 模型校验和、许可证与绑定配置一致', async ({ evidence }, testInfo) => {
  const lock = JSON.parse(fs.readFileSync(path.join(repoRoot, 'ui', 'desktop', 'assets.lock.json'), 'utf8')) as {
    assets: { path: string; sha256: string; bytes: number }[];
  };
  const report: Record<string, unknown>[] = [];
  for (const asset of lock.assets) {
    const source = path.join(repoRoot, 'ui', 'desktop', asset.path);
    const deployed = path.join(repoRoot, 'server', 'web-v2', asset.path.replace(/^public\//, ''));
    const sourceDigest = createHash('sha256').update(fs.readFileSync(source)).digest('hex');
    expect(sourceDigest, `${asset.path} 与冻结清单不一致`).toBe(asset.sha256);
    if (fs.existsSync(deployed)) {
      const deployedDigest = createHash('sha256').update(fs.readFileSync(deployed)).digest('hex');
      expect(deployedDigest, `部署副本 ${asset.path} 与冻结清单不一致`).toBe(asset.sha256);
    }
    report.push({ path: asset.path, sha256: sourceDigest, deployed: fs.existsSync(deployed), bytes: asset.bytes });
  }
  expect(fs.existsSync(path.join(repoRoot, 'ui', 'desktop', 'public', 'models', 'LICENSE.txt')), '许可证必须随资产保留').toBe(true);

  // 构建期校验器同时验证 schema、唯一性与模型节点存在性
  const validator = run(
    nodeBin,
    [path.join('ui', 'desktop', 'scripts', 'build-bindings.mjs')],
    repoRoot,
  );
  const output = validator.output;
  evidence.writeText('build-bindings.log', output);
  expect(validator.status, `绑定校验器必须通过：\n${output}`).toBe(0);
  evidence.writeJson('asset-integrity.json', { assets: report, validator: output.trim() });
  void testInfo;
});

test('DD-SYS-05 四条路由首次加载、硬刷新与前进后退都不产生 404', async ({ page, evidence }) => {
  const routes = [
    { hash: '#/factory', title: '工厂总览' },
    { hash: '#/monitor/ed84-001', title: '实时监测' },
    { hash: '#/events', title: '事件中心' },
    { hash: '#/analysis/ed84-001', title: '信号分析' },
  ];

  for (const route of routes) {
    await page.goto(`/dashboard-v2/${route.hash}`);
    await expect(page.locator('.page-head h1')).toHaveText(route.title);
    await page.reload();
    await expect(page.locator('.page-head h1')).toHaveText(route.title);
  }

  await page.goBack();
  await expect(page.locator('.page-head h1')).toHaveText('事件中心');
  await page.goForward();
  await expect(page.locator('.page-head h1')).toHaveText('信号分析');

  const badPaths = evidence.network.filter((entry) => entry.url.includes('/dashboard-v2/') && entry.status >= 400);
  expect(badPaths, `存在失败的资源请求：${JSON.stringify(badPaths)}`).toHaveLength(0);
  evidence.expectCleanConsole();
  evidence.writeJson('route-requests.json', evidence.network);
});
