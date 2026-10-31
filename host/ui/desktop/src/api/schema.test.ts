import { describe, expect, it } from 'vitest';
import { SchemaError, parseAnalysis, parseEvent, parseEventDetail, parseSummary, parseTelemetry, parseTrends } from './schema';

const validEvent = {
  event_id: 'e1', device_id: 'ed84-001', revision: 1, source: 'LIVE_IMU',
  condition: 'ACTIVE', ack: 'UNACKNOWLEDGED', severity: 'ALARM', diagnosis: 'UNKNOWN',
  quality: 'VALID', peak_deviation: 0.084, updated_ms: 1_700_000_000_000,
};

describe('事件 schema', () => {
  it('接受合法事件并保留服务端时间', () => {
    expect(parseEvent(validEvent).updated_ms).toBe(1_700_000_000_000);
  });

  it('拒绝未知枚举与缺失字段', () => {
    expect(() => parseEvent({ ...validEvent, condition: 'MAYBE' })).toThrow(SchemaError);
    expect(() => parseEvent({ ...validEvent, ack: 'ACKED' })).toThrow(SchemaError);
    expect(() => parseEvent({ ...validEvent, severity: 'CRITICAL' })).toThrow(SchemaError);
    expect(() => parseEvent({ ...validEvent, revision: undefined })).toThrow(SchemaError);
    expect(() => parseEvent(null)).toThrow(SchemaError);
    expect(() => parseEvent([validEvent])).toThrow(SchemaError);
  });

  it('拒绝非有限数值，避免把 NaN 当成有效偏离', () => {
    expect(() => parseEvent({ ...validEvent, peak_deviation: Number.NaN })).toThrow(SchemaError);
    expect(() => parseEvent({ ...validEvent, peak_deviation: 'high' })).toThrow(SchemaError);
  });

  it('详情保留服务端修订顺序', () => {
    const detail = parseEventDetail({
      event_id: 'e1',
      updated_ms: 5,
      revisions: [{ ...validEvent, revision: 1 }, { ...validEvent, revision: 2, condition: 'RECOVERED', server_received_ms: 9 }],
    });
    expect(detail.revisions.map((item) => item.revision)).toEqual([1, 2]);
    expect(detail.revisions[1].server_received_ms).toBe(9);
  });
});

describe('汇总 schema', () => {
  const binding = {
    asset_id: 'motor-01', device_id: 'ed84-001', scene_instance: 'MOTOR_01', zone: 'B',
    detail_model: null, sensor_node: 'IMU_Sensor', display_name_zh: '电机 01', binding_state: 'verified',
  };

  it('接受六区汇总并保留服务端计数', () => {
    const summary = parseSummary({
      assets: [{ ...binding, status: 'ONLINE', last_telemetry_received_ms: 1 }],
      counts: { ONLINE: 1, STALE: 0, UNKNOWN: 0 },
      online_threshold_ms: 30_000,
    });
    expect(summary.assets[0].status).toBe('ONLINE');
    expect(summary.online_threshold_ms).toBe(30_000);
  });

  it('拒绝伪造的资产状态', () => {
    expect(() => parseSummary({
      assets: [{ ...binding, status: 'HEALTHY', last_telemetry_received_ms: null }],
      counts: { ONLINE: 1, STALE: 0, UNKNOWN: 0 },
      online_threshold_ms: 30_000,
    })).toThrow(SchemaError);
    expect(() => parseSummary({ assets: {}, counts: {}, online_threshold_ms: 1 })).toThrow(SchemaError);
    expect(() => parseSummary({ assets: [], counts: { ONLINE: 0, STALE: 0 }, online_threshold_ms: 1 })).toThrow(SchemaError);
  });
});

describe('遥测 schema', () => {
  it('接受缺省的可选字段', () => {
    expect(parseTelemetry({ mono_ms: 5 })).toEqual({ mono_ms: 5 });
  });

  it('拒绝非数值与非法报警标记', () => {
    expect(() => parseTelemetry({ mono_ms: '5' })).toThrow(SchemaError);
    expect(() => parseTelemetry({ mono_ms: 5, rms_vector_g: '0.1' })).toThrow(SchemaError);
    expect(() => parseTelemetry({ mono_ms: 5, alarm: 'yes' })).toThrow(SchemaError);
    expect(() => parseTrends([{ mono_ms: 1 }, { rms: 2 }])).toThrow(SchemaError);
  });
});

describe('分析 schema', () => {
  it('接受明确的不可用状态', () => {
    const analysis = parseAnalysis({ event_id: 'e1', available: false, reason: 'verified evidence analysis is not available' });
    expect(analysis.available).toBe(false);
    expect(analysis.reason).toBeTruthy();
  });

  it('拒绝把可用标记写成字符串', () => {
    expect(() => parseAnalysis({ event_id: 'e1', available: 'false' })).toThrow(SchemaError);
  });
});
