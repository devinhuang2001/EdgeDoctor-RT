// 状态到文案/色调的映射。颜色只是辅助，文字与图标必须表达同一状态（规格 §4）。

import type { AssetStatus, BindingState, CommandStatus, EventRecord, SnapshotStatus } from '../api/types';

export type Tone = 'ok' | 'warning' | 'critical' | 'unknown' | 'info';

/** 设备类状态不使用 info，便于直接传给三维高亮等只接受四种色调的组件。 */
export type SignalTone = 'ok' | 'warning' | 'critical' | 'unknown';

export type StatusView = { label: string; tone: Tone; hint: string };
export type SignalStatusView = { label: string; tone: SignalTone; hint: string };

const SNAPSHOT: Record<SnapshotStatus, SignalStatusView> = {
  ALARM: { label: '严重报警', tone: 'critical', hint: '服务端判定存在严重偏离基线的活动报警。' },
  WARNING: { label: '预警', tone: 'warning', hint: '服务端判定存在活动报警，严重度未达严重级别。' },
  RECOVERED: { label: '已恢复', tone: 'ok', hint: '最近事件已恢复；若仍未确认，请人工确认后归档。' },
  NO_DATA: { label: '等待设备数据', tone: 'unknown', hint: '服务端尚无该设备的诊断快照，不展示健康结论。' },
  UNKNOWN: { label: '状态未知', tone: 'unknown', hint: '服务端无法确定当前状态，请检查信号质量与基线。' },
};

/** 快照状态文案。传入 null（尚未取到数据）也归入“状态未知”。 */
export const snapshotView = (status: SnapshotStatus | null | undefined): SignalStatusView => (
  status ? SNAPSHOT[status] : { label: '状态未知', tone: 'unknown', hint: '尚未读取到服务端状态。' }
);

const ASSET: Record<AssetStatus, SignalStatusView> = {
  ONLINE: { label: '在线', tone: 'ok', hint: '服务端在阈值内收到有效遥测。' },
  STALE: { label: '数据过期', tone: 'warning', hint: '超过阈值未收到遥测，最后数据不代表当前状态。' },
  UNKNOWN: { label: '未接入', tone: 'unknown', hint: '没有可用绑定或从未收到遥测，不计入在线统计。' },
};

export const assetView = (status: AssetStatus): SignalStatusView => ASSET[status];

export const bindingView = (state: BindingState): SignalStatusView => (
  state === 'verified' ? { label: '已验证绑定', tone: 'ok', hint: '资产、设备与场景节点已核验。' }
    : state === 'provisional' ? { label: '待核验绑定', tone: 'warning', hint: '绑定关系尚未核验，不显示为在线设备。' }
      : { label: '未接入设备', tone: 'unknown', hint: '模型仅用于浏览，未绑定任何设备。' }
);

export const commandView = (status: CommandStatus): StatusView => (
  status === 'APPLIED' ? { label: '设备已应用', tone: 'ok', hint: '设备回执为已应用。' }
    : status === 'PENDING' ? { label: '等待设备回执', tone: 'info', hint: '命令已进入持久队列，尚未收到设备最终回执。' }
      : status === 'REJECTED' ? { label: '设备拒绝', tone: 'critical', hint: '设备拒绝了该命令，可检查设备状态后重试。' }
        : { label: '命令已过期', tone: 'warning', hint: '命令在设备取走前已过期，请重新提交。' }
);

export type EventView = { title: string; tone: Tone; severityLabel: string; ackLabel: string };

/** 事件标题只描述服务端给出的观测事实，不给出故障类别。 */
export function eventView(event: Pick<EventRecord, 'condition' | 'ack' | 'severity'>): EventView {
  const active = event.condition === 'ACTIVE';
  return {
    title: active ? '振动偏离基线' : '报警已恢复',
    tone: !active ? 'ok' : event.severity === 'ALARM' ? 'critical' : 'warning',
    severityLabel: event.severity === 'ALARM' ? '严重' : '预警',
    ackLabel: event.ack === 'ACKNOWLEDGED' ? '已确认' : '待确认',
  };
}

export const serviceView = (state: 'LOADING' | 'READY' | 'UNAVAILABLE'): StatusView => (
  state === 'READY' ? { label: 'LAN 服务已就绪', tone: 'ok', hint: '状态、趋势与事件接口可用。' }
    : state === 'LOADING' ? { label: '正在连接服务', tone: 'info', hint: '正在读取服务端状态。' }
      : { label: '服务不可用', tone: 'critical', hint: '已渲染的历史数据保留在页面上，但不再刷新。' }
);

/** 数据新鲜度：服务端只提供最后接收时间，前端据此标注，不推断在线。 */
export function freshnessView(lastReceivedMs: number | null | undefined, nowMs: number, thresholdMs: number): SignalStatusView {
  if (lastReceivedMs === null || lastReceivedMs === undefined) {
    return { label: '暂无遥测', tone: 'unknown', hint: '服务端没有该设备的遥测接收记录。' };
  }
  const age = Math.max(0, nowMs - lastReceivedMs);
  return age <= thresholdMs
    ? { label: '数据新鲜', tone: 'ok', hint: `最后一次遥测在 ${Math.round(thresholdMs / 1000)} 秒阈值内。` }
    : { label: '数据过期', tone: 'warning', hint: `距最后一次遥测已超过 ${Math.round(thresholdMs / 1000)} 秒阈值。` };
}
