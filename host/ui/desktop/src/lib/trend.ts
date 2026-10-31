// 趋势几何与统计。规则（规格 §5.1 / §7 / DD-UI-03）：
//   - 只绘制真实存在且有限的 rms_vector_g，单位为 g；
//   - 缺样以断点呈现，不插值、不补零；
//   - 恒定值不得产生 NaN 或无穷；
//   - 报警点只来自服务端给出的 alarm 字段。

import type { Telemetry } from '../api/types';

export type TrendPoint = { x: number; y: number; alarm: boolean };

export type TrendSeries = {
  /** 与服务端顺序一致的采样序列，缺样位置为 null。 */
  points: (TrendPoint | null)[];
  valid: TrendPoint[];
  min: number;
  max: number;
  span: number;
  /** 有值但恒定的序列在图上按中线绘制。 */
  constant: boolean;
};

/** 设备单调时钟：优先使用服务端给出的 timestamp_ms，缺失时退回 mono_ms。 */
export const sampleClock = (sample: Telemetry): number => (
  typeof sample.timestamp_ms === 'number' && Number.isFinite(sample.timestamp_ms) ? sample.timestamp_ms : sample.mono_ms
);

export function sortByClock(samples: Telemetry[]): Telemetry[] {
  return [...samples].sort((left, right) => sampleClock(left) - sampleClock(right));
}

/** 按设备单调时钟截取最近 windowMs 的采样；windowMs 为空表示取全部。 */
export function windowByDeviceClock(samples: Telemetry[], windowMs: number | null): Telemetry[] {
  const sorted = sortByClock(samples);
  if (windowMs === null || sorted.length === 0) return sorted;
  const latest = sampleClock(sorted[sorted.length - 1]);
  return sorted.filter((sample) => sampleClock(sample) >= latest - windowMs);
}

export function trendSeries(samples: Telemetry[]): TrendSeries {
  const points: (TrendPoint | null)[] = samples.map((sample) => {
    const value = sample.rms_vector_g;
    if (typeof value !== 'number' || !Number.isFinite(value)) return null;
    return { x: sampleClock(sample), y: value, alarm: sample.alarm === true };
  });
  const valid = points.filter((point): point is TrendPoint => point !== null);
  if (valid.length === 0) {
    return { points, valid, min: 0, max: 0, span: 0, constant: false };
  }
  let min = valid[0].y;
  let max = valid[0].y;
  for (const point of valid) {
    if (point.y < min) min = point.y;
    if (point.y > max) max = point.y;
  }
  const span = max - min;
  return { points, valid, min, max, span, constant: span === 0 };
}

export const alarmPoints = (series: TrendSeries): TrendPoint[] => series.valid.filter((point) => point.alarm);

export type RmsSummary = {
  count: number;
  last: number | null;
  peak: number | null;
  min: number | null;
  windowMs: number | null;
};

/** 窗口内的真实统计量；没有有效采样时所有字段为 null。 */
export function rmsSummary(samples: Telemetry[]): RmsSummary {
  const series = trendSeries(samples);
  if (series.valid.length === 0) return { count: 0, last: null, peak: null, min: null, windowMs: null };
  const first = series.valid[0];
  const last = series.valid[series.valid.length - 1];
  return {
    count: series.valid.length,
    last: last.y,
    peak: series.max,
    min: series.min,
    windowMs: last.x - first.x,
  };
}

// ---------- 图表几何 ----------

export type ChartTick = { position: number; label: string };
export type ChartGeometry = {
  /** 每条连续线段一个子路径；缺样处自然断开。 */
  path: string;
  alarms: { cx: number; cy: number }[];
  xTicks: ChartTick[];
  yTicks: ChartTick[];
};

type ChartOptions = {
  width?: number;
  height?: number;
  paddingX?: number;
  paddingY?: number;
  tickCount?: number;
  formatX?: (value: number) => string;
  formatY?: (value: number) => string;
};

/**
 * 在 0..width / 0..height 的坐标系里生成折线路径。
 * 恒定值的序列画在垂直中线，避免除零产生 NaN。
 */
export function chartGeometry(series: TrendSeries, options: ChartOptions = {}): ChartGeometry {
  const {
    width = 100, height = 100, paddingX = 7, paddingY = 9, tickCount = 4,
    formatX = (value: number) => value.toFixed(1),
    formatY = (value: number) => value.toFixed(3),
  } = options;

  const innerW = Math.max(1, width - paddingX * 2);
  const innerH = Math.max(1, height - paddingY * 2);
  const valid = series.valid;
  if (valid.length === 0) return { path: '', alarms: [], xTicks: [], yTicks: [] };

  const minX = valid[0].x;
  const maxX = valid[valid.length - 1].x;
  const spanX = maxX - minX;

  const toX = (value: number) => (spanX === 0 ? paddingX + innerW / 2 : paddingX + ((value - minX) / spanX) * innerW);
  // 恒定值或单点：画在垂直中线，避免除零产生 NaN/无穷。
  const toY = (value: number) => (series.constant
    ? paddingY + innerH / 2
    : height - paddingY - ((value - series.min) / series.span) * innerH);

  let path = '';
  let penDown = false;
  const alarms: { cx: number; cy: number }[] = [];
  for (const point of series.points) {
    if (point === null) { penDown = false; continue; }
    const x = toX(point.x);
    const y = toY(point.y);
    path += `${penDown ? 'L' : 'M'}${x.toFixed(2)},${y.toFixed(2)} `;
    penDown = true;
    if (point.alarm) alarms.push({ cx: round2(x), cy: round2(y) });
  }

  const xTicks: ChartTick[] = [];
  for (let index = 0; index < tickCount; index += 1) {
    const ratio = tickCount === 1 ? 0 : index / (tickCount - 1);
    const value = minX + spanX * ratio;
    xTicks.push({ position: round2(paddingX + innerW * ratio), label: formatX(value) });
  }

  const yTicks: ChartTick[] = [];
  if (series.constant) {
    yTicks.push({ position: round2(toY(series.min)), label: formatY(series.min) });
  } else {
    for (let index = 0; index < 3; index += 1) {
      const ratio = index / 2;
      const value = series.max - series.span * ratio;
      yTicks.push({ position: round2(toY(value)), label: formatY(value) });
    }
  }

  return { path: path.trim(), alarms, xTicks, yTicks };
}

const round2 = (value: number): number => Math.round(value * 100) / 100;
