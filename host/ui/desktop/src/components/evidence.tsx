// 留证与证据状态。只有服务端存在元数据且 CRC 通过时才允许下载（规格 §5.1）。

import type { AnalysisView, EvidenceMetadata, EventRecord } from '../api/types';
import type { CommandState, EvidenceState } from '../state/store';
import type { ApiError } from '../api/client';
import { bytesText, dateTimeText } from '../lib/format';
import { commandView } from '../lib/status';
import { Badge, DetailList, DetailRow, ErrorNotice, Panel, PanelHead, StateView } from './primitives';

export function EvidenceCard({ evidence, state, error, command, onRequest, onReload }: {
  evidence: EvidenceMetadata | null;
  state: EvidenceState;
  error: ApiError | null;
  command: CommandState | null;
  onRequest: () => void;
  onReload: () => void;
}) {
  const pending = command?.kind === 'evidence' && command.status === 'PENDING';

  return (
    <Panel>
      <PanelHead
        eyebrow="Diagnostic Evidence"
        title="诊断证据"
        meta={pending ? <Badge tone="info">等待设备回执</Badge> : undefined}
      />

      {state === 'loading' && <StateView tone="loading" title="正在读取证据元数据" />}

      {state === 'idle' && (
        <StateView title="尚未选择事件">选择事件后显示其 EDV2 留证状态。</StateView>
      )}

      {state === 'missing' && (
        <>
          <StateView title="尚无留证记录">
            服务端没有该事件的证据元数据。请求留证后，需等设备上报并经服务端校验，才会出现可下载的原始数据。
          </StateView>
          <div className="btn-group" style={{ marginTop: 12 }}>
            <button className="btn primary" type="button" disabled={pending} onClick={onRequest}>
              {pending ? '留证请求待回执' : '请求留证'}
            </button>
          </div>
        </>
      )}

      {state === 'unauthorized' && (
        <StateView tone="warning" title="认证被拒绝">
          服务端要求 API 令牌才能读取证据元数据。请在顶栏输入令牌后重试。
        </StateView>
      )}

      {state === 'invalid' && (
        <StateView tone="warning" title="证据校验未通过">
          服务端记录了该事件的证据，但 CRC/框架校验未通过，因此不能视为可用的原始数据，也不提供下载。
        </StateView>
      )}

      {state === 'error' && <ErrorNotice error={error} onRetry={onReload} />}

      {state === 'available' && evidence && (
        <>
          <DetailList>
            <DetailRow term="数据来源设备">{evidence.device_id}</DetailRow>
            <DetailRow term="文件大小">{bytesText(evidence.bytes)}</DetailRow>
            <DetailRow term="CRC 校验">通过（服务端已解包验证）</DetailRow>
            <DetailRow term="服务端记录时间">{dateTimeText(evidence.created_ms)}</DetailRow>
          </DetailList>
          <div className="evidence-card" style={{ marginTop: 12 }}>
            <a className="btn primary block" href={evidence.download}>下载已校验的 EDV2 原始数据</a>
            <button className="btn secondary" type="button" onClick={onReload}>重新读取元数据</button>
          </div>
        </>
      )}

      <p className="panel-note">
        下载链接指向服务端已校验的 EDV2 文件（16 KB 以内、含 CRC）。
        HTTP 202 只代表请求已进入持久命令队列，不代表设备已经采集。
      </p>
    </Panel>
  );
}

export function EventActions({ event, command, onAck, onRequestEvidence, disabled }: {
  event: EventRecord;
  command: CommandState | null;
  onAck: () => void;
  onRequestEvidence: () => void;
  disabled: boolean;
}) {
  const pendingAck = command?.kind === 'ack' && command.status === 'PENDING';
  const pendingEvidence = command?.kind === 'evidence' && command.status === 'PENDING';
  const terminalFailed = command && (command.status === 'REJECTED' || command.status === 'EXPIRED');
  const needsAck = event.condition === 'ACTIVE' && event.ack !== 'ACKNOWLEDGED';

  return (
    <div className="btn-group">
      <button className="btn primary" type="button" disabled={disabled || !needsAck || pendingAck} onClick={onAck}>
        {event.ack === 'ACKNOWLEDGED' ? '已确认' : pendingAck ? '确认请求待回执' : '确认事件'}
      </button>
      <button className="btn secondary" type="button" disabled={disabled || pendingEvidence} onClick={onRequestEvidence}>
        {pendingEvidence ? '留证请求待回执' : '请求留证'}
      </button>
      {command && <Badge tone={commandView(command.status).tone}>{commandView(command.status).label}</Badge>}
      {terminalFailed && (
        <span className="faint">上一条命令{commandView(command.status).label}：{commandView(command.status).hint}</span>
      )}
    </div>
  );
}

/** 信号分析入口的服务端状态：未实现解析时明确说明，不展示模拟频谱。 */
export function AnalysisStatusCard({ analysis, error, eventId }: { analysis: AnalysisView | null; error: ApiError | null; eventId: string | null }) {
  if (!eventId) {
    return (
      <Panel>
        <PanelHead eyebrow="Analysis" title="分析接口状态" />
        <StateView title="未选择事件">选择事件后查询该事件的 EDV2 分析结果。</StateView>
      </Panel>
    );
  }
  if (error) {
    return (
      <Panel>
        <PanelHead eyebrow="Analysis" title="分析接口状态" />
        <ErrorNotice error={error} />
      </Panel>
    );
  }
  if (!analysis) {
    return (
      <Panel>
        <PanelHead eyebrow="Analysis" title="分析接口状态" />
        <StateView tone="loading" title="正在查询分析接口" />
      </Panel>
    );
  }
  return (
    <Panel>
      <PanelHead
        eyebrow="Analysis"
        title="分析接口状态"
        meta={<Badge tone={analysis.available ? 'ok' : 'unknown'}>{analysis.available ? '可用' : '不可用'}</Badge>}
      />
      <DetailList>
        <DetailRow term="事件">{analysis.event_id}</DetailRow>
        <DetailRow term="服务端解析">{analysis.available ? '已解析并校验' : '缺少合格证据或证据校验未通过'}</DetailRow>
      </DetailList>
      {analysis.reason && <p className="panel-note">服务端返回的原因：{analysis.reason}</p>}
    </Panel>
  );
}
