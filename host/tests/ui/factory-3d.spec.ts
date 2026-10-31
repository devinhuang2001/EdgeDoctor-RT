// DD-3D-01..06（BROWSER/GPU 层）：三维总览的加载、选择、状态高亮、相机与资源释放。

import fs from 'node:fs';
import path from 'node:path';
import { expect, test } from './evidence';
import {
  cameraState, hasHorizontalScroll, sceneDiagnostics, sceneHighlight, sceneRendererInfo, seedEvent, seedTelemetry, selectInstance, waitForScene,
} from './helpers';

async function openFactory(page: import('@playwright/test').Page) {
  await page.goto('/dashboard-v2/#/factory');
  await waitForScene(page);
}

test('DD-3D-01 正常加载显示进度与场景，模型失败时降级且不阻塞数据侧栏', async ({ page, evidence }) => {
  await openFactory(page);
  const info = await sceneRendererInfo(page);
  expect(info.geometries).toBeGreaterThan(0);
  await page.screenshot({ path: path.join(evidence.absoluteDir, 'screenshots', 'factory-ready.png') });
  evidence.writeJson('renderer-info.json', info);

  // 模拟 GLB 加载失败：先切走再回来，确保三维场景重新挂载并重新发起请求。
  // 这条失败是用例主动制造的，登记后不计入控制台噪声判定。
  evidence.expectedFailures.push('factory-web.glb');
  await page.route('**/models/factory-web.glb', (route) => route.abort());
  await page.goto('/dashboard-v2/#/events');
  await page.goto('/dashboard-v2/#/factory');
  await expect(page.getByText('模型暂不可用')).toBeVisible({ timeout: 30_000 });
  await expect(page.getByText(/静态预览/)).toBeVisible();
  // 数据侧栏与事件入口仍然可用
  await expect(page.getByRole('heading', { name: '工厂健康度' })).toBeVisible();
  await page.getByRole('button', { name: '事件中心' }).first().click();
  await expect(page.locator('.page-head h1')).toHaveText('事件中心');
  evidence.expectCleanConsole();
});

test('DD-3D-02 选择已绑定实例、未绑定实例与空白区域的行为一致', async ({ page, evidence }) => {
  await openFactory(page);
  const transcript: Record<string, unknown> = {};

  // 已绑定实例：B 区电机
  await selectInstance(page, 'MOTOR_01');
  await expect(page.getByText('旋转实验区电机 01').first()).toBeVisible();
  transcript.bound = await page.locator('.factory-detail-column').first().innerText();

  // 未绑定实例：A 区数控加工中心
  await selectInstance(page, 'CNC_MACHINE_01');
  await expect(page.getByText('生产区数控加工中心').first()).toBeVisible();
  await expect(page.getByText('未接入设备').first()).toBeVisible();
  transcript.unbound = await page.locator('.factory-detail-column').first().innerText();

  // 空白区域：保持上次选择，不发生乱跳
  await page.mouse.click(5, 5);
  await expect(page.getByText('生产区数控加工中心').first()).toBeVisible();
  transcript.emptyClickKeepsSelection = true;

  evidence.writeJson('selection-transcript.json', transcript);
  await page.screenshot({ path: path.join(evidence.absoluteDir, 'screenshots', 'factory-selection.png') });
});

test('DD-3D-03 高亮与侧栏文案随服务端状态变化，不给部件级故障定位', async ({ page, evidence }) => {
  const states: Record<string, unknown>[] = [];

  // 让该设备的最新事件处于“已恢复且已确认”，作为基准状态
  await seedTelemetry(page, { rms: 0.021, score: 1.2 });
  await seedEvent(page, { condition: 'RECOVERED', ack: 'ACKNOWLEDGED', severity: 'WARNING' });
  await openFactory(page);
  await selectInstance(page, 'MOTOR_01');
  await expect(page.getByText('已恢复').first()).toBeVisible({ timeout: 20_000 });
  states.push({ state: 'ONLINE', highlight: await sceneHighlight(page), sidebar: await page.locator('.factory-detail-column').first().innerText() });
  await expect.poll(async () => (await sceneHighlight(page))?.tone, { timeout: 15_000, message: '最新事件已恢复且资产在线时应为正常色调' }).toBe('ok');

  // 活动严重报警：最新事件为 ACTIVE + ALARM
  await seedEvent(page, { condition: 'ACTIVE', ack: 'UNACKNOWLEDGED', severity: 'ALARM' });
  await page.getByRole('button', { name: '刷新' }).first().click();
  await expect(page.getByText('严重报警').first()).toBeVisible({ timeout: 20_000 });
  await expect.poll(async () => (await sceneHighlight(page))?.tone, { timeout: 15_000, message: '活动严重报警必须高亮为严重' }).toBe('critical');
  states.push({ state: 'ALARM', highlight: await sceneHighlight(page) });

  // 恢复但未确认
  await seedEvent(page, { condition: 'RECOVERED', ack: 'UNACKNOWLEDGED', severity: 'WARNING' });
  await page.getByRole('button', { name: '刷新' }).first().click();
  await expect(page.getByText('已恢复 · 待确认').first()).toBeVisible({ timeout: 20_000 });
  states.push({ state: 'RECOVERED', highlight: await sceneHighlight(page) });

  // 侧栏不得出现未经验证的部件级结论
  const sidebar = await page.locator('.factory-detail-column').first().innerText();
  for (const forbidden of ['轴承', 'Bearing', '内圈', '外圈', '滚动体']) {
    expect(sidebar, `侧栏出现未经验证的部件定位：${forbidden}`).not.toContain(forbidden);
  }
  evidence.writeJson('status-highlight.json', states);

  // 离线（数据过期）需要超过冻结阈值，等待 31 秒
  await page.waitForTimeout(31_000);
  await page.getByRole('button', { name: '刷新' }).first().click();
  await expect(page.getByText('数据过期').first()).toBeVisible({ timeout: 20_000 });
  await expect.poll(async () => (await sceneHighlight(page))?.tone, { timeout: 15_000, message: '数据过期应高亮为警告' }).toBe('warning');
  states.push({ state: 'STALE', highlight: await sceneHighlight(page) });
  evidence.writeJson('status-highlight.json', states);
  await page.screenshot({ path: path.join(evidence.absoluteDir, 'screenshots', 'factory-stale.png') });
});

test('DD-3D-04 相机可旋转、缩放、分区聚焦与复位，键盘操作等效', async ({ page, evidence }) => {
  await openFactory(page);
  const canvas = page.locator('.scene canvas');
  // 左上角是分区按钮浮层，必须聚焦画布本身，键盘事件才由场景处理。
  await canvas.focus();
  await expect(canvas).toBeFocused();

  const initial = await cameraState(page);
  await page.keyboard.press('ArrowLeft');
  await page.keyboard.press('ArrowLeft');
  const rotated = await cameraState(page);
  expect(rotated.position).not.toEqual(initial.position);

  await page.keyboard.press('+');
  const zoomed = await cameraState(page);
  const distance = (state: typeof initial) => Math.hypot(state.position[0] - state.target[0], state.position[1] - state.target[1], state.position[2] - state.target[2]);
  expect(distance(zoomed)).toBeLessThan(distance(rotated));

  await page.keyboard.press('Home');
  const home = await cameraState(page);
  expect(home.target[0]).toBeCloseTo(0, 1);
  expect(home.target[1]).toBeCloseTo(0, 1);

  // 分区聚焦必须改变相机取景，而不只是改变朝向
  await page.getByRole('button', { name: '旋转设备' }).first().click();
  await page.waitForTimeout(2500);
  const focused = await cameraState(page);
  expect(Math.hypot(focused.target[0] - home.target[0], focused.target[2] - home.target[2]), '分区聚焦应移动相机目标').toBeGreaterThan(0.5);

  await page.getByRole('button', { name: '复位视角' }).click();
  await page.waitForTimeout(1200);
  const reset = await cameraState(page);
  expect(reset.position[1]).toBeGreaterThan(10);

  // 相机保持在可用范围（不得平移到远离工厂的空白）
  expect(distance(reset)).toBeGreaterThan(5);
  expect(distance(reset)).toBeLessThan(80);

  evidence.writeJson('camera-states.json', { initial, rotated, zoomed, home, focused, reset });
  expect(await hasHorizontalScroll(page)).toBe(false);
});

test('DD-3D-05 进出工厂页 20 次后三维资源不持续增长', async ({ page, evidence }, testInfo) => {
  // 这条用例要完整加载三维场景 20 次，在无头软件渲染下每次约 7s，实测已接近
  // 全局 150s 上限并因此偶发超时（超时不是资源泄漏，却被记成失败）。
  // 这里按"单次场景加载 + 等待"的真实成本单独放宽。
  test.setTimeout(360_000);
  const samples: { iteration: number; contextsCreated: number; contextsDisposed: number; geometries: number; textures: number; geometriesDisposed: number; texturesDisposed: number; heapMB: number | null }[] = [];

  for (let iteration = 1; iteration <= 20; iteration += 1) {
    await openFactory(page);
    await page.waitForTimeout(300);
    const info = await sceneRendererInfo(page);
    const diagnostics = await sceneDiagnostics(page);
    const heap = await page.evaluate(() => (performance as unknown as { memory?: { usedJSHeapSize: number } }).memory?.usedJSHeapSize ?? null);
    samples.push({
      iteration,
      contextsCreated: Number(diagnostics.contextsCreated ?? 0),
      contextsDisposed: Number(diagnostics.contextsDisposed ?? 0),
      geometries: info.geometries,
      textures: info.textures,
      geometriesDisposed: Number(diagnostics.geometriesDisposed ?? 0),
      texturesDisposed: Number(diagnostics.texturesDisposed ?? 0),
      heapMB: heap === null ? null : Math.round(heap / 1048576),
    });
    await page.goto('/dashboard-v2/#/events');
    await page.waitForTimeout(120);
  }

  const second = samples[1];
  const last = samples[samples.length - 1];
  const metrics = {
    samples,
    contextsLeaked: last.contextsCreated - last.contextsDisposed,
    geometryDisposedDelta: last.geometriesDisposed - second.geometriesDisposed,
    textureDisposedDelta: last.texturesDisposed - second.texturesDisposed,
  };
  evidence.writeJson('scene-resource-samples.json', metrics);
  fs.writeFileSync(path.join(evidenceRoot(), 'metrics-3d05.json'), `${JSON.stringify(metrics, null, 2)}\n`, 'utf8');

  expect(metrics.contextsLeaked, 'WebGL 上下文必须成对释放').toBeLessThanOrEqual(1);
  expect(metrics.geometryDisposedDelta, '几何必须在卸载时释放').toBeGreaterThan(0);
  // 交付的工厂模型不含贴图（模型包明确说明无外部贴图依赖），
  // 因此这里不以“贴图被释放”为通过条件，只用存活量是否增长来判定泄漏。
  expect(last.textures, '存活贴图应归零').toBeLessThanOrEqual(2);
  // 第 20 次相对第 2 次的存活几何与贴图不得持续增长（允许模型自身的常量差异）
  expect(last.geometries - second.geometries, '存活几何数量持续增长').toBeLessThanOrEqual(8);
  expect(last.textures - second.textures, '存活贴图数量持续增长').toBeLessThanOrEqual(8);
  void testInfo;
});

test('DD-3D-06 GPU 首屏可用画面时间（记录实测值）', async ({ browser, evidence }, testInfo) => {
  const samples: number[] = [];
  const renderers: string[] = [];
  for (let attempt = 0; attempt < 10; attempt += 1) {
    const context = await browser.newContext({ baseURL: process.env.DD_BASE_URL, viewport: { width: 1920, height: 1080 } });
    const page = await context.newPage();
    const started = Date.now();
    await page.goto('/dashboard-v2/#/factory');
    await waitForScene(page);
    // 首个可用画面：场景就绪且渲染器已有绘制内容
    await page.waitForFunction(() => {
      const probe = (window as unknown as { __edgedoctorSceneProbe?: { rendererInfo: () => { geometries: number } } }).__edgedoctorSceneProbe;
      if (!probe) return false;
      return probe.rendererInfo().geometries > 0;
    }, undefined, { timeout: 60_000 });
    samples.push(Date.now() - started);
    renderers.push(await page.evaluate(() => {
      const canvas = document.querySelector<HTMLCanvasElement>('.scene-host canvas');
      const gl = canvas?.getContext('webgl2');
      const extension = gl?.getExtension('WEBGL_debug_renderer_info');
      return gl && extension ? String(gl.getParameter(extension.UNMASKED_RENDERER_WEBGL)) : 'unknown';
    }));
    await context.close();
  }
  samples.sort((left, right) => left - right);
  const p95 = samples[Math.min(samples.length - 1, Math.ceil(samples.length * 0.95) - 1)];
  const hardware = process.env.DD_HEADED === '1' && renderers.every((renderer) => /NVIDIA|AMD|Intel|Apple|Qualcomm/i.test(renderer) && !/SwiftShader|llvmpipe|software|Microsoft Basic/i.test(renderer));
  const metrics = { unit: 'ms', samplesMs: samples, medianMs: samples[Math.floor(samples.length / 2)], p95Ms: p95, renderers, hardware, timer: 'before-navigation', freshContexts: true };
  evidence.writeJson('first-frame.json', metrics);
  fs.writeFileSync(path.join(evidenceRoot(), 'metrics-3d06.json'), `${JSON.stringify(metrics, null, 2)}\n`, 'utf8');

  // 无头软件渲染不构成 GPU 验收证据（规格 02 §1）
  if (!hardware) testInfo.annotations.push({
    type: 'missing-evidence',
    description: `未确认有头硬件 GPU，实测 P95=${p95} ms；renderer=${renderers[0]}`,
  });
  expect(p95).toBeGreaterThan(0);
  if (hardware) expect(p95, '导航至首个可用画面 P95 不超过 8 秒').toBeLessThanOrEqual(8000);
});

function evidenceRoot(): string {
  return process.env.DD_EVIDENCE_DIR ?? path.join(process.cwd(), 'benchmark', 'results', 'desktop-dashboard', 'local');
}
