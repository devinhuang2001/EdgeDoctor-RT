// 服务端契约的前端侧类型。所有字段都对应真实服务端返回，前端不补充推断字段。

export type BindingState = 'verified' | 'provisional' | 'unbound';
export type AssetStatus = 'ONLINE' | 'STALE' | 'UNKNOWN';

export type AssetBinding = {
  asset_id: string;
  /** 仅 verified（以及在途待核验的 provisional）绑定才可能非空。 */
  device_id: string | null;
  scene_instance: string;
  zone: string;
  detail_model: string | null;
  sensor_node: string | null;
  display_name_zh: string;
  binding_state: BindingState;
};

export type FactoryAsset = AssetBinding & {
  /** 由服务端按冻结阈值计算，前端不得改写。 */
  status: AssetStatus;
  last_telemetry_received_ms: number | null;
};

export type FactorySummary = {
  assets: FactoryAsset[];
  counts: Record<AssetStatus, number>;
  online_threshold_ms: number;
};

export type Telemetry = {
  mono_ms: number;
  timestamp_ms?: number;
  rms_vector_g?: number;
  /* 静置或缺少基线时服务端不会给 anomaly_score：没有偏离度，而不是偏离度为零。 */
  anomaly_score?: number;
  alarm?: boolean;
  idle?: boolean;
};

export type EventCondition = 'ACTIVE' | 'RECOVERED';
export type AckState = 'UNACKNOWLEDGED' | 'ACKNOWLEDGED';
export type Severity = 'WARNING' | 'ALARM';

export type EventRecord = {
  event_id: string;
  device_id: string;
  revision: number;
  source?: string;
  condition: EventCondition;
  ack: AckState;
  severity?: Severity;
  diagnosis?: string;
  quality?: string;
  peak_deviation?: number;
  snapshot_id?: number;
  /** 服务端接收时间，列表项携带。 */
  updated_ms?: number;
  /** 服务端接收该修订的时间，事件详情携带。 */
  server_received_ms?: number;
};

export type SnapshotStatus = 'NO_DATA' | 'ALARM' | 'WARNING' | 'RECOVERED' | 'UNKNOWN';

export type Snapshot = {
  device_id: string;
  status: SnapshotStatus;
  /** 最近事件距现在的毫秒数；无事件时为 null。 */
  snapshot_age_ms?: number | null;
  telemetry?: Telemetry | null;
  event?: EventRecord | null;
  advice?: string;
};

export type EvidenceMetadata = {
  event_id: string;
  device_id: string;
  bytes: number;
  crc_ok: boolean;
  created_ms: number;
  download: string;
};

export type EventDetail = {
  event_id: string;
  updated_ms: number | null;
  revisions: EventRecord[];
};

export type AnalysisView = {
  event_id: string;
  available: boolean;
  reason?: string;
  data?: AnalysisData;
};

export type AxisSeries = { x_g: number[]; y_g: number[]; z_g: number[] };
export type AxisStatistics = { mean_g: number; rms_ac_g: number; peak_ac_g: number; crest_factor: number | null; kurtosis: number | null };
export type AnalysisData = {
  parser_version: string; sample_rate_hz: number; sample_rate_kind: string; sample_count: number;
  waveform: AxisSeries & { time_s: number[] };
  spectrum: AxisSeries & { frequency_hz: number[] };
  envelope: AxisSeries & { time_s: number[]; method: string };
  statistics: Record<'x' | 'y' | 'z', AxisStatistics>;
  dominant_peaks: { axis: string; frequency_hz: number; amplitude_g: number }[];
};

export type CommandStatus = 'PENDING' | 'APPLIED' | 'REJECTED' | 'EXPIRED';

export type CommandView = {
  command_id: string;
  status: CommandStatus;
  detail?: string;
};

export type ActionKind = 'ack' | 'evidence';
