// 频谱轴规则（规格 §7）：横轴上限由证据的实际采样率决定，且不超过奈奎斯特频率。
// 没有已校验证据时返回 null，调用方必须显示“不可用”，不得回退到示例频段。

export function nyquistLimitHz(sampleRateHz: number | null | undefined): number | null {
  return typeof sampleRateHz === 'number' && Number.isFinite(sampleRateHz) && sampleRateHz > 0
    ? sampleRateHz / 2
    : null;
}

export function spectrumAxisMaxHz(sampleRateHz: number | null | undefined, requestedMaxHz?: number | null): number | null {
  const nyquist = nyquistLimitHz(sampleRateHz);
  if (nyquist === null) return null;
  if (typeof requestedMaxHz !== 'number' || !Number.isFinite(requestedMaxHz) || requestedMaxHz <= 0) return nyquist;
  return Math.min(nyquist, requestedMaxHz);
}

/** 频点是否落在可测量范围内。 */
export function isMeasurableFrequency(frequencyHz: number, sampleRateHz: number | null | undefined): boolean {
  const nyquist = nyquistLimitHz(sampleRateHz);
  return nyquist !== null && Number.isFinite(frequencyHz) && frequencyHz >= 0 && frequencyHz <= nyquist;
}
