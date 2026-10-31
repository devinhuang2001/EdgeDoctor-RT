// 数值与时间格式化。缺失值一律显示为显式占位符，不用 0 或示例值填充。

export const PLACEHOLDER = '—';

export function fixed(value: number | null | undefined, digits = 3): string {
  return typeof value === 'number' && Number.isFinite(value) ? value.toFixed(digits) : PLACEHOLDER;
}

export function integer(value: number | null | undefined): string {
  return typeof value === 'number' && Number.isFinite(value) ? String(Math.round(value)) : PLACEHOLDER;
}

export function percent(value: number | null | undefined, digits = 1): string {
  return typeof value === 'number' && Number.isFinite(value) ? `${(value * 100).toFixed(digits)}%` : PLACEHOLDER;
}

export function ageText(timestampMs: number | null | undefined, nowMs: number): string {
  if (typeof timestampMs !== 'number' || !Number.isFinite(timestampMs)) return '暂无接收记录';
  const seconds = Math.max(0, Math.round((nowMs - timestampMs) / 1000));
  if (seconds < 60) return `${seconds} 秒前`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes} 分钟前`;
  const hours = Math.floor(minutes / 60);
  if (hours < 24) return `${hours} 小时前`;
  return `${Math.floor(hours / 24)} 天前`;
}

const pad = (value: number): string => String(value).padStart(2, '0');

/** 本地时间的 HH:mm:ss，用于图表轴与事件时间。 */
export function clockText(timestampMs: number | null | undefined): string {
  if (typeof timestampMs !== 'number' || !Number.isFinite(timestampMs)) return PLACEHOLDER;
  const date = new Date(timestampMs);
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

export function dateTimeText(timestampMs: number | null | undefined): string {
  if (typeof timestampMs !== 'number' || !Number.isFinite(timestampMs)) return PLACEHOLDER;
  const date = new Date(timestampMs);
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${clockText(timestampMs)}`;
}

export function durationText(ms: number | null | undefined): string {
  if (typeof ms !== 'number' || !Number.isFinite(ms) || ms < 0) return PLACEHOLDER;
  const seconds = Math.round(ms / 1000);
  if (seconds < 60) return `${seconds} 秒`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes} 分 ${seconds % 60} 秒`;
  return `${Math.floor(minutes / 60)} 小时 ${minutes % 60} 分`;
}

export function bytesText(bytes: number | null | undefined): string {
  if (typeof bytes !== 'number' || !Number.isFinite(bytes)) return PLACEHOLDER;
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
}

/** 服务端枚举的直译；未知取值原样显示，避免伪造语义。 */
export function diagnosisText(diagnosis: string | undefined): string {
  switch (diagnosis) {
    case 'NO_BASELINE': return '尚无基线';
    case 'BASELINE_MISMATCH': return '与基线不符';
    case 'WITHIN_BASELINE': return '基线范围内';
    case 'UNKNOWN_ANOMALY': return '未分类偏离';
    case 'KNOWN_CLASS': return '服务端已分类';
    case 'UNCERTAIN': return '不确定';
    case 'UNAVAILABLE': return '诊断不可用';
    case 'UNKNOWN': return '未给出诊断类别';
    default: return diagnosis ?? PLACEHOLDER;
  }
}

export function qualityText(quality: string | undefined): string {
  switch (quality) {
    case 'WARMUP': return '预热中';
    case 'VALID': return '有效';
    case 'STALE': return '数据过期';
    case 'DISCONNECTED': return '已断连';
    case 'SATURATED': return '饱和';
    case 'GAPPED': return '有缺样';
    case 'CONFIG_ERROR': return '配置错误';
    default: return quality ?? PLACEHOLDER;
  }
}

export function sourceText(source: string | undefined): string {
  switch (source) {
    case 'LIVE_IMU': return '实时 IMU';
    case 'SENSOR': return '传感器';
    case 'STORAGE': return '本地存储';
    case 'TREND': return '趋势统计';
    default: return source ?? PLACEHOLDER;
  }
}
