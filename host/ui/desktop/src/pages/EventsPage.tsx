// 事件中心：设备筛选、事件列表、服务端修订详情、真实趋势与留证状态。

import { useMemo, useState } from 'react';
import type { DashboardState } from '../state/store';
import type { EventRecord } from '../api/types';
import { TrendChart } from '../components/charts';
import { EventActions, EvidenceCard } from '../components/evidence';
import { EventLifecycle, EventListPanel, EventSummaryCard, lifecycleSteps } from '../components/events';
import type { EventFilter } from '../components/events';
import { ErrorNotice, Panel, PanelHead, Segmented, StateView } from '../components/primitives';
import { useDeviceSeries } from '../state/useDeviceSeries';
import { durationText, fixed } from '../lib/format';
import { rmsSummary } from '../lib/trend';

export function EventsPage({ state, onRefresh, onSelectEvent, onAck, onEvidenceRequest, onReloadEvidence }: {
  state: DashboardState;
  onRefresh: () => void;
  onSelectEvent: (eventId: string | null) => void;
  onAck: (event: EventRecord) => void;
  onEvidenceRequest: (event: EventRecord) => void;
  onReloadEvidence: (eventId: string) => void;
}) {
  const [filter, setFilter] = useState<EventFilter>('all');
  const [deviceFilter, setDeviceFilter] = useState<string>('all');

  const devices = useMemo(() => {
    const ids = new Set(state.events.map((event) => event.device_id));
    (state.summary?.assets ?? []).forEach((asset) => { if (asset.device_id) ids.add(asset.device_id); });
    return [...ids].sort();
  }, [state.events, state.summary]);

  const events = useMemo(
    () => (deviceFilter === 'all' ? state.events : state.events.filter((event) => event.device_id === deviceFilter)),
    [state.events, deviceFilter],
  );

  const selected = useMemo(
    () => events.find((event) => event.event_id === state.selectedEventId)
      ?? state.events.find((event) => event.event_id === state.selectedEventId)
      ?? null,
    [events, state.events, state.selectedEventId],
  );

  // 事件当前状态以最新修订为准；列表项可能是旧修订。
  const current = useMemo(() => {
    const revisions = state.detail?.revisions ?? [];
    return revisions.length > 0 ? revisions[revisions.length - 1] : selected;
  }, [state.detail, selected]);

  const command = current ? state.commands[Object.keys(state.commands).filter((key) => state.commands[key].eventId === current.event_id)[0] ?? ''] ?? null : null;
  const series = useDeviceSeries(current?.device_id ?? null);
  const summary = useMemo(() => rmsSummary(series.samples), [series.samples]);
  const steps = useMemo(() => lifecycleSteps(state.detail, command), [state.detail, command]);

  return (
    <div className="page-grid events-grid">
      <div className="stack">
        <Panel>
          <PanelHead eyebrow="Device Filter" title="设备筛选" meta={`${state.events.length} 条事件`} />
          {devices.length === 0
            ? <StateView title="没有可筛选设备">服务端尚未返回任何事件。</StateView>
            : (
              <Segmented
                value={deviceFilter}
                options={[{ value: 'all', label: `全部设备` }, ...devices.map((id) => ({ value: id, label: id }))]}
                onChange={setDeviceFilter}
                label="按设备筛选事件"
              />
            )}
          <p className="panel-note">
            筛选只在服务端返回的最近 {state.events.length} 条事件内进行，
            不代表全厂历史总量。
          </p>
        </Panel>
        <EventListPanel
          events={events}
          scopedToDevice={deviceFilter !== 'all'}
          filter={filter}
          onFilterChange={setFilter}
          selectedEventId={state.selectedEventId}
          commandFor={(eventId) => state.commands[Object.keys(state.commands).filter((key) => state.commands[key].eventId === eventId)[0] ?? ''] ?? null}
          onOpen={(event) => onSelectEvent(event.event_id)}
          emptyHint="服务端还没有事件记录。设备上报偏离基线的快照后会出现在这里。"
        />
      </div>

      <div className="stack">
        <EventSummaryCard event={current} detail={state.detail} command={command} />

        <Panel>
          <PanelHead
            eyebrow="Event Window"
            title="设备 RMS 趋势"
            meta={current ? `设备 ${current.device_id} · ${summary.count} 个有效采样` : undefined}
          />
          {!current
            ? <StateView title="未选择事件">选择事件后显示该设备的 RMS 趋势。</StateView>
            : series.loading
              ? <StateView tone="loading" title="正在读取趋势" />
              : series.error
                ? <ErrorNotice error={series.error} />
                : series.samples.length === 0
                  ? <StateView title="该设备暂无遥测采样">没有趋势可绘制；缺失采样不会被补成 0。</StateView>
                  : (
                    <>
                      <TrendChart samples={series.samples} windowLabel={`设备 ${current.device_id} 全部采样`} />
                      <p className="panel-note">
                        横轴是设备单调时钟（mono_ms），与事件的墙钟接收时间不同源，因此不在图上叠加事件标记。
                        红色点为服务端 alarm 字段为真的采样。
                        {summary.peak !== null && ` 窗口峰值 ${fixed(summary.peak, 4)} g，窗口跨度 ${durationText(summary.windowMs)}。`}
                      </p>
                    </>
                  )}
        </Panel>

        <Panel>
          <PanelHead eyebrow="Event Description" title="事件描述与操作" />
          {!current
            ? <StateView title="未选择事件">选择事件后提供确认与留证操作。</StateView>
            : (
              <>
                <p className="muted" style={{ fontSize: 13 }}>
                  设备 {current.device_id} 在服务端时间
                  {state.detail?.updated_ms ? ` ${new Date(state.detail.updated_ms).toLocaleString('zh-CN')}` : '（未提供）'}
                  {' '}记录为{current.condition === 'ACTIVE' ? '活动' : '已恢复'}报警，
                  {current.peak_deviation === undefined ? '未携带基线偏离分数' : `服务端上报基线偏离分数 ${current.peak_deviation.toFixed(4)}（无量纲）`}。
                  诊断字段为服务端的分类结果，控制台不做部件级故障定位。
                </p>
                <div style={{ marginTop: 12 }}>
                  <EventActions
                    event={current}
                    command={command}
                    disabled={state.service === 'UNAVAILABLE'}
                    onAck={() => onAck(current)}
                    onRequestEvidence={() => onEvidenceRequest(current)}
                  />
                </div>
                {state.service === 'UNAVAILABLE' && (
                  <div style={{ marginTop: 10 }}>
                    <ErrorNotice error={state.serviceError} onRetry={onRefresh} />
                  </div>
                )}
              </>
            )}
        </Panel>
      </div>

      <div className="stack events-detail-column">
        <EvidenceCard
          evidence={state.evidence}
          state={state.evidenceState}
          error={state.evidenceError}
          command={command}
          onRequest={() => current && onEvidenceRequest(current)}
          onReload={() => current && onReloadEvidence(current.event_id)}
        />
        <EventLifecycle steps={steps} />
      </div>
    </div>
  );
}
