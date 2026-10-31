import { describe, expect, it } from 'vitest';
import { isMeasurableFrequency, nyquistLimitHz, spectrumAxisMaxHz } from './spectrum';

describe('频谱轴规则', () => {
  it('上限等于采样率的一半', () => {
    expect(nyquistLimitHz(208)).toBe(104);
    expect(nyquistLimitHz(1000)).toBe(500);
  });

  it('缺少采样率时不给上限，调用方必须显示不可用', () => {
    expect(nyquistLimitHz(undefined)).toBeNull();
    expect(nyquistLimitHz(null)).toBeNull();
    expect(nyquistLimitHz(0)).toBeNull();
    expect(nyquistLimitHz(Number.NaN)).toBeNull();
    expect(spectrumAxisMaxHz(undefined)).toBeNull();
  });

  it('请求的上限高于奈奎斯特时被裁剪', () => {
    expect(spectrumAxisMaxHz(208, 400)).toBe(104);
    expect(spectrumAxisMaxHz(208, 80)).toBe(80);
    expect(spectrumAxisMaxHz(208)).toBe(104);
  });

  it('208 Hz 数据不得出现 104 Hz 以上的实测频点', () => {
    expect(isMeasurableFrequency(42.3, 208)).toBe(true);
    expect(isMeasurableFrequency(104, 208)).toBe(true);
    expect(isMeasurableFrequency(104.1, 208)).toBe(false);
    expect(isMeasurableFrequency(500, 208)).toBe(false);
    expect(isMeasurableFrequency(42.3, null)).toBe(false);
  });
});
