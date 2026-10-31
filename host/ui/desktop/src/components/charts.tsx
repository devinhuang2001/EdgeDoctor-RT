// 趋势与频谱组件。只绘制真实数据：缺样断开、恒定值走中线、频谱无证据时不画线。

import type { AxisSeries, Telemetry } from '../api/types';
import { fixed } from '../lib/format';
import { alarmPoints, chartGeometry, trendSeries } from '../lib/trend';

type TrendChartProps = {
  samples: Telemetry[];
  /** 设备单调时钟的相对标签，单位秒。 */
  ariaLabel?: string;
  windowLabel?: string;
};

export function TrendChart({ samples, ariaLabel = 'RMS 振动趋势，单位 g', windowLabel }: TrendChartProps) {
  const series = trendSeries(samples);
  if (series.valid.length === 0) {
    return (
      <div className="chart-frame">
        <div className="chart-empty">
          {samples.length === 0
            ? '尚无遥测采样。服务端收到带 rms_vector_g 的遥测后才会绘制趋势。'
            : '当前窗口内没有有效的 rms_vector_g，缺失采样不绘制为 0。'}
        </div>
      </div>
    );
  }

  const origin = series.valid[0].x;
  const geometry = chartGeometry(series, {
    formatX: (value) => `${((value - origin) / 1000).toFixed(1)}s`,
    formatY: (value) => value.toFixed(3),
  });
  const alarms = alarmPoints(series);

  return (
    <div>
      <div className="chart-frame">
        <svg viewBox="0 0 100 100" preserveAspectRatio="none" role="img" aria-label={ariaLabel}>
          <defs>
            <clipPath id="trend-plot"><rect x="7" y="4" width="86" height="82" /></clipPath>
          </defs>
          {geometry.yTicks.map((tick) => (
            <line key={`y-${tick.position}`} className="chart-grid" x1="7" x2="93" y1={tick.position} y2={tick.position} />
          ))}
          {geometry.xTicks.map((tick) => (
            <line key={`x-${tick.position}`} className="chart-grid" x1={tick.position} x2={tick.position} y1="4" y2="86" />
          ))}
          <path d={geometry.path} className="chart-line" clipPath="url(#trend-plot)" vectorEffect="non-scaling-stroke" />
          {geometry.alarms.map((alarm, index) => (
            <circle key={index} className="chart-marker" cx={alarm.cx} cy={alarm.cy} r="1.4" />
          ))}
          {geometry.xTicks.map((tick) => (
            <text key={`xl-${tick.position}`} className="chart-axis" x={tick.position} y="97" textAnchor="middle">{tick.label}</text>
          ))}
          {geometry.yTicks.map((tick) => (
            <text key={`yl-${tick.position}`} className="chart-axis" x="5" y={tick.position + 1.4} textAnchor="end">{tick.label}</text>
          ))}
          <text className="chart-axis" x="5" y="9" textAnchor="start">g</text>
        </svg>
      </div>
      <div className="chart-legend">
        <span><i />RMS 有效值</span>
        <span><i className="critical" />服务端 alarm 标记（{alarms.length} 点）</span>
        <span>{series.valid.length} 个有效采样 · 缺样为断点</span>
        {windowLabel && <span>窗口：{windowLabel}（设备单调时钟）</span>}
      </div>
    </div>
  );
}

export function RmsRange({ min, max }: { min: number | null; max: number | null }) {
  return <span className="faint">{fixed(min, 4)} – {fixed(max, 4)} g</span>;
}

export function EvidenceChart({ coordinates, values, label, frequency = false }: { coordinates: number[]; values: AxisSeries; label: string; frequency?: boolean }) {
  const axes = ['x_g', 'y_g', 'z_g'] as const;
  const all = axes.flatMap((axis) => values[axis]);
  const min = Math.min(...all), max = Math.max(...all);
  const plots = axes.map((axis) => {
    const series = trendSeries(values[axis].map((value, i) => ({ mono_ms: coordinates[i], rms_vector_g: value })));
    return chartGeometry({ ...series, min, max, span: max - min, constant: min === max }, { formatX: (value) => `${value.toFixed(1)}${frequency ? 'Hz' : 's'}`, formatY: (value) => value.toPrecision(2) });
  });
  return <div data-testid={frequency ? 'verified-spectrum' : 'verified-waveform'}>
    <div className="chart-frame"><svg viewBox="0 0 100 100" preserveAspectRatio="none" role="img" aria-label={label}>
      {plots[0].yTicks.map((tick) => <g key={tick.position}><line className="chart-grid" x1="7" x2="93" y1={tick.position} y2={tick.position} /><text className="chart-axis" x="5" y={tick.position} textAnchor="end">{tick.label}</text></g>)}
      {plots.map((plot, i) => <path key={axes[i]} d={plot.path} className="chart-line" style={{ stroke: ['#16805c', '#2d63ba', '#b14c26'][i] }} vectorEffect="non-scaling-stroke" />)}
      {plots[0].xTicks.map((tick) => <text key={tick.position} className="chart-axis" x={tick.position} y="97" textAnchor="middle">{tick.label}</text>)}
    </svg></div>
    <div className="chart-legend">X（绿） · Y（蓝） · Z（橙） · 幅值 g · {coordinates.length} 点</div>
  </div>;
}

/**
 * 频谱占位。规格 §7：横轴上限不得超过奈奎斯特频率；
 * 在服务端提供并校验 EDV2 证据前，这里不绘制任何谱线。
 */
export function SpectrumFrame({ sampleRateHz, reason }: { sampleRateHz: number | null; reason: string }) {
  const nyquist = typeof sampleRateHz === 'number' && sampleRateHz > 0 ? sampleRateHz / 2 : null;
  return (
    <div className="spectrum-frame" role="img" aria-label={reason}>
      <span className="spectrum-unit">幅值 (g)</span>
      <p className="muted">{reason}</p>
      <div className="spectrum-axis">
        <span>0 Hz</span>
        <span>{nyquist === null ? '未获得证据采样率' : `上限 ${nyquist.toFixed(1)} Hz（奈奎斯特）`}</span>
      </div>
    </div>
  );
}
