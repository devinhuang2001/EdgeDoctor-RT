// 事件中心的列表、筛选与生命周期。所有内容来自服务端事件修订，不在客户端合成新版本。

import type { EventDetail, EventRecord } from '../api/types';
import type { CommandState } from '../state/store';
import { dateTimeText, clockText, diagnosisText, qualityText, sourceText } from '../lib/format';
import { commandView, eventView } from '../lib/status';
import { Badge, DetailList, DetailRow, Panel, PanelHead, StateView, StatusBadge } from './primitives';

export type EventFilter = 'pending' | 'all' | 'recovered';

export function filterEvents(events: EventRecord[], filter: EventFilter): EventRecord[] {
  if (filter === 'pending') return events.filter((event) => event.condition === 'ACTIVE' && event.ack !== 'ACKNOWLEDGED');
  if (filter === 'recovered') return events.filter((event) => event.condition === 'RECOVERED');
  return events;
}

export function EventFilters({ events, filter, onChange }: { events: EventRecord[]; filter: EventFilter; onChange: (filter: EventFilter) => void }) {
  const options: { value: EventFilter; label: string }[] = [
    { value: 'pending', label: `待处理 ${filterEvents(events, 'pending').length}` },
    { value: 'all', label: `全部 ${events.length}` },
    { value: 'recovered', label: `已恢复 ${filterEvents(events, 'recovered').length}` },
  ];
  return (
    <div className="filter-row" role="group" aria-label="事件筛选">
      {options.map((option) => (
        <button key={option.value} type="button" aria-pressed={filter === option.value} onClick={() => onChange(option.value)}>
          {option.label}
        </button>
      ))}
    </div>
  );
}

export function EventCard({ event, selected, command, onOpen }: {
  event: EventRecord;
  selected: boolean;
  command: CommandState | null;
  onOpen: (event: EventRecord) => void;
}) {
  const view = eventView(event);
  const commandBadge = command ? commandView(command.status) : null;
  return (
    <button type="button" className="event-card" aria-current={selected} onClick={() => onOpen(event)}>
      <span className={`event-icon ${view.tone}`} aria-hidden="true">
        {event.condition === 'RECOVERED' ? '✓' : event.severity === 'ALARM' ? '✕' : '!'}
      </span>
      <span>
        <h3>{view.title}</h3>
        <p className="event-meta">
          {event.device_id} · {dateTimeText(event.updated_ms ?? event.server_received_ms)} · rev {event.revision}
          {event.peak_deviation !== undefined && ` · 偏离分数 ${event.peak_deviation.toFixed(3)}（无量纲）`}
        </p>
        <p className="event-id">{event.event_id}</p>
      </span>
      <span className="event-right">
        <StatusBadge tone={view.tone} label={event.ack === 'ACKNOWLEDGED' ? '已确认' : view.severityLabel} />
        {commandBadge && <Badge tone={commandBadge.tone}>{commandBadge.label}</Badge>}
      </span>
    </button>
  );
}

export function EventListPanel({ events, scopedToDevice, filter, onFilterChange, selectedEventId, commandFor, onOpen, emptyHint }: {
  events: EventRecord[];
  scopedToDevice: boolean;
  filter: EventFilter;
  onFilterChange: (filter: EventFilter) => void;
  selectedEventId: string | null;
  commandFor: (eventId: string) => CommandState | null;
  onOpen: (event: EventRecord) => void;
  emptyHint?: string;
}) {
  const visible = filterEvents(events, filter);
  return (
    <Panel className="events-panel">
      <PanelHead
        eyebrow="事件中心"
        title="实时事件管理"
        meta={scopedToDevice ? '当前设备 · 最近 50 条' : '全部设备 · 最近 50 条'}
      />
      <EventFilters events={events} filter={filter} onChange={onFilterChange} />
      {events.length === 0
        ? <StateView title="暂无事件">{emptyHint ?? '设备上报偏离基线的快照后，事件会出现在这里。'}</StateView>
        : visible.length === 0
          ? <StateView title="当前筛选下没有事件">请切换筛选条件查看全部事件。</StateView>
          : (
            <div className="event-list">
              {visible.map((event) => (
                <EventCard
                  key={`${event.event_id}-${event.revision}`}
                  event={event}
                  selected={event.event_id === selectedEventId}
                  command={commandFor(event.event_id)}
                  onOpen={onOpen}
                />
              ))}
            </div>
          )}
      <p className="panel-note">
        列表为服务端排序的最近 50 条{scopedToDevice ? '（当前设备）' : '（全部设备）'}，
        不代表全厂历史总量。按修订号区分同一事件的不同版本。
      </p>
    </Panel>
  );
}

export type LifecycleStep = {
  key: string;
  title: string;
  note: string;
  state: 'done' | 'active' | 'pending';
  time: string | null;
};

/** 生命周期只由服务端修订与命令回执推导，缺少时间戳的步骤不显示时间。 */
export function lifecycleSteps(detail: EventDetail | null, command: CommandState | null): LifecycleStep[] {
  const revisions = detail?.revisions ?? [];
  const first = revisions[0];
  const recovered = revisions.find((revision) => revision.condition === 'RECOVERED');
  const acknowledged = revisions.find((revision) => revision.ack === 'ACKNOWLEDGED');
  const steps: LifecycleStep[] = [
    {
      key: 'detected',
      title: '检测到',
      note: first ? `设备上报 ${sourceText(first.source)} 快照，服务端已持久化修订 ${first.revision}。` : '等待设备上报诊断快照。',
      state: first ? 'done' : 'pending',
      time: first?.server_received_ms ? clockText(first.server_received_ms) : null,
    },
    {
      key: 'ack',
      title: '已确认',
      note: acknowledged
        ? `人工确认已记录在修订 ${acknowledged.revision}。`
        : command
          ? `确认请求${commandView(command.status).label}。`
          : '等待人工确认；确认请求只有在设备回执 APPLIED 后才算已应用。',
      state: acknowledged ? 'done' : command ? 'active' : 'pending',
      time: acknowledged?.server_received_ms ? clockText(acknowledged.server_received_ms) : null,
    },
    {
      key: 'recovered',
      title: '已恢复',
      note: recovered
        ? `设备上报恢复，服务端修订 ${recovered.revision}。`
        : '设备尚未上报恢复；恢复由板端判定，控制台不推断。',
      state: recovered ? 'done' : 'pending',
      time: recovered?.server_received_ms ? clockText(recovered.server_received_ms) : null,
    },
  ];
  if (command && command.kind === 'evidence') {
    const view = commandView(command.status);
    steps.push({
      key: 'evidence',
      title: '留证请求',
      note: view.hint,
      state: command.status === 'APPLIED' ? 'done' : command.status === 'PENDING' ? 'active' : 'pending',
      time: clockText(command.submittedMs),
    });
  }
  return steps;
}

export function EventLifecycle({ steps }: { steps: LifecycleStep[] }) {
  return (
    <Panel>
      <PanelHead eyebrow="Event Lifecycle" title="事件生命周期" meta="按服务端修订推导" />
      <ol className="timeline" style={{ listStyle: 'none', padding: 0, margin: 0 }}>
        {steps.map((step) => (
          <li className={`timeline-step ${step.state}`} key={step.key}>
            <span className="timeline-dot" aria-hidden="true">{step.state === 'done' ? '✓' : step.state === 'active' ? '⋯' : '·'}</span>
            <div className="timeline-body">
              <span className="timeline-title">
                {step.title}
                {step.state === 'active' && <Badge tone="warning">进行中</Badge>}
                {step.state === 'pending' && <Badge tone="unknown">待发生</Badge>}
              </span>
              {step.time && <span className="timeline-time">{step.time}</span>}
              <span className="timeline-note">{step.note}</span>
            </div>
          </li>
        ))}
      </ol>
    </Panel>
  );
}

export function EventSummaryCard({ event, detail, command }: { event: EventRecord | null; detail: EventDetail | null; command: CommandState | null }) {
  if (!event) {
    return (
      <Panel>
        <PanelHead eyebrow="事件详情" title="未选择事件" />
        <StateView title="请选择一条事件">左侧列表中的任一事件都可查看其服务端修订、诊断字段与留证状态。</StateView>
      </Panel>
    );
  }
  const view = eventView(event);
  const commandBadge = command ? commandView(command.status) : null;
  return (
    <Panel>
      <PanelHead
        eyebrow={`事件 ${event.event_id}`}
        title={view.title}
        actions={<div className="btn-group"><StatusBadge tone={view.tone} label={view.severityLabel} /><StatusBadge tone={event.ack === 'ACKNOWLEDGED' ? 'ok' : 'warning'} label={view.ackLabel} />{commandBadge && <Badge tone={commandBadge.tone}>{commandBadge.label}</Badge>}</div>}
      />
      <DetailList>
        <DetailRow term="设备">{event.device_id}</DetailRow>
        <DetailRow term="数据来源">{sourceText(event.source)}</DetailRow>
        <DetailRow term="服务端修订数">{detail ? detail.revisions.length : '—'}</DetailRow>
        <DetailRow term="最近更新">{dateTimeText(detail?.updated_ms ?? event.updated_ms)}</DetailRow>
        <DetailRow term="服务端诊断字段">{diagnosisText(event.diagnosis)}</DetailRow>
        <DetailRow term="信号质量">{qualityText(event.quality)}</DetailRow>
          <DetailRow term="基线偏离分数（无量纲）">
            {event.peak_deviation === undefined ? '—' : event.peak_deviation.toFixed(4)}
        </DetailRow>
      </DetailList>
      <p className="panel-note">
        “{diagnosisText(event.diagnosis)}” 是服务端给出的分类字段，不是控制台推断的故障结论；
        {event.peak_deviation === undefined
          ? '当前事件未携带基线偏离分数。'
          : '基线偏离分数为服务端上报值，单位为无量纲。'}
      </p>
    </Panel>
  );
}
