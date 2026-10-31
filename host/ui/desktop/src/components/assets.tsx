// 资产、分区与设备卡片。所有数字都来自服务端汇总、绑定配置或快照，不做估算。

import type { EventRecord, FactoryAsset, FactorySummary, Snapshot } from '../api/types';
import { bindingForAsset, zoneDefinitions, zoneIcon, zoneMeta } from '../config/zones';
import { ageText, bytesText, dateTimeText, fixed, PLACEHOLDER } from '../lib/format';
import { assetView, bindingView, eventView, freshnessView, snapshotView } from '../lib/status';
import { Badge, DetailList, DetailRow, Metric, MetricGrid, Panel, PanelHead, StateView, StatusBadge, StatusDot } from './primitives';

export function HealthCard({ summary, nowMs }: { summary: FactorySummary | null; nowMs: number }) {
  if (!summary) {
    return (
      <Panel>
        <PanelHead eyebrow="Factory Health" title="工厂健康度" />
        <StateView title="尚未读取汇总">服务端汇总接口返回后显示真实统计。</StateView>
      </Panel>
    );
  }
  const { counts, assets, online_threshold_ms: threshold } = summary;
  const verified = assets.filter((asset) => asset.binding_state === 'verified');
  const rate = verified.length > 0 ? counts.ONLINE / verified.length : null;
  const freshest = assets.reduce<number | null>((latest, asset) => (
    asset.last_telemetry_received_ms !== null && (latest === null || asset.last_telemetry_received_ms > latest)
      ? asset.last_telemetry_received_ms : latest
  ), null);

  return (
    <Panel>
      <PanelHead eyebrow="Factory Health" title="工厂健康度" meta={`阈值 ${Math.round(threshold / 1000)} 秒`} />
      <div style={{ display: 'flex', alignItems: 'center', gap: 16 }}>
        <div
          style={{
            display: 'grid', placeItems: 'center', width: 112, height: 112, borderRadius: '50%', flex: 'none',
            background: rate === null ? 'var(--idle-bg)' : `conic-gradient(var(--brand-600) ${rate * 360}deg, var(--idle-bg) ${rate * 360}deg)`,
          }}
          role="img"
          aria-label={rate === null ? '在线率不可计算：没有已验证绑定资产' : `已验证绑定资产在线率 ${(rate * 100).toFixed(0)}%`}
        >
          <div style={{ display: 'grid', placeItems: 'center', width: 84, height: 84, borderRadius: '50%', background: 'var(--surface)' }}>
            <strong className="num" style={{ fontSize: 23 }}>{rate === null ? PLACEHOLDER : `${(rate * 100).toFixed(0)}%`}</strong>
            <span className="faint" style={{ fontSize: 10.5 }}>已验证在线率</span>
          </div>
        </div>
        <div style={{ flex: 1, minWidth: 0 }}>
          <p className="muted" style={{ fontSize: 12.5 }}>
            在线 {counts.ONLINE} / 已验证绑定 {verified.length}，数据过期 {counts.STALE}，
            未接入与待核验 {counts.UNKNOWN}。
          </p>
          <p className="faint" style={{ fontSize: 11, marginTop: 6 }}>
            最近遥测：{freshest === null ? '暂无接收记录' : `${ageText(freshest, nowMs)}`}
          </p>
        </div>
      </div>
      <MetricGrid columns={3}>
        <Metric label="绑定资产" value={assets.length} hint="配置中的合计" />
        <Metric label="在线" value={counts.ONLINE} tone={counts.ONLINE > 0 ? undefined : 'unknown'} hint="服务端阈值内" />
        <Metric label="数据过期" value={counts.STALE} tone={counts.STALE > 0 ? 'warning' : undefined} hint="超阈值未上报" />
      </MetricGrid>
      <p className="panel-note">
        在线率分母是“已验证绑定”资产；未接入与待核验资产不计入分子或分母，
        在线判定完全由服务端按冻结阈值给出，控制台不推断。
      </p>
    </Panel>
  );
}

export function ZoneList({ summary, selectedZone, onSelect }: {
  summary: FactorySummary | null;
  selectedZone: string | null;
  onSelect: (zone: string | null) => void;
}) {
  const assets = summary?.assets ?? [];
  const buckets = zoneDefinitions.map((zone) => {
    const items = assets.filter((asset) => asset.zone === zone.zone);
    const online = items.filter((asset) => asset.status === 'ONLINE').length;
    const offline = items.length - online;
    return { zone, items, online, offline };
  });

  return (
    <Panel>
      <PanelHead eyebrow="Zone Status" title="区域状态" meta={`${zoneDefinitions.length} 个分区`} />
      <div className="zone-list">
        {buckets.map(({ zone, items, online }) => (
          <button
            key={zone.zone}
            type="button"
            className="zone-row"
            aria-pressed={selectedZone === zone.zone}
            onClick={() => onSelect(selectedZone === zone.zone ? null : zone.zone)}
          >
            <span className="zone-icon" aria-hidden="true">{zoneIcon(zone.zone)}</span>
            <span>
              <strong>{zone.display_name_zh}</strong>
              <small>{zone.display_name_en}</small>
              <small>
                {items.length === 0 ? '该分区没有绑定资产' : `绑定 ${items.length} · 在线 ${online}`}
              </small>
            </span>
            <span className="zone-count num">{items.length}</span>
          </button>
        ))}
      </div>
      <p className="panel-note">分区计数来自冻结的资产绑定配置；点击分区可在三维场景中聚焦该区。</p>
    </Panel>
  );
}

export function AssetList({ assets, selectedAssetId, onSelect, zoneFilter }: {
  assets: FactoryAsset[];
  selectedAssetId: string | null;
  onSelect: (assetId: string) => void;
  zoneFilter: string | null;
}) {
  const visible = zoneFilter ? assets.filter((asset) => asset.zone === zoneFilter) : assets;
  return (
    <Panel>
      <PanelHead
        eyebrow="Assets"
        title="绑定资产"
        meta={zoneFilter ? `已筛选 ${zoneMeta(zoneFilter).display_name_zh}` : `共 ${assets.length} 项`}
      />
      {visible.length === 0
        ? <StateView title="该分区没有资产">请选择其他分区或清除筛选。</StateView>
        : (
          <div className="asset-list">
            {visible.map((asset) => {
              const view = assetView(asset.status);
              return (
                <button
                  key={asset.asset_id}
                  type="button"
                  className="asset-chip"
                  aria-current={asset.asset_id === selectedAssetId}
                  onClick={() => onSelect(asset.asset_id)}
                >
                  <StatusDot tone={view.tone} label={view.label} />
                  <span className="asset-name">{asset.display_name_zh}</span>
                  <Badge tone={view.tone}>{view.label}</Badge>
                </button>
              );
            })}
          </div>
        )}
    </Panel>
  );
}

export function SelectedAssetCard({ asset, snapshot, deviceEvents, nowMs, thresholdMs, onOpenMonitor, onOpenEvents }: {
  asset: FactoryAsset | null;
  snapshot: Snapshot | null;
  /** 该设备在服务端最近 50 条事件中的记录，用于显示活动报警与恢复未确认状态。 */
  deviceEvents: EventRecord[];
  nowMs: number;
  thresholdMs: number;
  onOpenMonitor: (deviceId: string) => void;
  onOpenEvents: (deviceId: string) => void;
}) {
  if (!asset) {
    return (
      <Panel>
        <PanelHead eyebrow="Selected Asset" title="未选择设备" />
        <StateView title="请在场景或列表中选择资产">选中已绑定设备后显示其服务端状态。</StateView>
      </Panel>
    );
  }
  const binding = bindingView(asset.binding_state);
  const status = assetView(asset.status);
  const freshness = freshnessView(asset.last_telemetry_received_ms, nowMs, thresholdMs);
  const snapshotStatus = snapshot?.status ?? null;
  const hasTelemetry = snapshot?.telemetry != null;
  // 与服务端 /api/v1/query/status 同源：只以最近一条事件判定当前状态，
  // 避免“列表里还有更早的活动事件”与侧栏结论互相矛盾。
  const latestEvent = deviceEvents[0] ?? null;
  const eventView_ = latestEvent && latestEvent.condition === 'ACTIVE' ? eventView(latestEvent) : null;
  const recoveredUnacknowledged = latestEvent?.condition === 'RECOVERED' && latestEvent.ack === 'UNACKNOWLEDGED';
  const recoveredAcknowledged = latestEvent?.condition === 'RECOVERED' && latestEvent.ack === 'ACKNOWLEDGED';

  return (
    <Panel>
      <PanelHead
        eyebrow="Selected Asset"
        title={asset.display_name_zh}
        actions={<div className="btn-group"><StatusBadge tone={status.tone} label={status.label} /><Badge tone={binding.tone}>{binding.label}</Badge></div>}
      />
      <DetailList>
        <DetailRow term="资产 / 设备">
          {asset.asset_id} / {asset.device_id ?? '未绑定设备'}
        </DetailRow>
        <DetailRow term="分区">{zoneMeta(asset.zone).display_name_zh}（{asset.zone}）</DetailRow>
        <DetailRow term="场景实例">{asset.scene_instance}</DetailRow>
        <DetailRow term="传感器节点">{asset.sensor_node ?? '未配置'}</DetailRow>
        <DetailRow term="最近遥测">{ageText(asset.last_telemetry_received_ms, nowMs)}</DetailRow>
        <DetailRow term="数据新鲜度">{freshness.label}</DetailRow>
        <DetailRow term="活动事件">
          {eventView_
            ? <StatusBadge tone={eventView_.tone} label={`${eventView_.severityLabel}报警 · ${eventView_.ackLabel}`} />
            : recoveredUnacknowledged
              ? <StatusBadge tone="ok" label="已恢复 · 待确认" />
              : recoveredAcknowledged
                ? <StatusBadge tone="ok" label="已恢复 · 已确认" />
                : <span className="faint">最近 50 条事件内无活动报警</span>}
        </DetailRow>
      </DetailList>

      {asset.binding_state !== 'verified' && (
        <p className="panel-note">
          {asset.binding_state === 'provisional'
            ? '该绑定尚未核验：模型可以浏览，但不作为在线设备展示，也不产生诊断结论。'
            : '该模型未绑定设备：仅用于浏览工厂布局，没有遥测与事件。'}
        </p>
      )}

      {asset.device_id && (
        <>
          <MetricGrid columns={2}>
            <Metric
              label="当前 RMS"
              value={hasTelemetry ? fixed(snapshot?.telemetry?.rms_vector_g, 4) : PLACEHOLDER}
              unit={hasTelemetry ? 'g' : undefined}
              tone={hasTelemetry ? undefined : 'unknown'}
              small
            />
            <Metric
              label="异常分数"
              value={hasTelemetry ? fixed(snapshot?.telemetry?.anomaly_score, 1) : PLACEHOLDER}
              tone={hasTelemetry ? undefined : 'unknown'}
              small
            />
          </MetricGrid>
          <p className="faint" style={{ fontSize: 11, marginTop: 8 }}>
            服务端快照状态：{snapshotView(snapshotStatus).label}
            {snapshot?.advice ? ` · ${snapshot.advice}` : ''}
          </p>
          <div className="btn-group" style={{ marginTop: 12 }}>
            <button className="btn primary" type="button" onClick={() => onOpenMonitor(asset.device_id as string)}>查看实时监测</button>
            <button className="btn secondary" type="button" onClick={() => onOpenEvents(asset.device_id as string)}>查看事件</button>
          </div>
        </>
      )}
    </Panel>
  );
}

export function DeviceSpecCard({ asset, nowMs, thresholdMs, streamConnected }: {
  asset: FactoryAsset | null;
  nowMs: number;
  thresholdMs: number;
  streamConnected: boolean;
}) {
  if (!asset) {
    return (
      <Panel>
        <PanelHead eyebrow="Device" title="设备信息" />
        <StateView title="未选择设备">请先选择已绑定设备。</StateView>
      </Panel>
    );
  }
  const binding = bindingForAsset(asset.asset_id);
  const freshness = freshnessView(asset.last_telemetry_received_ms, nowMs, thresholdMs);
  return (
    <Panel>
      <PanelHead eyebrow="Device Spec" title="设备与绑定" meta={<Badge tone={bindingView(asset.binding_state).tone}>{bindingView(asset.binding_state).label}</Badge>} />
      <DetailList>
        <DetailRow term="资产标识">{asset.asset_id}</DetailRow>
        <DetailRow term="设备标识">{asset.device_id ?? '未绑定'}</DetailRow>
        <DetailRow term="分区">{zoneMeta(asset.zone).display_name_zh}（{asset.zone}）</DetailRow>
        <DetailRow term="场景实例">{asset.scene_instance}</DetailRow>
        <DetailRow term="传感器节点">{asset.sensor_node ?? '未配置'}</DetailRow>
        <DetailRow term="详情模型">{binding?.detail_model ?? '未配置'}</DetailRow>
        <DetailRow term="采样率">服务端未提供</DetailRow>
        <DetailRow term="最近遥测">{ageText(asset.last_telemetry_received_ms, nowMs)}</DetailRow>
        <DetailRow term="数据新鲜度">{freshness.label}</DetailRow>
        <DetailRow term="刷新通道">{streamConnected ? 'SSE 实时推送 + 兜底轮询' : '10 秒轮询'}</DetailRow>
      </DetailList>
      <p className="panel-note">
        采样率、上传频率等设备参数需要服务端提供；当前接口未返回这些字段，因此显示为未提供，
        不使用推测值填充。
      </p>
    </Panel>
  );
}

export function SamplingCard({ samples, asset, nowMs }: { samples: { length: number }; asset: FactoryAsset | null; nowMs: number }) {
  const last = asset?.last_telemetry_received_ms ?? null;
  return (
    <Panel>
      <PanelHead eyebrow="Sampling" title="采样与流量" />
      <DetailList>
        <DetailRow term="窗口内有效采样">{samples.length}</DetailRow>
        <DetailRow term="服务端最新接收">{last === null ? '暂无接收记录' : dateTimeText(last)}</DetailRow>
        <DetailRow term="数据年龄">{ageText(last, nowMs)}</DetailRow>
        <DetailRow term="采样率">服务端未提供</DetailRow>
        <DetailRow term="上传频率">服务端未提供</DetailRow>
      </DetailList>
      <p className="panel-note">
        趋势窗口按设备单调时钟（mono_ms）截取，不是墙钟时间；服务端未提供采样率与上传频率时不做推算。
      </p>
    </Panel>
  );
}

export function formatBytes(value: number | null | undefined): string {
  return bytesText(value);
}
