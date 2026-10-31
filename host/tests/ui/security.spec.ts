// DD-SEC-01..03、DD-A11Y-01、DD-PERF-01：安全、可访问性与性能证据。

import fs from 'node:fs';
import path from 'node:path';
import AxeBuilder from '@axe-core/playwright';
import { expect, seed, test } from './evidence';
import { seedEvent, seedTelemetry, uniqueId } from './helpers';

const repoRoot = process.cwd();

test('DD-SEC-01 事件名称、ID、备注与 API 错误中的 HTML 一律作为文本显示', async ({ page, evidence }) => {
  const marker = `sec01${Date.now()}`;
  const payload = `<script>window.${marker}=1</script><img src=x onerror="window.${marker}=1">`;
  const eventId = `evt-${payload}`.slice(0, 90);
  await seedEvent(page, { eventId });

  await page.goto('/dashboard-v2/#/events');
  await page.getByRole('button', { name: new RegExp('evt-') }).first().click();
  await expect(page.locator('.event-detail, .events-panel').first()).toBeVisible();

  const rendered = await page.locator('body').innerText();
  const executed = await page.evaluate((name) => Boolean((window as unknown as Record<string, unknown>)[name]), marker);
  const scriptCount = await page.locator('script').count();

  // 错误信息注入：让服务端返回带 HTML 的 detail
  await page.route('**/api/v1/factory/summary', (route) => route.fulfill({
    status: 500,
    contentType: 'application/json',
    body: JSON.stringify({ detail: `<img src=x onerror="window.${marker}=1">` }),
  }));
  await page.getByRole('button', { name: '刷新' }).first().click();
  await expect(page.locator('.notice.error, .state-view.error').first()).toBeVisible({ timeout: 20_000 });
  const errorText = await page.locator('.notice.error, .state-view.error').first().innerText();
  expect(errorText).toContain('<img src=x');

  evidence.writeJson('xss.json', { marker, executed, scriptCount, errorText, renderedHasPayload: rendered.includes('<script>') });
  expect(executed, '注入的脚本不得执行').toBe(false);
  expect(scriptCount, '页面脚本数量应保持为构建产物注入的固定数量').toBeLessThan(6);

  // 源码不得存在未转义的注入路径
  const offenders: string[] = [];
  const walk = (dir: string) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      if (entry.name === 'node_modules') continue;
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) { walk(full); continue; }
      if (!/\.(ts|tsx|js|jsx)$/.test(entry.name)) continue;
      const text = fs.readFileSync(full, 'utf8');
      if (/dangerouslySetInnerHTML|\.innerHTML\s*=|document\.write/.test(text)) offenders.push(path.relative(repoRoot, full));
    }
  };
  walk(path.join(repoRoot, 'ui', 'desktop', 'src'));
  expect(offenders, `存在未转义的注入路径：${offenders.join(' | ')}`).toHaveLength(0);
});

test('DD-SEC-02 源码、产物、URL 与浏览器存储中不含凭据', async ({ page, evidence }) => {
  await page.goto('/dashboard-v2/#/factory');
  await page.waitForTimeout(1200);

  const storage = await page.evaluate(() => ({
    local: { ...window.localStorage },
    session: { ...window.sessionStorage },
    cookie: document.cookie,
  }));
  expect(Object.keys(storage.local)).toHaveLength(0);
  expect(Object.keys(storage.session)).toHaveLength(0);
  expect(storage.cookie).toBe('');

  const url = page.url();
  expect(url, 'URL 中不得出现令牌').not.toMatch(/token|password|secret/i);

  const patterns = [
    /EDGEDOCTOR_API_TOKEN\s*[:=]\s*['"][^'"]+['"]/,
    /mqtt[_-]?password\s*[:=]\s*['"][^'"]+['"]/i,
    /-----BEGIN [A-Z ]*PRIVATE KEY-----/,
  ];
  const offenders: string[] = [];
  const walk = (dir: string) => {
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
      if (entry.name === 'node_modules') continue;
      const full = path.join(dir, entry.name);
      if (entry.isDirectory()) { walk(full); continue; }
      if (!/\.(ts|tsx|js|jsx|css|html|json|map)$/.test(entry.name)) continue;
      const text = fs.readFileSync(full, 'utf8');
      for (const pattern of patterns) if (pattern.test(text)) offenders.push(`${path.relative(repoRoot, full)} :: ${pattern}`);
    }
  };
  walk(path.join(repoRoot, 'ui', 'desktop', 'src'));
  walk(path.join(repoRoot, 'server', 'web-v2'));

  evidence.writeJson('credentials-scan.json', { storage, url, offenders });
  expect(offenders, `构建产物或源码含凭据：${offenders.join(' | ')}`).toHaveLength(0);
});

test('DD-SEC-03 401/403/429/500、超时与错误 JSON 都有可理解的恢复路径', async ({ page, evidence }) => {
  const scenarios: { name: string; handler: (route: import('@playwright/test').Route) => Promise<void> }[] = [
    { name: '401', handler: async (route) => { await route.fulfill({ status: 401, contentType: 'application/json', body: JSON.stringify({ detail: 'authentication required' }) }); } },
    { name: '403', handler: async (route) => { await route.fulfill({ status: 403, contentType: 'application/json', body: JSON.stringify({ detail: 'forbidden' }) }); } },
    { name: '429', handler: async (route) => { await route.fulfill({ status: 429, contentType: 'application/json', body: JSON.stringify({ detail: 'too many requests' }) }); } },
    { name: '500', handler: async (route) => { await route.fulfill({ status: 500, contentType: 'application/json', body: JSON.stringify({ detail: 'boom' }) }); } },
    { name: 'bad-json', handler: async (route) => { await route.fulfill({ status: 200, contentType: 'application/json', body: 'not-json{' }); } },
  ];

  const report: Record<string, unknown>[] = [];
  for (const scenario of scenarios) {
    await page.route('**/api/v1/factory/summary', scenario.handler);
    await page.goto('/dashboard-v2/#/factory');
    const notice = page.locator('.notice, .state-view').first();
    await expect(notice).toBeVisible({ timeout: 20_000 });
    const text = await notice.innerText();
    const bodyText = await page.locator('body').innerText();
    expect(bodyText.length, `${scenario.name} 时页面不应白屏`).toBeGreaterThan(200);
    report.push({ scenario: scenario.name, notice: text });
    await page.unroute('**/api/v1/factory/summary');
  }

  // 超时：接口挂起超过客户端超时时间
  await page.route('**/api/v1/factory/summary', async () => { /* 永不响应 */ });
  await page.goto('/dashboard-v2/#/factory');
  await expect(page.locator('.notice, .state-view').first()).toBeVisible({ timeout: 25_000 });
  await page.unroute('**/api/v1/factory/summary');

  // 不无限重试：恢复后 6 秒内请求次数应有限
  let calls = 0;
  page.on('request', (request) => { if (request.url().includes('/api/v1/factory/summary')) calls += 1; });
  await page.route('**/api/v1/factory/summary', (route) => route.fulfill({ status: 500, contentType: 'application/json', body: JSON.stringify({ detail: 'boom' }) }));
  await page.waitForTimeout(6000);
  report.push({ retriesIn6s: calls });
  expect(calls, '不得无限重试').toBeLessThanOrEqual(4);

  evidence.writeJson('error-states.json', report);
  evidence.expectCleanConsole();
});

test('DD-A11Y-01 可访问性扫描无关键严重违规，状态由文字与图标共同表达', async ({ page, evidence }) => {
  await seedTelemetry(page, { rms: 0.02 });
  await seedEvent(page, { condition: 'ACTIVE', ack: 'UNACKNOWLEDGED', severity: 'WARNING' });

  const report: Record<string, unknown>[] = [];
  for (const hash of ['#/factory', '#/monitor/ed84-001', '#/events', '#/analysis/ed84-001']) {
    await page.goto(`/dashboard-v2/${hash}`);
    await page.waitForTimeout(800);
    const results = await new AxeBuilder({ page })
      .withTags(['wcag2a', 'wcag2aa'])
      .disableRules(['color-contrast'])
      .analyze();
    const serious = results.violations.filter((violation) => violation.impact === 'critical' || violation.impact === 'serious');
    report.push({
      route: hash,
      violations: serious.map((violation) => ({ id: violation.id, impact: violation.impact, nodes: violation.nodes.length })),
      passes: results.passes.length,
    });
    expect(serious, `${hash} 存在严重违规：${serious.map((item) => item.id).join(', ')}`).toHaveLength(0);
  }

  // 状态必须同时有文字与图标，而不是只靠颜色
  const statusBadge = page.locator('.badge').first();
  const badgeText = await statusBadge.innerText();
  expect(badgeText.trim().length, '状态徽章必须带文字').toBeGreaterThan(0);
  const hasIcon = await page.locator('.badge [aria-hidden="true"], .state-icon, .banner-icon').count();
  expect(hasIcon, '状态必须配图标').toBeGreaterThan(0);

  evidence.writeJson('a11y.json', { report, contrastRuleDisabled: '颜色对比度单独人工检查（见报告说明）' });
});

test('DD-PERF-01 工厂页连续操作 60 秒无超过 1 秒的 UI 冻结', async ({ page, evidence }) => {
  await seedTelemetry(page, { rms: 0.02 });
  await page.goto('/dashboard-v2/#/factory');
  await page.waitForTimeout(2000);

  await page.evaluate(() => {
    const state = { longTasks: [] as number[], frames: 0, maxFrameGapMs: 0, lastFrame: performance.now() };
    (window as unknown as Record<string, unknown>).__perf = state;
    try {
      new PerformanceObserver((list) => {
        for (const entry of list.getEntries()) state.longTasks.push(Math.round(entry.duration));
      }).observe({ entryTypes: ['longtask'] });
    } catch { /* 浏览器不支持 longtask */ }
    const tick = () => {
      const now = performance.now();
      state.maxFrameGapMs = Math.max(state.maxFrameGapMs, now - state.lastFrame);
      state.lastFrame = now;
      state.frames += 1;
      if (state.frames < 4000) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  });

  const deadline = Date.now() + 60_000;
  let interactions = 0;
  while (Date.now() < deadline) {
    await page.mouse.move(700 + (interactions % 5) * 40, 500 + (interactions % 3) * 30);
    await page.mouse.wheel(0, interactions % 2 === 0 ? 120 : -120);
    await page.keyboard.press('ArrowLeft');
    await page.keyboard.press('ArrowRight');
    interactions += 1;
    await page.waitForTimeout(250);
  }

  const perf = await page.evaluate(() => {
    const state = (window as unknown as { __perf: { longTasks: number[]; frames: number; maxFrameGapMs: number } }).__perf;
    const memory = (performance as unknown as { memory?: { usedJSHeapSize: number; jsHeapSizeLimit: number } }).memory;
    return {
      longTasks: state.longTasks,
      longTaskCount: state.longTasks.length,
      longestTaskMs: state.longTasks.length ? Math.max(...state.longTasks) : 0,
      frames: state.frames,
      maxFrameGapMs: Math.round(state.maxFrameGapMs),
      heapUsedMB: memory ? Math.round(memory.usedJSHeapSize / 1048576) : null,
      heapLimitMB: memory ? Math.round(memory.jsHeapSizeLimit / 1048576) : null,
    };
  });

  const metrics = { durationSeconds: 60, interactions, ...perf };
  evidence.writeJson('perf-60s.json', metrics);
  fs.writeFileSync(path.join(process.env.DD_EVIDENCE_DIR ?? '', 'metrics.json'), `${JSON.stringify(metrics, null, 2)}\n`, 'utf8');

  expect(metrics.longestTaskMs, '不得出现超过 1 秒的冻结').toBeLessThan(1000);
  expect(metrics.frames, '渲染循环应保持运行').toBeGreaterThan(60);
  void seed;
});
