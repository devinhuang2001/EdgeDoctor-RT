// DD-UI-01/02/04/05（VIS/BROWSER 层）：分辨率、键盘、事件列表边界与信号分析状态。

import path from 'node:path';
import { expect, test } from './evidence';
import { assertReachable, buildEdv2, hasHorizontalScroll, seedEvent, seedTelemetry, seedTrend, uniqueId } from './helpers';

const ROUTES = [
  { hash: '#/factory', title: '工厂总览' },
  { hash: '#/monitor/ed84-001', title: '实时监测' },
  { hash: '#/events', title: '事件中心' },
  { hash: '#/analysis/ed84-001', title: '信号分析' },
];

const VIEWPORTS = [
  { width: 1920, height: 1080, name: '1920x1080' },
  { width: 1440, height: 900, name: '1440x900' },
  { width: 1280, height: 720, name: '1280x720' },
];

test('DD-UI-01 三种分辨率下四条路由的截图与布局约束', async ({ page, evidence }) => {
  await seedTelemetry(page, { rms: 0.019, score: 3.1 });
  await seedEvent(page, { condition: 'ACTIVE', ack: 'UNACKNOWLEDGED', severity: 'WARNING' });

  const report: Record<string, unknown>[] = [];
  for (const viewport of VIEWPORTS) {
    await page.setViewportSize({ width: viewport.width, height: viewport.height });
    for (const route of ROUTES) {
      await page.goto(`/dashboard-v2/${route.hash}`);
      await expect(page.locator('.page-head h1')).toHaveText(route.title);
      await page.waitForTimeout(600);

      const overflow = await hasHorizontalScroll(page);
      // 判据是“滚到该元素后仍能被点到”，而不是与固定导航几何不相交：
      // 固定导航必然覆盖滚动经过的内容，只有点不到才算遮挡。
      const blockedButtons = await assertReachable(page, '.btn');
      const blockedMetrics = await assertReachable(page, '.metric');
      const blockedCharts = await assertReachable(page, '.chart-frame');
      const text = await page.locator('body').innerText();
      const screenshot = await evidence.screenshot(`${viewport.name}-${route.hash.replace(/[#/]/g, '')}`);
      report.push({ viewport: viewport.name, route: route.hash, overflow, blockedButtons, blockedMetrics, blockedCharts, chineseRendered: text.includes(route.title), screenshot });
      expect(overflow, `${viewport.name} ${route.hash} 出现横向滚动`).toBe(false);
      expect(blockedButtons, `${viewport.name} ${route.hash} 有操作按钮被遮挡：${blockedButtons.join(' | ')}`).toHaveLength(0);
      expect(blockedMetrics, `${viewport.name} ${route.hash} 有指标被遮挡：${blockedMetrics.join(' | ')}`).toHaveLength(0);
      expect(blockedCharts, `${viewport.name} ${route.hash} 图表被遮挡：${blockedCharts.join(' | ')}`).toHaveLength(0);
    }
  }
  evidence.writeJson('visual-report.json', report);
  evidence.expectCleanConsole();
});

test('DD-UI-01b 加载中、无数据、告警与服务故障状态截图', async ({ page, evidence }) => {
  const screenshots: Record<string, string> = {};

  // 加载中：只延迟第一个汇总请求；路由保留到用例结束，避免取消后继续产生噪声。
  let delayedOnce = false;
  await page.route('**/api/v1/factory/summary', async (route) => {
    if (!delayedOnce) {
      delayedOnce = true;
      await new Promise((resolve) => setTimeout(resolve, 4000));
    }
    try {
      await route.continue();
    } catch { /* 请求可能在延迟期间被取消 */ }
  });
  await page.goto('/dashboard-v2/#/factory');
  await page.waitForTimeout(700);
  screenshots.loading = await evidence.screenshot('state-loading');
  await expect(page.getByRole('heading', { name: '工厂健康度' })).toBeVisible({ timeout: 30_000 });

  // 无数据：未接入资产（未绑定设备）
  await page.goto('/dashboard-v2/#/factory');
  await page.getByRole('button', { name: /生产区数控加工中心/ }).first().click();
  await expect(page.getByText('未接入设备').first()).toBeVisible();
  screenshots.noData = await evidence.screenshot('state-no-data');

  // 告警：活动严重报警
  await seedEvent(page, { condition: 'ACTIVE', ack: 'UNACKNOWLEDGED', severity: 'ALARM' });
  await page.getByRole('button', { name: /旋转实验区电机 01/ }).first().click();
  await page.getByRole('button', { name: '刷新' }).first().click();
  await expect(page.getByText('严重报警').first()).toBeVisible({ timeout: 20_000 });
  screenshots.alarm = await evidence.screenshot('state-alarm');

  // 服务故障：汇总接口 500，历史数据保留
  await page.unroute('**/api/v1/factory/summary');
  await page.route('**/api/v1/factory/summary', (route) => route.fulfill({ status: 500, contentType: 'application/json', body: JSON.stringify({ detail: 'acceptance injected failure' }) }));
  await page.getByRole('button', { name: '刷新' }).first().click();
  await expect(page.getByText('服务不可用').first()).toBeVisible({ timeout: 20_000 });
  await expect(page.getByRole('heading', { name: '工厂健康度' })).toBeVisible();
  screenshots.serviceDown = await evidence.screenshot('state-service-down');

  evidence.writeJson('state-screenshots.json', screenshots);
  expect(Object.values(screenshots).every((value) => value.endsWith('.png'))).toBe(true);
});

test('DD-UI-02 键盘可完成导航、筛选与确认操作，焦点可见', async ({ page, evidence }) => {
  const eventId = await seedEvent(page, { eventId: uniqueId('dd-ui-02'), condition: 'ACTIVE', ack: 'UNACKNOWLEDGED' });
  await page.goto('/dashboard-v2/#/events');
  await expect(page.getByRole('heading', { name: '事件中心' }).first()).toBeVisible();

  // 键盘导航到工厂总览
  const factoryNav = page.getByRole('button', { name: '工厂总览' }).last();
  await factoryNav.focus();
  await page.keyboard.press('Enter');
  await expect(page.locator('.page-head h1')).toHaveText('工厂总览');
  await expect(page.getByRole('button', { name: '工厂总览' }).last()).toHaveAttribute('aria-current', 'page');

  const outline = await page.evaluate(() => {
    const element = document.activeElement as HTMLElement | null;
    if (!element) return null;
    const style = getComputedStyle(element);
    return { outlineWidth: style.outlineWidth, outlineStyle: style.outlineStyle, tag: element.tagName, label: element.getAttribute('aria-label') ?? element.textContent?.slice(0, 20) ?? '' };
  });
  expect(outline?.outlineStyle, '焦点必须可见').not.toBe('none');

  await page.evaluate(() => {
    const target = document.querySelector<HTMLElement>('button[aria-current], .bottom-nav button');
    target?.focus();
  });

  // 键盘完成筛选
  await page.goto('/dashboard-v2/#/events');
  const filter = page.getByRole('button', { name: /已恢复/ }).first();
  await filter.focus();
  await page.keyboard.press('Enter');
  await expect(filter).toHaveAttribute('aria-pressed', 'true');

  // 键盘完成确认操作：先切回“全部”筛选，再确认该事件在最近 50 条列表内
  const all = page.getByRole('button', { name: /全部 \d+/ }).first();
  await all.focus();
  await page.keyboard.press('Enter');
  const card = page.getByRole('button', { name: new RegExp(eventId) });
  await expect(card, '播种的事件应出现在最近 50 条列表内').toBeVisible({ timeout: 20_000 });
  await card.click();
  const ack = page.getByRole('button', { name: '确认事件' });
  await ack.focus();
  await page.keyboard.press('Enter');
  await expect(page.locator('.toast').first()).toContainText('确认请求已提交');

  const liveRegions = await page.locator('[aria-live]').count();
  evidence.writeJson('keyboard.json', { outline, liveRegions, filter: '已恢复', ackSubmitted: true });
  expect(liveRegions, '需要 aria-live 区域播报状态变化').toBeGreaterThan(0);
});

test('DD-UI-04 事件列表在 0/1/50 条与恶意字符串下可读且被转义', async ({ page, evidence }) => {
  // 先填满列表，再播种恶意与超长 ID，保证它们位于最新 50 条之内。
  for (let index = 0; index < 52; index += 1) {
    await seedEvent(page, { eventId: uniqueId(`dd-ui-04-${index}`) });
  }
  const xssMarker = `xss${Date.now()}`;
  const xssEventId = `<img src=x onerror="window.${xssMarker}=1">`;
  await seedEvent(page, { eventId: xssEventId, condition: 'ACTIVE', ack: 'UNACKNOWLEDGED' });
  const longId = `evt-${'长'.repeat(40)}-${'a'.repeat(40)}`;
  await seedEvent(page, { eventId: longId, peakDeviation: 1.2345 });

  await page.goto('/dashboard-v2/#/events');
  await expect(page.getByText('全部 50').first()).toBeVisible({ timeout: 20_000 });
  await expect(page.locator('.events-panel')).toContainText('<img src=x onerror=', { timeout: 20_000 });
  const listText = await page.locator('.events-panel').innerText();
  await page.getByRole('button', { name: new RegExp(longId) }).click();
  await expect(page.getByText('服务端上报基线偏离分数 1.2345（无量纲）', { exact: false })).toBeVisible();
  expect(await page.locator('body').innerText()).not.toContain('1.2345 g');
  expect(listText, '恶意事件 ID 必须以文本显示').toContain('<img src=x onerror=');
  expect(await page.evaluate((marker) => Boolean((window as unknown as Record<string, unknown>)[marker]), xssMarker), '注入的脚本不得执行').toBe(false);

  const note = await page.locator('.events-panel .panel-note').innerText();
  expect(note, '必须说明列表是最近 50 条而非全厂总量').toContain('不代表全厂历史总量');

  const imgCount = await page.locator('.events-panel img').count();
  evidence.writeJson('event-list.json', { note, imgCount, longIdRendered: listText.includes('长') });
  expect(imgCount, '不得渲染注入的图片标签').toBe(0);
  await page.screenshot({ path: path.join(evidence.absoluteDir, 'screenshots', 'event-list.png') });
});

test('DD-UI-05 信号分析页拒绝无效证据并展示已校验 XYZ 波形与频谱', async ({ page, evidence }) => {
  await seedTrend(page, [0.01, 0.02, null, 0.03, 0.08]);
  const eventId = await seedEvent(page, { eventId: uniqueId('dd-ui-05') });

  await page.goto('/dashboard-v2/#/analysis/ed84-001');
  await expect(page.getByRole('heading', { name: '特征概览' })).toBeVisible();
  await expect(page.getByText('RMS 最新值')).toBeVisible();
  await expect(page.getByText('需要原始波形').first()).toBeVisible();

  const spectrum = page.locator('.spectrum-frame');
  await expect(spectrum).toBeVisible();
  await expect(spectrum).toContainText('不绘制频谱');
  await expect(spectrum).toContainText('未获得证据采样率');
  expect(await spectrum.locator('canvas, path.chart-line').count(), '不得绘制谱线').toBe(0);

  const select = page.locator('select');
  await expect(select.locator(`option[value="${eventId}"]`), '事件应出现在分析对象选择器中').toHaveCount(1, { timeout: 20_000 });
  await select.selectOption(eventId);
  await expect(page.getByText('verified evidence analysis is not available')).toBeVisible({ timeout: 20_000 });
  await expect(page.getByText('不可用').first()).toBeVisible();

  const bodyText = await page.locator('body').innerText();
  for (const forbidden of ['42.3', '96.8', '轴承异常']) {
    expect(bodyText, `不得出现示例诊断内容 ${forbidden}`).not.toContain(forbidden);
  }
  evidence.writeJson('analysis-states.json', { eventId, spectrumText: await spectrum.innerText() });
  await page.screenshot({ path: path.join(evidence.absoluteDir, 'screenshots', 'analysis-unavailable.png') });

  const payload = Buffer.alloc(208 * 6);
  for (let i = 0; i < 208; i += 1) {
    payload.writeInt16LE(Math.round(1000 * Math.sin(2 * Math.PI * 13 * i / 208)), i * 6);
    payload.writeInt16LE(8000, i * 6 + 4);
  }
  const blob = buildEdv2({ event_id: eventId, encoding: 'xyz_int16_le', complete: true, sample_rate_hz: 208, sample_rate_kind: 'nominal', sensitivity_g_per_adc: 0.000122 }, payload);
  const upload = await page.request.post(`/api/v1/events/${eventId}/evidence`, { multipart: { upload: { name: 'xyz.edv2', mimeType: 'application/octet-stream', buffer: blob } } });
  expect(upload.status()).toBe(201);
  await page.reload();
  await expect(page.getByTestId('verified-spectrum')).toBeVisible();
  await expect(page.getByTestId('verified-waveform')).toBeVisible();
  await expect(page.getByText('104 Hz').first()).toBeVisible();
  expect(await page.getByTestId('verified-spectrum').locator('path.chart-line').count()).toBe(3);
  const parsed = await (await page.request.get(`/api/v1/events/${eventId}/analysis`)).json();
  expect(parsed.available).toBe(true);
  expect(parsed.spectrum.frequency_hz.at(-1)).toBe(104);
  expect(parsed.dominant_peaks[0].frequency_hz).toBeCloseTo(13, 0);
  await page.getByRole('button', { name: '统计特征', exact: true }).click();
  await expect(page.getByRole('columnheader', { name: 'AC RMS (g)', exact: true })).toBeVisible();
  await page.getByRole('button', { name: '包络', exact: true }).click();
  await expect(page.getByText('去均值后取绝对值')).toBeVisible();
  const corruptId = await seedEvent(page, { eventId: uniqueId('dd-ui-05-corrupt') });
  const corrupt = Buffer.from(blob); corrupt[corrupt.length - 1] ^= 1;
  const rejected = await page.request.post(`/api/v1/events/${corruptId}/evidence`, { multipart: { upload: { name: 'bad.edv2', mimeType: 'application/octet-stream', buffer: corrupt } } });
  expect(rejected.status()).toBe(422);
  await page.reload();
  await page.locator('select').selectOption(corruptId);
  await expect(page.getByTestId('verified-spectrum')).toHaveCount(0);
  await expect(page.getByText('verified evidence analysis is not available')).toBeVisible();
  evidence.writeJson('analysis-verified.json', { eventId, sampleCount: parsed.sample_count, parser: parsed.parser_version, maxFrequency: parsed.spectrum.frequency_hz.at(-1), peak: parsed.dominant_peaks[0], corruptStatus: rejected.status() });
});
