// 验收测试的公共工具：EDV2 造帧、状态播种、场景探针读取。

import type { Page } from '@playwright/test';
import { apiGet, seed, uniqueId } from './evidence';

export { apiGet, seed, uniqueId };

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let index = 0; index < 256; index += 1) {
    let value = index;
    for (let bit = 0; bit < 8; bit += 1) value = value & 1 ? 0xedb88320 ^ (value >>> 1) : value >>> 1;
    table[index] = value >>> 0;
  }
  return table;
})();

export function crc32(buffer: Buffer): number {
  let crc = 0xffffffff;
  for (const byte of buffer) crc = CRC_TABLE[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

/** 构造服务端可校验的 EDV2 帧（与 server/evidence.py 的 pack 一致）。 */
export function buildEdv2(metadata: Record<string, unknown>, payload: Buffer): Buffer {
  const meta = Buffer.from(JSON.stringify(metadata), 'utf8');
  const header = Buffer.alloc(20);
  header.write('EDV2', 0, 'ascii');
  header.writeUInt16LE(2, 4);
  header.writeUInt16LE(20, 6);
  header.writeUInt32LE(meta.length, 8);
  header.writeUInt32LE(payload.length, 12);
  header.writeUInt32LE(crc32(meta), 16);
  const trailer = Buffer.alloc(4);
  trailer.writeUInt32LE(crc32(payload), 0);
  return Buffer.concat([header, meta, payload, trailer]);
}

export type EventSeed = {
  eventId?: string;
  deviceId?: string;
  condition?: 'ACTIVE' | 'RECOVERED';
  ack?: 'UNACKNOWLEDGED' | 'ACKNOWLEDGED';
  severity?: 'WARNING' | 'ALARM';
  revision?: number;
  diagnosis?: string;
  peakDeviation?: number;
  source?: string;
};

export async function seedEvent(page: Page, options: EventSeed = {}): Promise<string> {
  const eventId = options.eventId ?? uniqueId('evt');
  const body = {
    schema_version: 1,
    device_id: options.deviceId ?? 'ed84-001',
    event_id: eventId,
    revision: options.revision ?? 1,
    source: options.source ?? 'LIVE_IMU',
    condition: options.condition ?? 'ACTIVE',
    ack: options.ack ?? 'UNACKNOWLEDGED',
    severity: options.severity ?? 'WARNING',
    diagnosis: options.diagnosis ?? 'UNKNOWN_ANOMALY',
    quality: 'VALID',
    ...(options.peakDeviation === undefined ? {} : { peak_deviation: options.peakDeviation }),
  };
  const response = await seed('/api/v1/events', body, page);
  if (response.status !== 200) throw new Error(`事件播种失败：${response.status} ${JSON.stringify(response.body)}`);
  return eventId;
}

export async function seedTelemetry(page: Page, options: { deviceId?: string; monoMs?: number; rms?: number; score?: number; alarm?: boolean } = {}): Promise<void> {
  const body = {
    device_id: options.deviceId ?? 'ed84-001',
    mono_ms: options.monoMs ?? Date.now() % 1_000_000,
    ...(options.rms === undefined ? {} : { rms_vector_g: options.rms }),
    ...(options.score === undefined ? {} : { anomaly_score: options.score }),
    ...(options.alarm === undefined ? {} : { alarm: options.alarm }),
  };
  const response = await seed('/api/v1/telemetry', body, page);
  if (response.status !== 200) throw new Error(`遥测播种失败：${response.status} ${JSON.stringify(response.body)}`);
}

export async function seedTrend(page: Page, values: (number | null)[], deviceId = 'ed84-001'): Promise<void> {
  const base = Date.now() % 100_000;
  for (const [index, value] of values.entries()) {
    await seedTelemetry(page, {
      deviceId,
      monoMs: base + index * 1000,
      ...(value === null ? {} : { rms: value }),
      ...(value !== null && value > 0.5 ? { alarm: true } : {}),
    });
  }
}

export type Probe = {
  ready: boolean;
  projectInstance: (name: string) => { x: number; y: number; visible: boolean } | null;
  selectedInstance: () => string | null;
  projectZone: (zone: string) => { x: number; y: number } | null;
  camera: () => { position: [number, number, number]; target: [number, number, number] };
  highlight: () => { name: string; tone: string } | null;
  rendererInfo: () => { geometries: number; textures: number; programs: number | null };
  diagnostics: () => Record<string, number | string[]>;
};

/** 等待三维场景就绪（模型加载完成后探针才 ready）。 */
export async function waitForScene(page: Page, timeoutMs = 60_000): Promise<void> {
  await page.waitForFunction(() => Boolean((window as unknown as { __edgedoctorSceneProbe?: { ready?: boolean } }).__edgedoctorSceneProbe?.ready), undefined, { timeout: timeoutMs });
}

export function probeSnapshot(page: Page): Promise<Probe | null> {
  return page.evaluate(() => {
    const probe = (window as unknown as { __edgedoctorSceneProbe?: Probe }).__edgedoctorSceneProbe;
    if (!probe) return null;
    return {
      ready: probe.ready,
      projectInstance: () => null,
      selectedInstance: () => null,
      projectZone: () => null,
      camera: probe.camera(),
      highlight: probe.highlight(),
      rendererInfo: probe.rendererInfo(),
      diagnostics: probe.diagnostics(),
    } as unknown as Probe;
  });
}

export async function cameraState(page: Page): Promise<{ position: [number, number, number]; target: [number, number, number] }> {
  return page.evaluate(() => {
    const probe = (window as unknown as { __edgedoctorSceneProbe?: Probe }).__edgedoctorSceneProbe;
    if (!probe) throw new Error('三维探针不可用');
    return probe.camera();
  });
}

export async function sceneHighlight(page: Page): Promise<{ name: string; tone: string } | null> {
  return page.evaluate(() => {
    const probe = (window as unknown as { __edgedoctorSceneProbe?: Probe }).__edgedoctorSceneProbe;
    return probe ? probe.highlight() : null;
  });
}

export async function sceneDiagnostics(page: Page): Promise<Record<string, number | string[]>> {
  return page.evaluate(() => {
    const probe = (window as unknown as { __edgedoctorSceneProbe?: Probe }).__edgedoctorSceneProbe;
    return probe ? probe.diagnostics() : {};
  });
}

export async function sceneRendererInfo(page: Page): Promise<{ geometries: number; textures: number; programs: number | null }> {
  return page.evaluate(() => {
    const probe = (window as unknown as { __edgedoctorSceneProbe?: Probe }).__edgedoctorSceneProbe;
    if (!probe) throw new Error('三维探针不可用');
    return probe.rendererInfo();
  });
}

function projectInstancePoint(page: Page, instanceName: string) {
  return page.evaluate((name) => {
    const probe = (window as unknown as { __edgedoctorSceneProbe?: Probe }).__edgedoctorSceneProbe;
    return probe ? probe.projectInstance(name) : null;
  }, instanceName);
}

function readSelectedInstance(page: Page): Promise<string | null> {
  return page.evaluate(() => {
    const probe = (window as unknown as { __edgedoctorSceneProbe?: Probe }).__edgedoctorSceneProbe;
    return probe ? probe.selectedInstance() : null;
  });
}

/**
 * 把场景实例投影到页面坐标后点击，用于真实射线拾取。
 *
 * 只在实例已投影为可见时才点击，调用方必须判断返回值。想"确保选中它"请用
 * selectInstance——那里会等待可见并复核选中结果。
 */
export async function clickInstance(page: Page, instanceName: string): Promise<boolean> {
  const point = await projectInstancePoint(page, instanceName);
  if (!point || !point.visible) return false;
  await page.mouse.click(point.x, point.y);
  return true;
}

/**
 * 选中一个场景实例，直到探针确认选中的就是它。
 *
 * 为什么不能只点一次：`probe.ready` 只表示模型加载完成，实例投影坐标还要等首帧
 * 渲染与相机取景稳定。更关键的是——投影可见并不代表射线命中了目标；落空的点击
 * 会被场景忽略，页面停在默认选中的资产上，断言随之以完全误导的方式失败
 * （DD-3D-03 曾因此假失败：点的是电机，侧栏里却是数控加工中心）。
 * 这里对"可见 → 点击 → 复核选中"整体重试，失败时带出最后一次的实际选中值。
 */
export async function selectInstance(page: Page, instanceName: string, timeoutMs = 30_000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let lastReason = '尚未取得投影坐标';
  while (Date.now() < deadline) {
    const point = await projectInstancePoint(page, instanceName);
    if (point?.visible) {
      await page.mouse.click(point.x, point.y);
      const selected = await readSelectedInstance(page);
      if (selected === instanceName) return;
      lastReason = `点击 (${Math.round(point.x)}, ${Math.round(point.y)}) 后选中的是 ${selected ?? 'null'}`;
    } else {
      lastReason = `${instanceName} 尚未投影为可见`;
    }
    await page.waitForTimeout(200);
  }
  throw new Error(`${timeoutMs}ms 内未能选中场景实例 ${instanceName}：${lastReason}`);
}

export async function clickZone(page: Page, zone: string): Promise<boolean> {
  const point = await page.evaluate((name) => {
    const probe = (window as unknown as { __edgedoctorSceneProbe?: Probe }).__edgedoctorSceneProbe;
    return probe ? probe.projectZone(name) : null;
  }, zone);
  if (!point) return false;
  await page.mouse.click(point.x, point.y);
  return true;
}

/**
 * 断言元素在滚动到可视区中部后没有被固定底部导航遮挡（DD-UI-01）。
 * 固定导航必然覆盖滚动经过的内容，因此判据是“元素能否被点到”，而不是几何不相交。
 */
export async function assertReachable(page: Page, selector: string, sample = 6): Promise<string[]> {
  return page.evaluate(({ selector: target, sample: count }) => {
    const nodes = [...document.querySelectorAll<HTMLElement>(target)].slice(0, count);
    const blocked: string[] = [];
    for (const node of nodes) {
      node.scrollIntoView({ block: 'center' });
      const box = node.getBoundingClientRect();
      if (box.width === 0 || box.height === 0) continue;
      const hit = document.elementFromPoint(box.left + box.width / 2, box.top + box.height / 2);
      const reachable = hit === node || node.contains(hit);
      if (!reachable) blocked.push(`${(node.textContent ?? '').slice(0, 24)} -> ${hit?.className ?? 'unknown'}`);
    }
    window.scrollTo(0, 0);
    return blocked;
  }, { selector, sample });
}

/** 页面是否出现横向滚动（DD-UI-01 不允许）。 */
export async function hasHorizontalScroll(page: Page): Promise<boolean> {
  return page.evaluate(() => document.documentElement.scrollWidth > window.innerWidth + 1);
}
