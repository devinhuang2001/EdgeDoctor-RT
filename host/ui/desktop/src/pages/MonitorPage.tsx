// 实时监测：设备模型、最新服务端状态、真实 RMS 趋势与最近事件。
// 首版只用已存在的 RMS、异常分数与趋势，不生成波形或频谱。

import { lazy, Suspense, useMemo, useState } from 'react';
import type { DashboardState } from '../state/store';
import type { EventRecord } from '../api/types';
import { DeviceSpecCard, SamplingCard } from '../components/assets';
import { TrendChart } from '../components/charts';
import { EventListPanel } from '../components/events';
import type { EventFilter } from '../components/events';
import { Banner, ErrorNotice, Metric, MetricGrid, Panel, PanelHead, Segmented, StateView, StatusBadge, StatusDot, DetailList, DetailRow } from '../components/primitives';
import { ageText, fixed, integer, PLACEHOLDER } from '../lib/format';
import { bindingView, snapshotView } from '../lib/status';
import { rmsSummary, windowByDeviceClock } from '../lib/trend';
import { useNow } from '../lib/useNow';

const MotorScene = lazy(() => import('../scene/MotorScene').then((module) => ({ default: module.MotorScene })));

type Window = '60s' | '300s' | 'all';

const WINDOWS: { value: Window; label: string; ms: number | null }[] = [
  { value: '60s', label: '最近 60 秒', ms: 60_000 },
  { value: '300s', label: '最近 5 分钟', ms: 300_000 },
  { value: 'all', label: '全部采样', ms: null },
];

export function MonitorPage({ state, onRefresh, onOpenEvents, onAck, onEvidenceRequest }: {
  state: DashboardState;
  onRefresh: () => void;
  onOpenEvents: () => void;
  onAck: (event: EventRecord) => void;
  onEvidenceRequest: (event: EventRecord) => void;
}) {
  const now = useNow();
  const [window, setWindow] = useState<Window>('all');
  const [filter, setFilter] = useState<EventFilter>('all');

  const asset = useMemo(() => {
    const assets = state.summary?.assets ?? [];
    return assets.find((item) => item.device_id === state.deviceId) ?? null;
  }, [state.summary, state.deviceId]);

  const windowMs = WINDOWS.find((item) => item.value === window)?.ms ?? null;
  const samples = useMemo(() => windowByDeviceClock(state.samples, windowMs), [state.samples, windowMs]);
  const summary = useMemo(() => rmsSummary(samples), [samples]);
  const view = snapshotView(state.snapshot?.status);
  const hasTelemetry = state.snapshot?.telemetry != null;

  if (!asset) {
    return (
      <Panel>
        <PanelHead eyebrow="Realtime Monitoring" title="未绑定设备" />
        <StateView tone="warning" title="该设备不在绑定配置中">
          {state.deviceId
            ? `路由中的设备 ${state.deviceId} 没有对应的已验证资产绑定，因此没有可展示的状态与趋势。`
            : '请在地址栏提供设备号，或从工厂总览选择一个已绑定设备。'}
        </StateView>
        <div className="btn-group" style={{ marginTop: 12 }}>
          <button className="btn primary" type="button" onClick={onOpenEvents}>前往事件中心</button>
        </div>
      </Panel>
    );
  }

  return (
    <div className="page-grid monitor-grid">
      <div className="stack">
        <Panel className="scene-panel">
          <PanelHead
            eyebrow="Device"
            title={asset.display_name_zh}
            meta={<span>{asset.asset_id} / {asset.device_id}</span>}
            actions={(
              <div className="btn-group">
                <StatusBadge tone={bindingView(asset.binding_state).tone} label={bindingView(asset.binding_state).label} />
                <button className="btn ghost" type="button" onClick={onRefresh}>刷新</button>
              </div>
            )}
          />
          <Suspense fallback={<StateView tone="loading" title="正在加载设备模型" />}>
            <MotorScene
              modelPath={asset.detail_model ?? 'models/induction_motor.glb'}
              sensorNode={asset.sensor_node}
              highlight={view.tone}
              label={asset.display_name_zh}
            />
          </Suspense>
          <Banner tone={view.tone} title={view.label}>{state.snapshot?.advice ?? view.hint}</Banner>

          <div style={{ marginTop: 14 }}>
            <MetricGrid columns={4}>
              <Metric
                label="当前 RMS"
                value={hasTelemetry ? fixed(state.snapshot?.telemetry?.rms_vector_g, 4) : PLACEHOLDER}
                unit={hasTelemetry ? 'g' : undefined}
                tone={hasTelemetry ? undefined : 'unknown'}
                hint="服务端最新遥测"
              />
              <Metric
                label="异常分数"
                value={hasTelemetry ? fixed(state.snapshot?.telemetry?.anomaly_score, 1) : PLACEHOLDER}
                tone={hasTelemetry ? undefined : 'unknown'}
                hint="服务端上报值"
              />
              <Metric label="窗口峰值" value={fixed(summary.peak, 4)} unit={summary.peak === null ? undefined : 'g'} tone={summary.peak === null ? 'unknown' : undefined} hint="窗口内 RMS 最大值" />
              <Metric label="有效采样" value={integer(summary.count)} hint="含数值的采样点" />
            </MetricGrid>
          </div>

          <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginTop: 16, gap: 12, flexWrap: 'wrap' }}>
            <div>
              <p className="eyebrow">RMS Trend</p>
              <h3 style={{ fontSize: 15 }}>RMS 振动趋势（单位 g）</h3>
            </div>
            <Segmented value={window} options={WINDOWS.map(({ value, label }) => ({ value, label }))} onChange={setWindow} label="趋势时间窗口" />
          </div>
          <TrendChart samples={samples} windowLabel={WINDOWS.find((item) => item.value === window)?.label} />

          {state.service === 'UNAVAILABLE' && state.serviceError && (
            <div style={{ marginTop: 12 }}>
              <ErrorNotice error={state.serviceError} onRetry={onRefresh} />
            </div>
          )}
          {state.service === 'UNAVAILABLE' && (
            <p className="panel-note">
              服务不可用时保留最近一次成功渲染的曲线，并保留数据时间
              {state.lastRefreshMs ? `（${new Date(state.lastRefreshMs).toLocaleTimeString('zh-CN')}）` : ''}。
            </p>
          )}
        </Panel>
      </div>

      <div className="stack">
        <Panel>
          <PanelHead
            eyebrow="Live Status"
            title="实时状态"
            actions={<StatusBadge tone={view.tone} label={view.label} />}
          />
          <DetailList>
            <DetailRow term="服务端状态">{view.label}</DetailRow>
            <DetailRow term="快照年龄">
              {state.snapshot?.snapshot_age_ms == null ? '—' : `${Math.round(state.snapshot.snapshot_age_ms / 1000)} 秒`}
            </DetailRow>
            <DetailRow term="最近遥测接收">{ageText(asset.last_telemetry_received_ms, now)}</DetailRow>
            {/* 静置期间设备照常上报，但没有基线偏离度，异常分数为空。
                这一行用来解释"异常分数为什么是 —"，而不是让操作员以为数据丢了。 */}
            <DetailRow term="采样状态">
              {state.snapshot?.telemetry?.idle == null
                ? '—'
                : state.snapshot.telemetry.idle ? '静置（无基准偏离度）' : '运转中'}
            </DetailRow>
            <DetailRow term="资产状态">
              <span style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
                <StatusDot tone={asset.status === 'ONLINE' ? 'ok' : asset.status === 'STALE' ? 'warning' : 'unknown'} label={asset.status} />
                {asset.status === 'ONLINE' ? '在线' : asset.status === 'STALE' ? '数据过期' : '未接入'}
              </span>
            </DetailRow>
            <DetailRow term="刷新通道">{state.streamStatus === 'open' ? 'SSE 实时推送' : '10 秒轮询'}</DetailRow>
          </DetailList>
          <p className="panel-note">
            状态与建议完全来自服务端 /api/v1/query/status；
            NO_DATA 时控制台不展示任何健康结论。
          </p>
        </Panel>

        <DeviceSpecCard asset={asset} nowMs={now} thresholdMs={state.summary?.online_threshold_ms ?? 30_000} streamConnected={state.streamStatus === 'open'} />
        <SamplingCard samples={samples} asset={asset} nowMs={now} />

        <EventListPanel
          events={state.events}
          scopedToDevice
          filter={filter}
          onFilterChange={setFilter}
          selectedEventId={state.selectedEventId}
          commandFor={(eventId) => state.commands[Object.keys(state.commands).filter((key) => state.commands[key].eventId === eventId)[0] ?? ''] ?? null}
          onOpen={() => onOpenEvents()}
          emptyHint="该设备尚无事件；设备上报偏离基线的快照后会出现在这里。"
        />
        <Panel>
          <PanelHead eyebrow="Actions" title="事件操作" />
          {state.events.length === 0
            ? <StateView title="暂无可确认事件">有活动事件时会在此提供确认与留证入口。</StateView>
            : (
              <div className="btn-group">
                {state.events.slice(0, 3).map((event) => (
                  <button
                    key={event.event_id}
                    className="btn secondary"
                    type="button"
                    disabled={event.ack === 'ACKNOWLEDGED'}
                    onClick={() => onAck(event)}
                    title={event.event_id}
                    aria-label={`确认事件 ${event.event_id}`}
                  >
                    {event.ack === 'ACKNOWLEDGED' ? '已确认' : `确认事件 · ${event.event_id.slice(0, 12)}${event.event_id.length > 12 ? '…' : ''}`}
                  </button>
                ))}
                <button className="btn secondary" type="button" onClick={() => state.events[0] && onEvidenceRequest(state.events[0])}>
                  为最新事件请求留证
                </button>
              </div>
            )}
          <p className="panel-note">确认与留证都会先进入持久命令队列；只有设备回执 APPLIED 才算已应用。</p>
        </Panel>
      </div>
    </div>
  );
}
