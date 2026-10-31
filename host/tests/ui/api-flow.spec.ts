// DD-API-03..06（BROWSER 层）：切页竞态、命令回执、留证状态与 SSE 去抖/回退。

import path from 'node:path';
import { expect, test } from './evidence';
import { buildEdv2, seedEvent, uniqueId } from './helpers';

async function pendingCommandId(page: import('@playwright/test').Page, deviceId = 'ed84-001'): Promise<string> {
  const response = await page.request.get(`/api/v1/commands?device_id=${deviceId}&status=PENDING`);
  const commands = (await response.json()) as { command_id: string; event_id: string }[];
  expect(commands.length, '应存在待回执命令').toBeGreaterThan(0);
  return commands[0].command_id;
}

async function applyReceipt(page: import('@playwright/test').Page, commandId: string, status: 'APPLIED' | 'REJECTED' | 'EXPIRED') {
  const response = await page.request.post(`/api/v1/commands/${commandId}/result`, { data: { status, detail: `acceptance-${status.toLowerCase()}` } });
  expect(response.status(), `回执写入失败：${response.status()}`).toBe(200);
}

test('DD-API-03 切换到另一台设备后，延迟返回的旧设备响应不得覆盖新设备页面', async ({ page, evidence }) => {
  // 规格要求的是 A→B 两台设备切换。冻结绑定里只有 ed84-001 一台 verified 设备，
  // 因此这里用总览接口注入第二台已验证设备来构造 A→B。这是客户端代际保护的场景
  // 夹具，不代表现场真有两台已接入设备；两台真实设备的 LAN 走查仍属未完成的验收。
  const DEVICE_A = 'ed84-001';
  const DEVICE_B = 'ed84-002';
  const A_ADVICE = '设备 A 迟到响应：严重报警';
  const B_ADVICE = '设备 B 当前响应：报警已恢复';

  // 先确认服务端就绪。上一次这个用例失败，是因为首个 /health/ready 落在 503 上，
  // 刷新在第一步就中止，整个用例只发出过一次 status 请求——竞态前置条件根本没建立。
  await expect.poll(async () => (await page.request.get('/health/ready')).status(), {
    message: '服务端必须在用例开始前就绪',
    timeout: 30_000,
  }).toBe(200);

  await page.route('**/api/v1/factory/summary', async (route) => {
    const response = await route.fetch();
    const body = (await response.json()) as { assets?: unknown[]; counts?: Record<string, number> };
    body.assets = [...(body.assets ?? []), {
      asset_id: 'motor-02',
      device_id: DEVICE_B,
      scene_instance: 'MOTOR_02',
      zone: 'B',
      detail_model: 'models/induction_motor.glb',
      sensor_node: 'IMU_Sensor',
      display_name_zh: '旋转实验区电机 02（夹具）',
      binding_state: 'verified',
      status: 'ONLINE',
      last_telemetry_received_ms: Date.now(),
    }];
    body.counts = { ...(body.counts ?? { ONLINE: 0, STALE: 0, UNKNOWN: 0 }), ONLINE: 2 };
    await route.fulfill({ response, json: body });
  });

  const snapshotBody = (deviceId: string, status: string, advice: string) => ({
    device_id: deviceId,
    status,
    snapshot_age_ms: 1000,
    telemetry: { mono_ms: 1, rms_vector_g: 0.9, anomaly_score: 3.2 },
    advice,
    read_only: true,
  });

  // A 的响应被显式扣住，同时记录“请求确实已经发出”。这是确定性的前置条件，
  // 不再依赖固定 sleep 去猜旧请求是否来得及发出。
  let releaseA: () => void = () => {};
  const gateA = new Promise<void>((resolve) => { releaseA = resolve; });
  let markAIssued: () => void = () => {};
  const aIssued = new Promise<void>((resolve) => { markAIssued = resolve; });
  let aCalls = 0;
  let aCancelled = false;
  await page.route(`**/api/v1/query/status?device_id=${DEVICE_A}`, async (route) => {
    aCalls += 1;
    markAIssued();
    await gateA;
    try {
      await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(snapshotBody(DEVICE_A, 'ALARM', A_ADVICE)) });
    } catch {
      // 切到 B 时前端会 abort 旧代际的请求，此时 fulfill 抛错是预期行为。
      aCancelled = true;
    }
  });
  await page.route(`**/api/v1/query/status?device_id=${DEVICE_B}`, async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(snapshotBody(DEVICE_B, 'RECOVERED', B_ADVICE)) });
  });

  // 打开 A 的监测页，等到 A 的状态请求真的离开浏览器。
  await page.goto(`/dashboard-v2/#/monitor/${DEVICE_A}`);
  await aIssued;
  expect(aCalls, 'A 的慢请求必须先发出，竞态前置条件才成立').toBeGreaterThan(0);

  // 立刻切到 B：前端应放弃 A 这一代，并且只渲染 B 的结论。
  await page.goto(`/dashboard-v2/#/monitor/${DEVICE_B}`);
  await expect(page.locator('.banner')).toContainText(B_ADVICE, { timeout: 20_000 });

  // 放行 A 的迟到响应，确认它不会覆盖 B。
  releaseA();
  await page.waitForTimeout(1500);
  const banner = await page.locator('.banner').innerText();
  expect(banner, 'A 的迟到响应不得覆盖 B 的页面结论').not.toContain(A_ADVICE);
  expect(banner).toContain(B_ADVICE);

  evidence.writeJson('race-device-switch.json', {
    device_a: DEVICE_A, device_b: DEVICE_B, a_calls: aCalls, a_cancelled: aCancelled, banner, final_url: page.url(),
  });
  await evidence.screenshot('race-final.png');
});

test('DD-API-04 确认请求只有 APPLIED 才显示已应用，拒绝与过期保留可重试解释', async ({ page, evidence }) => {
  const cases: { status: 'APPLIED' | 'REJECTED' | 'EXPIRED'; expectText: string }[] = [
    { status: 'APPLIED', expectText: '设备已应用' },
    { status: 'REJECTED', expectText: '设备拒绝' },
    { status: 'EXPIRED', expectText: '命令已过期' },
  ];
  const transcript: unknown[] = [];

  for (const item of cases) {
    const eventId = await seedEvent(page, { eventId: uniqueId(`dd-api-04-${item.status.toLowerCase()}`), condition: 'ACTIVE', ack: 'UNACKNOWLEDGED', severity: 'ALARM' });
    await page.goto(`/dashboard-v2/#/events`);
    await page.getByRole('button', { name: new RegExp(eventId) }).click();
    await expect(page.getByRole('heading', { name: '振动偏离基线' }).first()).toBeVisible();

    await page.getByRole('button', { name: '确认事件' }).click();
    await expect(page.locator('.toast').first()).toContainText('确认请求已提交');
    await expect(page.getByText('等待设备回执').first()).toBeVisible();

    await applyReceipt(page, await pendingCommandId(page), item.status);
    await expect(page.getByText(item.expectText).first()).toBeVisible({ timeout: 20_000 });
    if (item.status !== 'APPLIED') {
      await expect(page.getByText(/可在设备就绪后重试|请重新提交/).first()).toBeVisible();
    }
    transcript.push({ eventId, status: item.status, expected: item.expectText });
    await page.screenshot({ path: path.join(evidence.absoluteDir, 'screenshots', `ack-${item.status}.png`) });
  }
  evidence.writeJson('ack-transcript.json', transcript);
  evidence.expectCleanConsole();
});

test('DD-API-05 只有 CRC 合格的证据才显示下载，损坏证据与 401 不得称为原始数据可用', async ({ page, evidence }) => {
  const report: Record<string, unknown> = {};

  // 1) 无证据
  const plainEvent = await seedEvent(page, { eventId: uniqueId('dd-api-05-plain') });
  await page.goto('/dashboard-v2/#/events');
  await page.getByRole('button', { name: new RegExp(plainEvent) }).click();
  await expect(page.getByText('尚无留证记录')).toBeVisible();
  report.noEvidence = '尚无留证记录';

  // 2) 损坏证据被服务端拒绝
  const corruptEvent = await seedEvent(page, { eventId: uniqueId('dd-api-05-corrupt') });
  const corrupt = await page.request.post(`/api/v1/events/${corruptEvent}/evidence`, {
    multipart: { upload: { name: 'corrupt.edv2', mimeType: 'application/octet-stream', buffer: Buffer.from('EDV2-not-a-real-frame') } },
  });
  report.corruptUploadStatus = corrupt.status();
  expect(corrupt.status(), '损坏证据必须被服务端拒绝').toBe(422);
  const corruptMeta = await page.request.get(`/api/v1/events/${corruptEvent}/evidence`);
  expect(corruptMeta.status(), '损坏证据不得产生元数据').toBe(404);

  // 3) 合格证据：CRC 通过后才显示下载
  const goodEvent = await seedEvent(page, { eventId: uniqueId('dd-api-05-good') });
  const blob = buildEdv2({ event_id: goodEvent, device_id: 'ed84-001', sample_rate_hz: 208, channels: ['x', 'y', 'z'] }, Buffer.alloc(256, 7));
  const upload = await page.request.post(`/api/v1/events/${goodEvent}/evidence`, {
    multipart: { upload: { name: 'good.edv2', mimeType: 'application/octet-stream', buffer: blob } },
  });
  expect(upload.status(), `合格证据上传应成功：${upload.status()}`).toBe(201);
  report.goodUploadStatus = upload.status();

  await page.goto('/dashboard-v2/#/events');
  await page.getByRole('button', { name: new RegExp(goodEvent) }).click();
  await expect(page.getByRole('link', { name: /下载已校验的 EDV2 原始数据/ })).toBeVisible();
  const download = await page.request.get(`/api/v1/evidence/${goodEvent}/download`);
  expect(download.status()).toBe(200);
  expect((await download.body()).length).toBeGreaterThan(0);
  report.downloadStatus = download.status();
  await page.screenshot({ path: path.join(evidence.absoluteDir, 'screenshots', 'evidence-available.png') });

  // 4) 401：认证被拒绝时不得显示下载
  await page.route('**/api/v1/events/*/evidence', async (route) => {
    await route.fulfill({ status: 401, contentType: 'application/json', body: JSON.stringify({ detail: 'authentication required' }) });
  });
  await page.reload();
  await page.getByRole('button', { name: new RegExp(goodEvent) }).click();
  await expect(page.getByText('认证被拒绝')).toBeVisible();
  await expect(page.getByRole('link', { name: /下载已校验/ })).toHaveCount(0);
  report.unauthorized = '认证被拒绝，且无下载入口';

  evidence.writeJson('evidence-states.json', report);
});

test('DD-API-06 SSE 收到连续通知只刷新一次，断流后按 10 秒轮询恢复', async ({ page, evidence }) => {
  const summaryCalls: number[] = [];
  const streamCalls: number[] = [];
  page.on('request', (request) => {
    const url = request.url();
    if (url.includes('/api/v1/factory/summary')) summaryCalls.push(Date.now());
    if (url.includes('/api/v1/stream')) streamCalls.push(Date.now());
  });

  await page.goto('/dashboard-v2/#/monitor/ed84-001');
  await page.waitForTimeout(3000);
  summaryCalls.length = 0;

  // 连续 20 条通知（每条事件都会写入 audit_log 并触发 SSE 消息）
  for (let index = 0; index < 20; index += 1) {
    await seedEvent(page, { eventId: uniqueId(`dd-api-06-burst-${index}`), revision: 1 });
  }
  await page.waitForTimeout(4000);
  const afterBurst = summaryCalls.length;
  evidence.writeJson('sse-burst.json', { notifications: 20, summaryRequests: afterBurst });
  expect(afterBurst, '20 条通知不得触发 20 次刷新').toBeLessThanOrEqual(4);
  expect(afterBurst, '至少应触发一次去抖刷新').toBeGreaterThanOrEqual(1);

  // 阻断 SSE 后应回退到 10 秒轮询
  await page.route('**/api/v1/stream', (route) => route.abort());
  await page.reload();
  await page.waitForTimeout(2000);
  summaryCalls.length = 0;
  await page.waitForTimeout(26_000);
  const polled = summaryCalls.length;
  evidence.writeJson('polling-fallback.json', { windowMs: 26_000, summaryRequests: polled, streamRequests: streamCalls.length });
  expect(polled, '断流后应至少轮询两次').toBeGreaterThanOrEqual(2);
  expect(polled, '轮询不得变成请求风暴').toBeLessThanOrEqual(6);
  await expect(page.getByText(/轮询模式/)).toBeVisible();
});
