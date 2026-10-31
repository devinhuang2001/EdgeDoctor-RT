import { describe, expect, it } from 'vitest';
import type { Telemetry } from '../api/types';
import { alarmPoints, chartGeometry, rmsSummary, trendSeries, windowByDeviceClock } from './trend';

const sample = (mono: number, rms?: number, alarm?: boolean): Telemetry => ({
  mono_ms: mono,
  ...(rms === undefined ? {} : { rms_vector_g: rms }),
  ...(alarm === undefined ? {} : { alarm }),
});

describe('trendSeries', () => {
  it('只把有限的 rms_vector_g 视为有效采样', () => {
    const series = trendSeries([
      sample(0, 0.01),
      sample(1000),
      sample(2000, Number.NaN),
      sample(3000, 0.03, true),
    ]);
    expect(series.valid).toHaveLength(2);
    expect(series.points[1]).toBeNull();
    expect(series.points[2]).toBeNull();
    expect(series.min).toBeCloseTo(0.01);
    expect(series.max).toBeCloseTo(0.03);
  });

  it('缺样保持为 null，不补 0', () => {
    const series = trendSeries([sample(0, 1), sample(1), sample(2, 1)]);
    expect(series.points.map((point) => (point ? point.y : null))).toEqual([1, null, 1]);
  });

  it('单点序列不产生 NaN 路径', () => {
    const geometry = chartGeometry(trendSeries([sample(10, 0.02)]));
    expect(geometry.path).toMatch(/^M[\d.]+,[\d.]+$/);
    expect(geometry.path).not.toMatch(/NaN/);
  });

  it('恒定值走垂直中线且坐标有限', () => {
    const series = trendSeries([sample(0, 0.05), sample(1, 0.05), sample(2, 0.05)]);
    expect(series.constant).toBe(true);
    const geometry = chartGeometry(series);
    expect(geometry.path).not.toMatch(/NaN|Infinity/);
    const ys = geometry.path.split(' ').map((segment) => Number(segment.split(',')[1]));
    expect(new Set(ys).size).toBe(1);
    expect(ys[0]).toBeCloseTo(50, 1);
  });

  it('超长序列仍然生成有限坐标', () => {
    const samples = Array.from({ length: 2000 }, (_, index) => sample(index * 10, Math.sin(index / 30) * 0.02 + 0.05));
    const geometry = chartGeometry(trendSeries(samples));
    expect(geometry.path).not.toMatch(/NaN|Infinity/);
    expect(geometry.path.split(' ').length).toBe(2000);
  });

  it('报警点只来自真实 alarm 字段', () => {
    const series = trendSeries([sample(0, 0.01), sample(1, 0.9), sample(2, 0.02, true)]);
    expect(alarmPoints(series)).toHaveLength(1);
    expect(alarmPoints(series)[0].y).toBeCloseTo(0.02);
  });
});

describe('windowByDeviceClock', () => {
  it('按设备单调时钟截取最近窗口', () => {
    const samples = [sample(0, 0.01), sample(60_000, 0.02), sample(120_000, 0.03)];
    expect(windowByDeviceClock(samples, 60_000).map((item) => item.mono_ms)).toEqual([60_000, 120_000]);
    expect(windowByDeviceClock(samples, null)).toHaveLength(3);
  });

  it('乱序输入会按时钟排序', () => {
    const samples = [sample(2000, 0.02), sample(0, 0.01), sample(1000, 0.015)];
    expect(windowByDeviceClock(samples, null).map((item) => item.mono_ms)).toEqual([0, 1000, 2000]);
  });
});

describe('rmsSummary', () => {
  it('没有有效采样时全部为 null', () => {
    expect(rmsSummary([sample(0), sample(1)])).toEqual({ count: 0, last: null, peak: null, min: null, windowMs: null });
  });

  it('返回真实窗口统计', () => {
    const summary = rmsSummary([sample(0, 0.01), sample(500, 0.03), sample(1000, 0.02)]);
    expect(summary.count).toBe(3);
    expect(summary.last).toBeCloseTo(0.02);
    expect(summary.peak).toBeCloseTo(0.03);
    expect(summary.min).toBeCloseTo(0.01);
    expect(summary.windowMs).toBe(1000);
  });
});
