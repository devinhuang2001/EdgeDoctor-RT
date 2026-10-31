// 运行时状态仓（规格 §3 的“运行时状态仓”一层）。
// 承担三件事：请求代际防串页、命令回执跟踪、错误分类兜底。
// 页面不得自行推断报警、在线或诊断结论，只能渲染这里保存的服务端事实。

import { useSyncExternalStore } from 'react';
import { ApiError, api, isAbortError, setApiToken } from '../api/client';
import type {
  ActionKind, AnalysisView, CommandStatus, EventDetail, EventRecord, EvidenceMetadata,
  FactorySummary, Snapshot, Telemetry,
} from '../api/types';

export type ServiceState = 'LOADING' | 'READY' | 'UNAVAILABLE';
export type DataSource = 'INITIAL' | 'SSE' | 'POLLING' | 'MANUAL' | 'ACTION';
export type EvidenceState = 'idle' | 'loading' | 'missing' | 'unauthorized' | 'invalid' | 'available' | 'error';
export type ToastTone = 'success' | 'error' | 'info' | 'warning';

export type Toast = { id: number; tone: ToastTone; text: string };

export type CommandState = {
  eventId: string;
  kind: ActionKind;
  commandId: string;
  status: CommandStatus;
  detail?: string;
  submittedMs: number;
};

export type RouteContext = { deviceId: string | null; eventsScope: 'device' | 'all' };

export type DashboardState = {
  service: ServiceState;
  serviceError: ApiError | null;
  token: string;
  summary: FactorySummary | null;
  selectedAssetId: string | null;
  zoneFilter: string | null;
  deviceId: string | null;
  snapshot: Snapshot | null;
  samples: Telemetry[];
  events: EventRecord[];
  eventsScope: 'device' | 'all';
  selectedEventId: string | null;
  detail: EventDetail | null;
  evidence: EvidenceMetadata | null;
  evidenceState: EvidenceState;
  evidenceError: ApiError | null;
  analysis: AnalysisView | null;
  analysisError: ApiError | null;
  commands: Record<string, CommandState>;
  refreshing: boolean;
  lastRefreshMs: number | null;
  lastSource: DataSource;
  streamStatus: 'connecting' | 'open' | 'closed' | 'unsupported';
  toasts: Toast[];
};

const initialState: DashboardState = {
  service: 'LOADING',
  serviceError: null,
  token: '',
  summary: null,
  selectedAssetId: null,
  zoneFilter: null,
  deviceId: null,
  snapshot: null,
  samples: [],
  events: [],
  eventsScope: 'device',
  selectedEventId: null,
  detail: null,
  evidence: null,
  evidenceState: 'idle',
  evidenceError: null,
  analysis: null,
  analysisError: null,
  commands: {},
  refreshing: false,
  lastRefreshMs: null,
  lastSource: 'INITIAL',
  streamStatus: 'connecting',
  toasts: [],
};

const COMMAND_POLL_MS = 1500;
const COMMAND_POLL_LIMIT = 40;

class DashboardStore {
  private state: DashboardState = initialState;
  private listeners = new Set<() => void>();
  private context: RouteContext = { deviceId: null, eventsScope: 'device' };
  private generation = 0;
  // 事件详情/证据/分析的代际，与 refresh() 的 generation 刻意分开。
  // generation 表达的是"设备页面数据"的代际，由后台刷新推进；详情按 event_id 取数，
  // 不受设备刷新影响，若与 generation 共用就会出现下面的问题：
  // 用户选中事件后详情请求还在飞，此时一次 SSE 通知或兜底轮询推进了 generation，
  // 已完成的详情被当作过期结果丢弃，evidenceState 永久停在 'loading'，
  // 面板一直显示"正在读取证据元数据"。因此这里只由"更新的详情请求"或"清空选择"推进。
  private detailSeq = 0;
  private controller: AbortController | null = null;
  private pollers = new Map<string, number>();
  private toastSeq = 0;

  // ---------- React 绑定 ----------
  subscribe = (listener: () => void): (() => void) => {
    this.listeners.add(listener);
    return () => { this.listeners.delete(listener); };
  };

  getState = (): DashboardState => this.state;

  private patch(partial: Partial<DashboardState>): void {
    this.state = { ...this.state, ...partial };
    this.listeners.forEach((listener) => listener());
  }

  // ---------- 提示 ----------
  toast(tone: ToastTone, text: string): void {
    const id = ++this.toastSeq;
    this.patch({ toasts: [...this.state.toasts, { id, tone, text }].slice(-4) });
    window.setTimeout(() => this.dismissToast(id), tone === 'error' ? 12_000 : 7000);
  }

  dismissToast(id: number): void {
    this.patch({ toasts: this.state.toasts.filter((toast) => toast.id !== id) });
  }

  // ---------- 令牌 ----------
  setToken(token: string): void {
    setApiToken(token);
    this.patch({ token });
    void this.refresh('MANUAL');
  }

  // ---------- 路由上下文 ----------
  setContext(context: RouteContext): void {
    const changed = context.deviceId !== this.context.deviceId || context.eventsScope !== this.context.eventsScope;
    this.context = context;
    if (changed) {
      // 切换设备时先丢弃旧设备数据，避免新页面闪现旧设备的结论。
      this.patch({ deviceId: context.deviceId, eventsScope: context.eventsScope, snapshot: null, samples: [], events: [] });
      void this.refresh('MANUAL');
    }
  }

  selectAsset(assetId: string): void {
    this.patch({ selectedAssetId: assetId, zoneFilter: this.state.zoneFilter });
  }

  setZoneFilter(zone: string | null): void {
    this.patch({ zoneFilter: zone });
  }

  setStreamStatus(status: DashboardState['streamStatus']): void {
    if (status !== this.state.streamStatus) this.patch({ streamStatus: status });
  }

  // ---------- 主刷新 ----------
  async refresh(source: DataSource): Promise<void> {
    const generation = ++this.generation;
    this.controller?.abort();
    const controller = new AbortController();
    this.controller = controller;
    this.patch({ refreshing: true });

    const { deviceId, eventsScope } = this.context;
    try {
      const ready = await api.ready(controller.signal);
      if (generation !== this.generation) return;
      const summary = await api.summary(controller.signal);
      if (generation !== this.generation) return;

      const [snapshot, samples, events] = deviceId
        ? await Promise.all([
          api.snapshot(deviceId, controller.signal),
          api.trends(deviceId, 2000, controller.signal).catch((error) => { if (isAbortError(error)) throw error; return [] as Telemetry[]; }),
          api.events(eventsScope === 'all' ? undefined : deviceId, 50, controller.signal),
        ])
        : [null, [] as Telemetry[], await api.events(undefined, 50, controller.signal)];

      if (generation !== this.generation) return;
      const selected = this.state.selectedAssetId
        ?? summary.assets.find((asset) => asset.device_id === deviceId)?.asset_id
        ?? summary.assets[0]?.asset_id
        ?? null;
      this.patch({
        service: ready.status === 'ready' ? 'READY' : 'UNAVAILABLE',
        serviceError: null,
        summary,
        selectedAssetId: selected,
        snapshot,
        samples,
        events,
        refreshing: false,
        lastRefreshMs: Date.now(),
        lastSource: source,
      });
    } catch (cause) {
      if (isAbortError(cause)) return;
      if (generation !== this.generation) return;
      const error = cause instanceof ApiError ? cause : new ApiError('network', '无法读取服务端数据');
      // 服务不可用不清空已渲染的历史数据，只标记数据时间（规格 §5.1）。
      this.patch({ service: 'UNAVAILABLE', serviceError: error, refreshing: false });
    }
  }

  // ---------- 事件详情 / 证据 / 分析 ----------
  selectEvent(eventId: string | null): void {
    // A new selection must never expose the previous event's actions or evidence
    // while its own context is still loading.
    this.patch({ selectedEventId: eventId, detail: null, evidence: null, evidenceState: 'idle', evidenceError: null, analysis: null, analysisError: null });
    if (!eventId) {
      // 清空选择会让在飞的详情请求作废，避免它回来后又把面板写回数据态。
      this.detailSeq += 1;
      this.patch({ detail: null, evidence: null, evidenceState: 'idle', evidenceError: null, analysis: null, analysisError: null });
      return;
    }
    void this.loadEventContext(eventId);
  }

  async loadEventContext(eventId: string): Promise<void> {
    const seq = ++this.detailSeq;
    this.patch({ evidenceState: 'loading', evidenceError: null, analysisError: null });
    const [detail, evidence, analysis] = await Promise.allSettled([
      api.event(eventId),
      api.evidence(eventId),
      api.analysis(eventId),
    ]);
    // 只有"更新的详情请求"才能作废本次结果；后台刷新不得作废，否则会留下永久的读取中。
    if (seq !== this.detailSeq) return;
    if (this.state.selectedEventId !== eventId) return;

    if (detail.status === 'fulfilled') this.patch({ detail: detail.value });
    else if (!isAbortError(detail.reason)) this.patch({ detail: null });

    if (evidence.status === 'fulfilled') {
      const metadata = evidence.value;
      this.patch({
        evidence: metadata,
        evidenceState: metadata.crc_ok ? 'available' : 'invalid',
        evidenceError: null,
      });
    } else {
      const error = evidence.reason instanceof ApiError ? evidence.reason : null;
      const state: EvidenceState = error?.kind === 'not-found' ? 'missing'
        : error?.kind === 'auth' || error?.kind === 'forbidden' ? 'unauthorized'
          : isAbortError(evidence.reason) ? 'idle' : 'error';
      this.patch({ evidence: null, evidenceState: state, evidenceError: error });
    }

    if (analysis.status === 'fulfilled') this.patch({ analysis: analysis.value, analysisError: null });
    else {
      const error = analysis.reason instanceof ApiError ? analysis.reason : null;
      this.patch({ analysis: null, analysisError: isAbortError(analysis.reason) ? null : error });
    }
  }

  // ---------- 确认 / 留证 ----------
  async submit(event: EventRecord, kind: ActionKind): Promise<void> {
    try {
      const result = kind === 'ack'
        ? await api.acknowledge(event.event_id, event.revision, 'desktop-dashboard')
        : await api.requestEvidence(event.event_id);
      const command: CommandState = {
        eventId: event.event_id,
        kind,
        commandId: result.command_id,
        status: 'PENDING',
        submittedMs: Date.now(),
      };
      this.patch({ commands: { ...this.state.commands, [result.command_id]: command } });
      this.toast('info', kind === 'ack'
        ? '确认请求已提交，等待设备回执。收到 APPLIED 前不显示已应用。'
        : '留证请求已提交，等待设备回执。');
      this.trackCommand(command);
    } catch (cause) {
      const error = cause instanceof ApiError ? cause : new ApiError('network', '请求失败');
      this.toast('error', error.message);
    }
  }

  private trackCommand(command: CommandState): void {
    let attempts = 0;
    const tick = async () => {
      attempts += 1;
      try {
        const view = await api.command(command.commandId);
        const current = this.state.commands[command.commandId];
        if (!current) return;
        if (view.status !== current.status) {
          this.patch({ commands: { ...this.state.commands, [command.commandId]: { ...current, status: view.status, detail: view.detail } } });
        }
        if (view.status !== 'PENDING') {
          window.clearInterval(timer);
          this.pollers.delete(command.commandId);
          if (view.status === 'APPLIED') {
            this.toast('success', command.kind === 'ack' ? '设备已应用确认。' : '设备已应用留证请求。');
            if (command.kind === 'evidence') await this.loadEventContext(command.eventId);
            await this.refresh('ACTION');
          } else if (view.status === 'REJECTED') {
            this.toast('error', `设备拒绝该请求（REJECTED）。事件状态未改变，可在设备就绪后重试。`);
          } else {
            this.toast('warning', '命令已过期（EXPIRED），未送达设备。请重新提交。');
          }
          return;
        }
      } catch (cause) {
        if (!(cause instanceof ApiError) || cause.kind !== 'not-found') {
          window.clearInterval(timer);
          this.pollers.delete(command.commandId);
          return;
        }
      }
      if (attempts >= COMMAND_POLL_LIMIT) {
        window.clearInterval(timer);
        this.pollers.delete(command.commandId);
        this.toast('warning', '等待设备回执超时，命令仍在持久队列中。可稍后刷新查看。');
      }
    };
    const timer = window.setInterval(() => { void tick(); }, COMMAND_POLL_MS);
    this.pollers.set(command.commandId, timer);
  }

  /** 事件最近一次命令，用于在事件卡上显示 待回执 / 已应用 / 拒绝。 */
  commandForEvent(eventId: string): CommandState | null {
    const entries = Object.values(this.state.commands).filter((command) => command.eventId === eventId);
    if (entries.length === 0) return null;
    return entries.sort((left, right) => right.submittedMs - left.submittedMs)[0];
  }

  dispose(): void {
    this.controller?.abort();
    this.pollers.forEach((timer) => window.clearInterval(timer));
    this.pollers.clear();
  }
}

export const dashboard = new DashboardStore();

export function useDashboard(): DashboardState {
  return useSyncExternalStore(dashboard.subscribe, dashboard.getState, dashboard.getState);
}
