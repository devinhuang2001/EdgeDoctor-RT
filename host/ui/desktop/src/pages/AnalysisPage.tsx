// 信号分析：只呈现可验证的固定特征与已校验的 EDV2 分析结果。
// 频谱、包络、主峰与统计特征在没有服务端解析结果时明确标为不可用（规格 §7）。

import { useMemo, useState } from 'react';
import type { DashboardState } from '../state/store';
import { EvidenceChart, SpectrumFrame } from '../components/charts';
import { AnalysisStatusCard } from '../components/evidence';
import { DetailList, DetailRow, ErrorNotice, Metric, MetricGrid, Panel, PanelHead, Segmented, StateView } from '../components/primitives';
import { useDeviceSeries } from '../state/useDeviceSeries';
import { diagnosisText, durationText, fixed, integer, PLACEHOLDER, qualityText } from '../lib/format';
import { nyquistLimitHz } from '../lib/spectrum';
import { rmsSummary } from '../lib/trend';

type View = 'spectrum' | 'envelope' | 'stats';

const UNAVAILABLE_REASON = '服务端尚未提供已解析并通过校验的 EDV2 证据，因此不绘制频谱、包络或统计特征。';

export function AnalysisPage({ state, onSelectEvent }: {
  state: DashboardState;
  onSelectEvent: (eventId: string) => void;
}) {
  const [view, setView] = useState<View>('spectrum');

  const asset = useMemo(() => (state.summary?.assets ?? []).find((item) => item.device_id === state.deviceId) ?? null, [state.summary, state.deviceId]);
  const series = useDeviceSeries(asset?.device_id ?? null);
  const summary = useMemo(() => rmsSummary(series.samples), [series.samples]);
  const latestEvent = state.events[0] ?? null;
  const selectedEvent = state.events.find((event) => event.event_id === state.selectedEventId) ?? latestEvent;

  // 采样率只能来自已校验的证据；服务端分析接口未提供时为 null，横轴不给上限。
  const analysis = state.analysis?.event_id === state.selectedEventId ? state.analysis.data : undefined;
  const sampleRateHz = analysis?.sample_rate_hz ?? null;

  return (
    <div className="page-grid analysis-grid">
      <div className="stack">
        <Panel>
          <PanelHead
            eyebrow="Features"
            title="特征概览"
            meta={asset ? `设备 ${asset.device_id}` : undefined}
          />
          {!asset
            ? <StateView tone="warning" title="未选择已绑定设备">信号分析需要已验证绑定的设备。</StateView>
            : series.loading
              ? <StateView tone="loading" title="正在读取遥测序列" />
              : series.error
                ? <ErrorNotice error={series.error} />
                : (
                  <>
                    <MetricGrid columns={2}>
                      <Metric label="RMS 最新值" value={fixed(summary.last, 4)} unit={summary.last === null ? undefined : 'g'} tone={summary.last === null ? 'unknown' : undefined} hint="序列最后一个有效采样" />
                      <Metric label="窗口峰值" value={fixed(summary.peak, 4)} unit={summary.peak === null ? undefined : 'g'} tone={summary.peak === null ? 'unknown' : undefined} hint="窗口内最大值" />
                      <Metric label="窗口最小值" value={fixed(summary.min, 4)} unit={summary.min === null ? undefined : 'g'} tone={summary.min === null ? 'unknown' : undefined} hint="窗口内最小值" />
                      <Metric label="有效采样" value={integer(summary.count)} hint="含数值的采样点" />
                    </MetricGrid>
                    <DetailList>
                      <DetailRow term="数据窗口">{durationText(summary.windowMs)}（设备时钟）</DetailRow>
                      <DetailRow term="Crest Factor（峰值因子）">{analysis ? `X ${fixed(analysis.statistics.x.crest_factor, 3)} · Y ${fixed(analysis.statistics.y.crest_factor, 3)} · Z ${fixed(analysis.statistics.z.crest_factor, 3)}` : '不可用：需要原始波形'}</DetailRow>
                      <DetailRow term="Kurtosis（峭度）">{analysis ? `X ${fixed(analysis.statistics.x.kurtosis, 3)} · Y ${fixed(analysis.statistics.y.kurtosis, 3)} · Z ${fixed(analysis.statistics.z.kurtosis, 3)}` : '不可用：需要原始波形'}</DetailRow>
                    </DetailList>
                  </>
                )}
          <p className="panel-note">
            特征概览只使用已有遥测的 RMS 序列做固定计算；
            峰值因子与峭度必须由原始波形计算，服务端未提供前显示为不可用，不用估算值填充。
          </p>
        </Panel>

        <Panel>
          <PanelHead eyebrow="Boundary" title="数据边界" />
          <ul className="muted" style={{ paddingLeft: 18, fontSize: 12.5, display: 'grid', gap: 6 }}>
            <li>频谱横轴上限由证据中的实际采样率决定，且不超过奈奎斯特频率。</li>
            <li>208 Hz 采样率的数据，任何图表都不得显示 104 Hz 以上的实测频谱。</li>
            <li>不使用随机数、固定主频或固定故障类别填充无数据状态。</li>
            <li>异常只代表相对基线的观测偏离，没有定位证据时只高亮设备或传感器安装位置。</li>
          </ul>
        </Panel>
      </div>

      <div className="stack">
        <Panel>
          <PanelHead
            eyebrow="Spectrum Analysis"
            title="频谱与派生视图"
            meta={sampleRateHz === null ? '单位 g · 采样率未知' : `单位 g · 采样率 ${sampleRateHz} Hz`}
            actions={(
              <Segmented
                value={view}
                options={[{ value: 'spectrum', label: '频谱' }, { value: 'envelope', label: '包络' }, { value: 'stats', label: '统计特征' }]}
                onChange={setView}
                label="分析视图"
              />
            )}
          />
          {!analysis ? <SpectrumFrame sampleRateHz={sampleRateHz} reason={UNAVAILABLE_REASON} /> : <>
            {view === 'spectrum' && <EvidenceChart coordinates={analysis.spectrum.frequency_hz} values={analysis.spectrum} label="已校验三轴频谱，幅值 g" frequency />}
            {view === 'envelope' && <><EvidenceChart coordinates={analysis.envelope.time_s} values={analysis.envelope} label="已校验三轴整流平滑幅值，单位 g" /><p className="panel-note">去均值后取绝对值并做约 100ms 移动平均。</p></>}
            {view === 'stats' && <table className="table"><thead><tr><th>轴</th><th>AC RMS (g)</th><th>AC 峰值 (g)</th><th>均值 (g)</th></tr></thead><tbody>{(['x', 'y', 'z'] as const).map((axis) => <tr key={axis}><td>{axis.toUpperCase()}</td><td>{fixed(analysis.statistics[axis].rms_ac_g, 5)}</td><td>{fixed(analysis.statistics[axis].peak_ac_g, 5)}</td><td>{fixed(analysis.statistics[axis].mean_g, 5)}</td></tr>)}</tbody></table>}
            <h3>原始三轴波形</h3>
            <EvidenceChart coordinates={analysis.waveform.time_s} values={analysis.waveform} label="已校验原始三轴波形，单位 g" />
          </>}
          <p className="panel-note">
            {analysis ? `已校验 ${analysis.sample_count} 样本 · ${analysis.parser_version} · ${analysis.sample_rate_kind === 'nominal' ? '标称采样率' : '证据声明采样率'}。Hann 窗单边幅值谱，去除直流分量。`
              : view === 'spectrum' ? '频谱视图等待服务端解析结果。'
              : view === 'envelope' ? '包络视图等待服务端解析结果。'
                : '统计特征视图等待服务端解析结果。'}
            奈奎斯特上限：{nyquistLimitHz(sampleRateHz ?? undefined) === null ? '无法确定（缺少证据采样率）' : `${nyquistLimitHz(sampleRateHz ?? undefined)} Hz`}。
          </p>
        </Panel>

        <Panel>
          <PanelHead eyebrow="Dominant Peaks" title="主峰" meta="需要已校验证据" />
          <table className="table">
            <thead>
              <tr>
                <th scope="col">#</th>
                <th scope="col">频率</th>
                <th scope="col">幅值</th>
                <th scope="col">可能来源</th>
              </tr>
            </thead>
            <tbody>
              {analysis ? analysis.dominant_peaks.length > 0 ? analysis.dominant_peaks.map((peak, i) => <tr key={`${peak.axis}-${peak.frequency_hz}`}><td>{i + 1} · {peak.axis}</td><td>{fixed(peak.frequency_hz, 2)} Hz</td><td>{fixed(peak.amplitude_g, 5)} g</td><td>未定位（仅观测峰值）</td></tr>) : <tr><td colSpan={4}>无非零局部主峰</td></tr> : <tr>
                <td colSpan={4}>
                  <StateView title="暂无主峰数据">
                    主峰必须来自服务端解析并校验通过的频谱结果；控制台不生成示例峰值。
                  </StateView>
                </td>
              </tr>}
            </tbody>
          </table>
        </Panel>
      </div>

      <div className="stack analysis-side-column">
        <Panel>
          <PanelHead eyebrow="Event Selection" title="分析对象" meta={`${state.events.length} 条事件`} />
          {state.events.length === 0
            ? <StateView title="该设备暂无事件">没有事件就没有与该事件绑定的留证与分析结果。</StateView>
            : (
              <>
                <label className="token-field" style={{ width: '100%' }}>
                  选择事件
                  <select
                    value={selectedEvent?.event_id ?? ''}
                    onChange={(event) => onSelectEvent(event.target.value)}
                    style={{ width: '100%', padding: '8px 10px', border: '1px solid var(--border-strong)', borderRadius: 8, font: 'inherit' }}
                  >
                    {state.events.map((event) => (
                      <option key={event.event_id} value={event.event_id}>
                        {event.event_id} · rev {event.revision} · {event.condition === 'ACTIVE' ? '活动' : '已恢复'}
                      </option>
                    ))}
                  </select>
                </label>
                {selectedEvent && (
                  <DetailList>
                    <DetailRow term="设备">{selectedEvent.device_id}</DetailRow>
                    <DetailRow term="服务端诊断">{diagnosisText(selectedEvent.diagnosis)}</DetailRow>
                    <DetailRow term="信号质量">{qualityText(selectedEvent.quality)}</DetailRow>
                    <DetailRow term="基线偏离分数（无量纲）">
                      {selectedEvent.peak_deviation === undefined ? PLACEHOLDER : selectedEvent.peak_deviation.toFixed(4)}
                    </DetailRow>
                  </DetailList>
                )}
              </>
            )}
          <p className="panel-note">
            事件绑定的留证分析结果由服务端解析；控制台只展示其状态与字段，不自行推断故障类别。
          </p>
        </Panel>

        <AnalysisStatusCard analysis={state.analysis} error={state.analysisError} eventId={state.selectedEventId} />
      </div>
    </div>
  );
}
