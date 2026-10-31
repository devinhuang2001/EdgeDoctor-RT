import { describe, expect, it } from 'vitest';
import { parseAnalysis, SchemaError } from './schema';

const xyz = { x_g: [0, 1, 0, -1], y_g: [0, 0, 0, 0], z_g: [1, 1, 1, 1] };
const statistics = { mean_g: 0, rms_ac_g: 0, peak_ac_g: 0, crest_factor: null, kurtosis: null };
const valid = {
  event_id: 'e1', available: true, crc_ok: true, unit: 'g', parser_version: 'edv2-xyz-v1', sample_rate_hz: 8, sample_rate_kind: 'nominal', sample_count: 4,
  waveform: { time_s: [0, 0.125, 0.25, 0.375], ...xyz },
  spectrum: { frequency_hz: [0, 2, 4], x_g: [0, 1, 0], y_g: [0, 0, 0], z_g: [0, 0, 0] },
  envelope: { time_s: [0, 0.125, 0.25, 0.375], method: 'rectified_ac_moving_mean', ...xyz },
  statistics: { x: statistics, y: statistics, z: statistics }, dominant_peaks: [{ axis: 'X', frequency_hz: 2, amplitude_g: 1 }],
};
describe('EDV2 analysis contract', () => {
  it('preserves verified axes and nullable constant-wave statistics', () => {
    expect(parseAnalysis(valid).data?.waveform.z_g).toEqual([1, 1, 1, 1]);
    expect(parseAnalysis(valid).data?.statistics.z.kurtosis).toBeNull();
  });
  it('rejects unverified data, unknown parsers, illegal axes, and beyond-Nyquist spectra', () => {
    for (const broken of [{ ...valid, crc_ok: false }, { ...valid, parser_version: 'other' }, { ...valid, waveform: { ...valid.waveform, x_g: [1] } }, { ...valid, spectrum: { ...valid.spectrum, frequency_hz: [0, 2, 5] } }, { ...valid, sample_rate_hz: NaN }, { ...valid, waveform: { ...valid.waveform, time_s: [0, 0.1, 0.2, 0.3] } }]) {
      expect(() => parseAnalysis(broken)).toThrow(SchemaError);
    }
  });
});
