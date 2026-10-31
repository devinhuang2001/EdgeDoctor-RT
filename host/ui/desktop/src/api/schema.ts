// 运行时 schema 校验（规格 §3：API 客户端只返回经过校验的数据）。
// 任何非法值一律抛错并降级为“数据不可用”，绝不用默认值掩盖服务端异常。

import type {
  AnalysisView, AssetBinding, EventDetail, EventRecord, EvidenceMetadata,
  FactorySummary, Snapshot, Telemetry,
} from './types';

export class SchemaError extends Error {
  constructor(path: string) {
    super(`服务端返回的数据格式无效：${path}`);
    this.name = 'SchemaError';
  }
}

const fail = (path: string): never => { throw new SchemaError(path); };

const isPlainObject = (value: unknown): value is Record<string, unknown> => (
  typeof value === 'object' && value !== null && !Array.isArray(value)
);

const object = (value: unknown, path: string): Record<string, unknown> => (
  isPlainObject(value) ? value : fail(path)
);
const str = (value: unknown, path: string): string => (typeof value === 'string' ? value : fail(path));
const nullableStr = (value: unknown, path: string): string | null => (
  value === null || value === undefined ? null : str(value, path)
);
const finite = (value: unknown, path: string): number => (
  typeof value === 'number' && Number.isFinite(value) ? value : fail(path)
);
const nullableNum = (value: unknown, path: string): number | null => (
  value === null || value === undefined ? null : finite(value, path)
);
const bool = (value: unknown, path: string): boolean => (typeof value === 'boolean' ? value : fail(path));
const integer = (value: unknown, path: string): number => (
  typeof value === 'number' && Number.isInteger(value) ? value : fail(path)
);
const enumerated = <T extends string>(value: unknown, choices: readonly T[], path: string): T => (
  typeof value === 'string' && (choices as readonly string[]).includes(value) ? value as T : fail(path)
);
const optional = <T>(value: unknown, path: string, parser: (value: unknown, path: string) => T): T | undefined => (
  value === undefined || value === null ? undefined : parser(value, path)
);

export function parseTelemetry(value: unknown, path = 'telemetry'): Telemetry {
  const raw = object(value, path);
  return {
    mono_ms: finite(raw.mono_ms, `${path}.mono_ms`),
    timestamp_ms: optional(raw.timestamp_ms, `${path}.timestamp_ms`, finite),
    rms_vector_g: optional(raw.rms_vector_g, `${path}.rms_vector_g`, finite),
    anomaly_score: optional(raw.anomaly_score, `${path}.anomaly_score`, finite),
    alarm: optional(raw.alarm, `${path}.alarm`, bool),
    idle: optional(raw.idle, `${path}.idle`, bool),
  };
}

export function parseEvent(value: unknown, path = 'event'): EventRecord {
  const raw = object(value, path);
  return {
    event_id: str(raw.event_id, `${path}.event_id`),
    device_id: str(raw.device_id, `${path}.device_id`),
    revision: finite(raw.revision, `${path}.revision`),
    source: optional(raw.source, `${path}.source`, str),
    condition: enumerated(raw.condition, ['ACTIVE', 'RECOVERED'] as const, `${path}.condition`),
    ack: enumerated(raw.ack, ['UNACKNOWLEDGED', 'ACKNOWLEDGED'] as const, `${path}.ack`),
    severity: optional(raw.severity, `${path}.severity`, (item, itemPath) => enumerated(item, ['WARNING', 'ALARM'] as const, itemPath)),
    diagnosis: optional(raw.diagnosis, `${path}.diagnosis`, str),
    quality: optional(raw.quality, `${path}.quality`, str),
    peak_deviation: optional(raw.peak_deviation, `${path}.peak_deviation`, finite),
    snapshot_id: optional(raw.snapshot_id, `${path}.snapshot_id`, integer),
    updated_ms: optional(raw.updated_ms, `${path}.updated_ms`, finite),
    server_received_ms: optional(raw.server_received_ms, `${path}.server_received_ms`, finite),
  };
}

export function parseBinding(value: unknown, path = 'binding'): AssetBinding {
  const raw = object(value, path);
  return {
    asset_id: str(raw.asset_id, `${path}.asset_id`),
    device_id: nullableStr(raw.device_id, `${path}.device_id`),
    scene_instance: str(raw.scene_instance, `${path}.scene_instance`),
    zone: str(raw.zone, `${path}.zone`),
    detail_model: nullableStr(raw.detail_model, `${path}.detail_model`),
    sensor_node: nullableStr(raw.sensor_node, `${path}.sensor_node`),
    display_name_zh: str(raw.display_name_zh, `${path}.display_name_zh`),
    binding_state: enumerated(raw.binding_state, ['verified', 'provisional', 'unbound'] as const, `${path}.binding_state`),
  };
}

export function parseSummary(value: unknown): FactorySummary {
  const raw = object(value, 'summary');
  const rawAssets = raw.assets;
  if (!Array.isArray(rawAssets)) throw new SchemaError('summary.assets');
  const counts = object(raw.counts, 'summary.counts');
  return {
    assets: rawAssets.map((item, index) => {
      const binding = parseBinding(item, `summary.assets[${index}]`);
      const extra = object(item, `summary.assets[${index}]`);
      return {
        ...binding,
        status: enumerated(extra.status, ['ONLINE', 'STALE', 'UNKNOWN'] as const, `summary.assets[${index}].status`),
        last_telemetry_received_ms: nullableNum(extra.last_telemetry_received_ms, `summary.assets[${index}].last_telemetry_received_ms`),
      };
    }),
    counts: {
      ONLINE: finite(counts.ONLINE, 'summary.counts.ONLINE'),
      STALE: finite(counts.STALE, 'summary.counts.STALE'),
      UNKNOWN: finite(counts.UNKNOWN, 'summary.counts.UNKNOWN'),
    },
    online_threshold_ms: finite(raw.online_threshold_ms, 'summary.online_threshold_ms'),
  };
}

export function parseSnapshot(value: unknown): Snapshot {
  const raw = object(value, 'snapshot');
  return {
    device_id: str(raw.device_id, 'snapshot.device_id'),
    status: enumerated(raw.status, ['NO_DATA', 'ALARM', 'WARNING', 'RECOVERED', 'UNKNOWN'] as const, 'snapshot.status'),
    snapshot_age_ms: optional(raw.snapshot_age_ms, 'snapshot.snapshot_age_ms', finite),
    telemetry: optional(raw.telemetry, 'snapshot.telemetry', parseTelemetry),
    event: optional(raw.event, 'snapshot.event', parseEvent),
    advice: optional(raw.advice, 'snapshot.advice', str),
  };
}

export function parseEvents(value: unknown): EventRecord[] {
  if (!Array.isArray(value)) throw new SchemaError('events');
  return value.map((item, index) => parseEvent(item, `events[${index}]`));
}

export function parseTrends(value: unknown): Telemetry[] {
  if (!Array.isArray(value)) throw new SchemaError('trends');
  return value.map((item, index) => parseTelemetry(item, `trends[${index}]`));
}

export function parseEvidence(value: unknown): EvidenceMetadata {
  const raw = object(value, 'evidence');
  return {
    event_id: str(raw.event_id, 'evidence.event_id'),
    device_id: str(raw.device_id, 'evidence.device_id'),
    bytes: finite(raw.bytes, 'evidence.bytes'),
    crc_ok: bool(raw.crc_ok, 'evidence.crc_ok'),
    created_ms: finite(raw.created_ms, 'evidence.created_ms'),
    download: str(raw.download, 'evidence.download'),
  };
}

export function parseEventDetail(value: unknown): EventDetail {
  const raw = object(value, 'eventDetail');
  return {
    event_id: str(raw.event_id, 'eventDetail.event_id'),
    updated_ms: nullableNum(raw.updated_ms, 'eventDetail.updated_ms'),
    revisions: parseEvents(raw.revisions),
  };
}

export function parseAnalysis(value: unknown): AnalysisView {
  const raw = object(value, 'analysis');
  const result: AnalysisView = {
    event_id: str(raw.event_id, 'analysis.event_id'),
    available: bool(raw.available, 'analysis.available'),
    reason: optional(raw.reason, 'analysis.reason', str),
  };
  if (!result.available) return result;
  if (raw.crc_ok !== true || raw.unit !== 'g' || raw.parser_version !== 'edv2-xyz-v1') fail('analysis.verification');
  const rate = finite(raw.sample_rate_hz, 'analysis.sample_rate_hz');
  const count = integer(raw.sample_count, 'analysis.sample_count');
  if (rate <= 0 || rate > 32000 || count < 4 || count > 2730) fail('analysis.bounds');
  const vector = (input: unknown, path: string): number[] => {
    if (!Array.isArray(input) || input.length < 2 || input.length > 4097) return fail(path);
    return input.map((item, i) => finite(item, `${path}[${i}]`));
  };
  const series = (input: unknown, axis: string, expected?: number) => {
    const item = object(input, `analysis.${axis}`);
    const coordinates = vector(item[axis], `analysis.${axis}`);
    if (expected !== undefined && coordinates.length !== expected) fail(`analysis.${axis}.length`);
    if (coordinates[0] !== 0 || coordinates.some((point, i) => i > 0 && point <= coordinates[i - 1])) fail(`analysis.${axis}.order`);
    const values = { x_g: vector(item.x_g, 'analysis.x_g'), y_g: vector(item.y_g, 'analysis.y_g'), z_g: vector(item.z_g, 'analysis.z_g') };
    if (Object.values(values).some((points) => points.length !== coordinates.length)) fail('analysis.axis.length');
    return { coordinates, ...values };
  };
  const { coordinates: times, ...waveform } = series(raw.waveform, 'time_s', count);
  const { coordinates: frequencies, ...spectrum } = series(raw.spectrum, 'frequency_hz');
  const { coordinates: envelopeTimes, ...envelope } = series(raw.envelope, 'time_s', count);
  if (frequencies.some((frequency) => frequency > rate / 2) || Object.values(spectrum).some((points) => points.some((point) => point < 0))) fail('analysis.spectrum.bounds');
  if (times.some((time, i) => Math.abs(time - i / rate) > 1e-6 || time !== envelopeTimes[i])) fail('analysis.time_s');
  const stats = object(raw.statistics, 'analysis.statistics');
  const axisStats = (axis: string) => {
    const item = object(stats[axis], `analysis.statistics.${axis}`);
    return { mean_g: finite(item.mean_g, 'mean_g'), rms_ac_g: finite(item.rms_ac_g, 'rms_ac_g'), peak_ac_g: finite(item.peak_ac_g, 'peak_ac_g'), crest_factor: nullableNum(item.crest_factor, 'crest_factor'), kurtosis: nullableNum(item.kurtosis, 'kurtosis') };
  };
  if (!Array.isArray(raw.dominant_peaks) || raw.dominant_peaks.length > 10) fail('analysis.dominant_peaks');
  const peaks = (raw.dominant_peaks as unknown[]).map((input) => {
    const item = object(input, 'analysis.peak');
    const frequency = finite(item.frequency_hz, 'peak.frequency_hz');
    const amplitude = finite(item.amplitude_g, 'peak.amplitude_g');
    if (frequency < 0 || frequency > rate / 2 || amplitude < 0) fail('analysis.peak.bounds');
    return { axis: enumerated(item.axis, ['X', 'Y', 'Z'], 'peak.axis'), frequency_hz: frequency, amplitude_g: amplitude };
  });
  result.data = {
    parser_version: str(raw.parser_version, 'parser_version'), sample_rate_hz: rate, sample_rate_kind: str(raw.sample_rate_kind, 'sample_rate_kind'), sample_count: count,
    waveform: { time_s: times, ...waveform }, spectrum: { frequency_hz: frequencies, ...spectrum },
    envelope: { time_s: envelopeTimes, method: enumerated(object(raw.envelope, 'envelope').method, ['rectified_ac_moving_mean'], 'envelope.method'), ...envelope },
    statistics: { x: axisStats('x'), y: axisStats('y'), z: axisStats('z') }, dominant_peaks: peaks,
  };
  return result;
}
