// DD-API-05 / DD-UI-05 的确定性前置条件：事件详情/证据/分析的落定，不得被后台刷新作废。
//
// 浏览器用例只能覆盖"某一次运行"的时序。实测复现（docs/desktop-dashboard-recheck-2026-09-26.md）：
// 详情请求在途时，一次 SSE 通知或 60 秒兜底轮询会推进 refresh() 的代际；详情响应随后才落定，
// 被当成过期结果丢弃，evidenceState 永久停在 'loading' —— "诊断证据"面板一直显示
// "正在读取证据元数据"，分析面板一直显示"正在查询分析接口"。
//
// 单元测试的价值在于把那个亚秒级竞态钉成确定性的：api 替身让详情请求停在在途状态，
// 强制在中间插入一次 refresh()，再放行响应。判据只有一条：状态必须落定。
//
// 注意：这里的 api.event/evidence/analysis 刻意不接 signal，因为生产代码也不传 signal——
// 详情请求本来就不会被 refresh() 的 abort 取消，能作废它的只有代际检查。

import { describe, expect, it, vi } from 'vitest';

const harness = vi.hoisted(() => {
  type Deferred = { promise: Promise<unknown>; resolve: (value: unknown) => void; reject: (reason: unknown) => void };
  const pending: Record<string, Deferred> = {};
  const calls: string[] = [];

  function defer(key: string): Promise<unknown> {
    let resolve: (value: unknown) => void = () => {};
    let reject: (reason: unknown) => void = () => {};
    const promise = new Promise<unknown>((res, rej) => { resolve = res; reject = rej; });
    pending[key] = { promise, resolve, reject };
    return promise;
  }

  return { pending, calls, defer };
});

vi.mock('../api/client', () => ({
  ApiError: class MockApiError extends Error {
    readonly kind: string;
    readonly status: number;
    constructor(kind = 'network', message = 'mock', status = 0) {
      super(message);
      this.kind = kind;
      this.status = status;
    }
  },
  isAbortError: (error: unknown) => (error as { name?: string } | null)?.name === 'AbortError',
  setApiToken: () => {},
  api: {
    ready: async () => { harness.calls.push('ready'); return { status: 'ready' }; },
    summary: async () => {
      harness.calls.push('summary');
      return { assets: [], counts: { ONLINE: 0, STALE: 0, UNKNOWN: 0 }, online_threshold_ms: 30_000 };
    },
    snapshot: async () => null,
    trends: async () => [],
    events: async () => [],
    event: (eventId: string) => harness.defer(`event:${eventId}`),
    evidence: (eventId: string) => harness.defer(`evidence:${eventId}`),
    analysis: (eventId: string) => harness.defer(`analysis:${eventId}`),
  },
}));

import { ApiError } from '../api/client';
import { dashboard } from './store';

const tick = (): Promise<void> => new Promise((resolve) => { setTimeout(resolve, 0); });

/** 提前触发一次后台刷新（等价于一次 SSE 通知或兜底轮询），并确认它真的发出去了。 */
async function backgroundRefresh(deviceId: string): Promise<void> {
  const before = harness.calls.filter((name) => name === 'ready').length;
  dashboard.setContext({ deviceId, eventsScope: 'device' });
  await tick();
  const after = harness.calls.filter((name) => name === 'ready').length;
  if (after <= before) throw new Error('后台刷新未发出，竞态前置条件不成立');
}

function settle(key: string, value: unknown): void {
  const entry = harness.pending[key];
  if (!entry) throw new Error(`${key} 请求尚未发出，前置条件不成立`);
  entry.resolve(value);
}

function fail(key: string, reason: unknown): void {
  const entry = harness.pending[key];
  if (!entry) throw new Error(`${key} 请求尚未发出，前置条件不成立`);
  entry.reject(reason);
}

const detailOf = (eventId: string) => ({ event_id: eventId, updated_ms: 1, revisions: [] });
const evidenceOf = (eventId: string, crcOk = true) => ({
  event_id: eventId, device_id: 'ed84-001', bytes: 256, crc_ok: crcOk, created_ms: 1,
  download: `/api/v1/evidence/${eventId}/download`,
});
const analysisOf = (eventId: string, available = false, reason = 'verified evidence analysis is not available') =>
  ({ event_id: eventId, available, reason });

/** 放行某个事件的三条详情请求；evidenceError 用于模拟 404/401。 */
function releaseDetail(eventId: string, options: { evidence?: unknown; evidenceError?: unknown; analysis?: unknown } = {}): void {
  settle(`event:${eventId}`, detailOf(eventId));
  if ('evidenceError' in options) fail(`evidence:${eventId}`, options.evidenceError);
  else settle(`evidence:${eventId}`, options.evidence ?? evidenceOf(eventId));
  settle(`analysis:${eventId}`, options.analysis ?? analysisOf(eventId));
}

describe('事件详情/证据/分析的落定', () => {
  it('切换事件时立即清除已加载的旧详情，避免操作提交给旧事件', async () => {
    dashboard.selectEvent('evt-loaded');
    await tick();
    releaseDetail('evt-loaded');
    await tick();
    expect(dashboard.getState().detail?.event_id).toBe('evt-loaded');
    dashboard.selectEvent('evt-next');
    expect(dashboard.getState().selectedEventId).toBe('evt-next');
    expect(dashboard.getState().detail).toBeNull();
    expect(dashboard.getState().evidence).toBeNull();
    expect(dashboard.getState().analysis).toBeNull();
    releaseDetail('evt-next');
    await tick();
    expect(dashboard.getState().detail?.event_id).toBe('evt-next');
  });
  it('详情在途时发生后台刷新，证据状态仍必须落定为 missing，不得永久停在 loading', async () => {
    dashboard.selectEvent(null);
    dashboard.selectEvent('evt-plain');
    await tick();
    expect(dashboard.getState().evidenceState, '选择后应进入读取中').toBe('loading');

    await backgroundRefresh('dev-A');

    // 详情响应在刷新之后才到。后台刷新表达的是"设备页面数据"过期，与按 event_id 取数的细节无关。
    releaseDetail('evt-plain', { evidenceError: new ApiError('not-found', '服务端没有该记录。', 404) });
    await tick();

    expect(dashboard.getState().evidenceState, '证据状态必须落定，不得停在读取中').toBe('missing');
  });

  it('详情在途时发生后台刷新，401 仍必须落定为 unauthorized', async () => {
    dashboard.selectEvent(null);
    dashboard.selectEvent('evt-401');
    await tick();

    await backgroundRefresh('dev-B');

    releaseDetail('evt-401', { evidenceError: new ApiError('auth', '服务端要求令牌。', 401) });
    await tick();

    expect(dashboard.getState().evidenceState).toBe('unauthorized');
  });

  it('分析结果必须落定，不得因后台刷新停在正在查询分析接口', async () => {
    dashboard.selectEvent(null);
    dashboard.selectEvent('evt-analysis');
    await tick();

    await backgroundRefresh('dev-C');

    releaseDetail('evt-analysis');
    await tick();

    expect(dashboard.getState().analysis?.reason).toBe('verified evidence analysis is not available');
    expect(dashboard.getState().analysisError).toBeNull();
  });

  it('更早一次详情请求的迟到响应不得覆盖新选中的事件', async () => {
    dashboard.selectEvent(null);
    dashboard.selectEvent('evt-a');
    await tick();
    dashboard.selectEvent('evt-b');
    await tick();

    // 新选择先落定。
    releaseDetail('evt-b', { evidence: evidenceOf('evt-b') });
    await tick();
    expect(dashboard.getState().evidenceState).toBe('available');
    expect(dashboard.getState().selectedEventId).toBe('evt-b');

    // 旧选择的响应迟到，必须被丢弃。
    releaseDetail('evt-a', { evidenceError: new ApiError('not-found', '服务端没有该记录。', 404) });
    await tick();

    expect(dashboard.getState().evidenceState, '旧选择的迟到响应不得覆盖新选择').toBe('available');
    expect(dashboard.getState().evidence?.event_id).toBe('evt-b');
  });

  it('清空选择会作废在途详情，响应回来不得把面板写回数据态', async () => {
    dashboard.selectEvent(null);
    dashboard.selectEvent('evt-cleared');
    await tick();

    dashboard.selectEvent(null);
    expect(dashboard.getState().evidenceState).toBe('idle');

    releaseDetail('evt-cleared', { evidence: evidenceOf('evt-cleared') });
    await tick();

    expect(dashboard.getState().evidenceState, '清空后不得被在途响应写回 available').toBe('idle');
    expect(dashboard.getState().evidence).toBeNull();
  });
});
