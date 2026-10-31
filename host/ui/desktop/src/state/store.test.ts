// DD-API-03 的确定性前置条件：切设备代际保护。
//
// 浏览器用例里，切页会 abort 旧代际的请求，所以"迟到的响应"通常根本到不了前端，
// 那个用例只能证明可观察结果，证明不了守卫本身。这里的 api 替身刻意忽略 abort，
// 让旧设备的响应真的送达，从而把判据压缩到唯一一处：generation 检查是否生效。

import { describe, expect, it, vi } from 'vitest';

type Deferred = { promise: Promise<unknown>; resolve: (value: unknown) => void };

const state = vi.hoisted(() => ({
  calls: [] as string[],
  pending: {} as Record<string, Deferred>,
}));

vi.mock('../api/client', () => ({
  ApiError: class MockApiError extends Error {
    readonly kind = 'network';
    readonly status = 0;
  },
  isAbortError: (error: unknown) => (error as { name?: string } | null)?.name === 'AbortError',
  setApiToken: () => {},
  api: {
    ready: async () => ({ status: 'ready' }),
    summary: async () => ({
      assets: [
        { asset_id: 'motor-01', device_id: 'dev-A', scene_instance: 'MOTOR_01', zone: 'B', detail_model: null, sensor_node: null, display_name_zh: '设备 A', binding_state: 'verified', status: 'ONLINE', last_telemetry_received_ms: 1 },
        { asset_id: 'motor-02', device_id: 'dev-B', scene_instance: 'MOTOR_02', zone: 'B', detail_model: null, sensor_node: null, display_name_zh: '设备 B', binding_state: 'verified', status: 'ONLINE', last_telemetry_received_ms: 1 },
      ],
      counts: { ONLINE: 2, STALE: 0, UNKNOWN: 0 },
      online_threshold_ms: 30_000,
    }),
    // 刻意忽略 signal：不模拟取消，只让旧响应迟到送达。
    snapshot: (deviceId: string) => {
      state.calls.push(deviceId);
      let resolve: (value: unknown) => void = () => {};
      const promise = new Promise<unknown>((res) => { resolve = res; });
      state.pending[deviceId] = { promise, resolve };
      return promise;
    },
    trends: async () => [],
    events: async () => [],
  },
}));

import { dashboard } from './store';

const tick = (): Promise<void> => new Promise((resolve) => { setTimeout(resolve, 0); });

function settle(deviceId: string, value: unknown): void {
  const entry = state.pending[deviceId];
  if (!entry) throw new Error(`设备 ${deviceId} 的请求尚未发出，前置条件不成立`);
  entry.resolve(value);
}

const snapshot = (deviceId: string, status: string, advice: string) => ({
  device_id: deviceId, status, snapshot_age_ms: 1000, advice, read_only: true,
});

describe('切设备代际保护', () => {
  it('迟到的旧设备响应即使送达，也不得覆盖新设备的页面结论', async () => {
    dashboard.setContext({ deviceId: 'dev-A', eventsScope: 'device' });
    await tick();
    expect(state.calls).toEqual(['dev-A']);

    // 在 A 仍在途时切到 B：新一轮刷新必须真正发出 B 的请求。
    dashboard.setContext({ deviceId: 'dev-B', eventsScope: 'device' });
    await tick();
    expect(state.calls).toEqual(['dev-A', 'dev-B']);

    // B 先返回。
    settle('dev-B', snapshot('dev-B', 'RECOVERED', '设备 B 已在服务端恢复'));
    await tick();
    expect(dashboard.getState().service).toBe('READY');
    expect(dashboard.getState().snapshot?.advice).toBe('设备 B 已在服务端恢复');

    // A 的响应随后才到，而且没有被取消。守卫必须把它丢掉。
    settle('dev-A', snapshot('dev-A', 'ALARM', '设备 A 严重报警'));
    await tick();

    expect(dashboard.getState().snapshot?.device_id).toBe('dev-B');
    expect(dashboard.getState().snapshot?.advice).toBe('设备 B 已在服务端恢复');
    expect(dashboard.getState().snapshot?.advice).not.toBe('设备 A 严重报警');
  });
});
