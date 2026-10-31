// API 客户端：区分错误类别，令牌只保存在页面内存，绝不写入存储、URL 或日志。

import { SchemaError, parseAnalysis, parseBinding, parseEventDetail, parseEvents, parseEvidence, parseSnapshot, parseSummary, parseTrends } from './schema';
import type { AnalysisView, AssetBinding, CommandView, EventDetail, EventRecord, EvidenceMetadata, FactorySummary, Snapshot, Telemetry } from './types';

export type ApiErrorKind =
  | 'network' | 'timeout' | 'aborted'
  | 'auth' | 'forbidden' | 'not-found' | 'conflict' | 'rate-limited' | 'server' | 'client' | 'schema';

export class ApiError extends Error {
  readonly kind: ApiErrorKind;
  readonly status: number;
  readonly detail: string;

  constructor(kind: ApiErrorKind, message: string, status = 0, detail = '') {
    super(message);
    this.name = 'ApiError';
    this.kind = kind;
    this.status = status;
    this.detail = detail;
  }

  /** 是否值得提示用户重试（规格 §8：API 失败都显示可恢复错误）。 */
  get retryable(): boolean {
    return this.kind !== 'aborted' && this.kind !== 'auth' && this.kind !== 'not-found';
  }
}

export const isAbortError = (error: unknown): boolean => (
  error instanceof ApiError ? error.kind === 'aborted' : (error as { name?: string } | null)?.name === 'AbortError'
);

const MESSAGES: Record<ApiErrorKind, string> = {
  network: '无法连接本地服务，请确认服务进程仍在运行。',
  timeout: '服务端响应超时，请稍后重试。',
  aborted: '请求已取消。',
  auth: '服务端要求 API 令牌。请在顶部输入令牌后重试。',
  forbidden: '当前令牌无权执行该操作。',
  'not-found': '服务端没有该记录。',
  conflict: '服务端记录了更新的修订，页面数据已过期。',
  'rate-limited': '请求过于频繁，已限流，请稍后重试。',
  server: '服务端内部错误，请查看服务日志。',
  client: '请求参数被服务端拒绝。',
  schema: '服务端返回的数据格式不符合约定，已按“数据不可用”处理。',
};

const REQUEST_TIMEOUT_MS = 12_000;

let apiToken = '';

/** 令牌仅保存在当前页面内存中，刷新即失效。 */
export function setApiToken(token: string): void { apiToken = token; }
export function getApiToken(): string { return apiToken; }

function classify(status: number, detail: string): ApiError {
  const kind: ApiErrorKind = status === 401 ? 'auth'
    : status === 403 ? 'forbidden'
      : status === 404 ? 'not-found'
        : status === 409 ? 'conflict'
          : status === 429 ? 'rate-limited'
            : status >= 500 ? 'server'
              : 'client';
  const suffix = detail ? `（${detail}）` : '';
  return new ApiError(kind, `${MESSAGES[kind]}${suffix}`, status, detail);
}

async function readDetail(response: Response): Promise<string> {
  try {
    const body: unknown = await response.json();
    if (body && typeof body === 'object' && 'detail' in body) {
      const detail = (body as { detail: unknown }).detail;
      if (typeof detail === 'string') return detail;
      return JSON.stringify(detail);
    }
  } catch { /* 非 JSON 错误体 */ }
  return '';
}

type RequestOptions = {
  method?: string;
  body?: unknown;
  signal?: AbortSignal;
  timeoutMs?: number;
};

async function request<T>(path: string, parse: (value: unknown) => T, options: RequestOptions = {}): Promise<T> {
  const { method = 'GET', body, signal, timeoutMs = REQUEST_TIMEOUT_MS } = options;
  const controller = new AbortController();
  const timer = window.setTimeout(() => controller.abort(new DOMException('timeout', 'TimeoutError')), timeoutMs);
  const onAbort = () => controller.abort(new DOMException('aborted', 'AbortError'));
  signal?.addEventListener('abort', onAbort);

  const headers = new Headers({ Accept: 'application/json' });
  if (apiToken) headers.set('X-Edgedoctor-Token', apiToken);
  if (body !== undefined) headers.set('Content-Type', 'application/json');

  let response: Response;
  try {
    response = await fetch(path, {
      method,
      headers,
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: controller.signal,
      credentials: 'same-origin',
    });
  } catch (cause) {
    const name = (cause as { name?: string } | null)?.name;
    if (name === 'AbortError' && signal?.aborted) throw new ApiError('aborted', MESSAGES.aborted);
    if (name === 'TimeoutError') throw new ApiError('timeout', MESSAGES.timeout);
    if (name === 'AbortError') throw new ApiError('timeout', MESSAGES.timeout);
    throw new ApiError('network', MESSAGES.network);
  } finally {
    window.clearTimeout(timer);
    signal?.removeEventListener('abort', onAbort);
  }

  if (!response.ok) throw classify(response.status, await readDetail(response));

  let payload: unknown;
  try {
    payload = await response.json();
  } catch {
    throw new ApiError('schema', MESSAGES.schema, response.status, 'JSON 解析失败');
  }
  try {
    return parse(payload);
  } catch (cause) {
    const message = cause instanceof SchemaError ? cause.message : MESSAGES.schema;
    throw new ApiError('schema', message, response.status, message);
  }
}

export const api = {
  ready: (signal?: AbortSignal) => request<{ status: string }>('/health/ready', (value) => value as { status: string }, { signal, timeoutMs: 5000 }),

  summary: (signal?: AbortSignal) => request<FactorySummary>('/api/v1/factory/summary', parseSummary, { signal }),

  binding: (assetId: string, signal?: AbortSignal) => request<AssetBinding>(
    `/api/v1/assets/${encodeURIComponent(assetId)}/binding`, parseBinding, { signal },
  ),

  snapshot: (deviceId: string, signal?: AbortSignal) => request<Snapshot>(
    `/api/v1/query/status?device_id=${encodeURIComponent(deviceId)}`, parseSnapshot, { signal },
  ),

  trends: (deviceId: string, limit = 2000, signal?: AbortSignal) => request<Telemetry[]>(
    `/api/v1/trends?device_id=${encodeURIComponent(deviceId)}&limit=${limit}`, parseTrends, { signal },
  ),

  events: (deviceId: string | undefined, limit = 50, signal?: AbortSignal) => request<EventRecord[]>(
    `/api/v1/events?limit=${limit}${deviceId ? `&device_id=${encodeURIComponent(deviceId)}` : ''}`, parseEvents, { signal },
  ),

  event: (eventId: string, signal?: AbortSignal) => request<EventDetail>(
    `/api/v1/events/${encodeURIComponent(eventId)}`, parseEventDetail, { signal },
  ),

  evidence: (eventId: string, signal?: AbortSignal) => request<EvidenceMetadata>(
    `/api/v1/events/${encodeURIComponent(eventId)}/evidence`, parseEvidence, { signal },
  ),

  analysis: (eventId: string, signal?: AbortSignal) => request<AnalysisView>(
    `/api/v1/events/${encodeURIComponent(eventId)}/analysis`, parseAnalysis, { signal },
  ),

  acknowledge: (eventId: string, revision: number, actor: string, signal?: AbortSignal) => request<{ command_id: string; status: string }>(
    `/api/v1/events/${encodeURIComponent(eventId)}/ack`,
    (value) => value as { command_id: string; status: string },
    { method: 'POST', body: { actor, expected_revision: revision }, signal },
  ),

  requestEvidence: (eventId: string, signal?: AbortSignal) => request<{ command_id: string; status: string }>(
    `/api/v1/events/${encodeURIComponent(eventId)}/evidence-request`,
    (value) => value as { command_id: string; status: string },
    { method: 'POST', signal },
  ),

  command: (commandId: string, signal?: AbortSignal) => request<CommandView>(
    `/api/v1/commands/${encodeURIComponent(commandId)}`,
    (value) => {
      const raw = value as { command_id: string; status: string; detail?: string };
      return { command_id: raw.command_id, status: raw.status as CommandView['status'], detail: raw.detail };
    },
    { signal },
  ),
};
