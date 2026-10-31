// SSE 实时刷新（规格 §5.1）：收到消息后 250 ms 去抖刷新一次；
// 断流期间由调用方回退到 10 秒轮询。这里只负责连接与去抖，不读取业务数据。

const DEBOUNCE_MS = 250;
const RECONNECT_MS = 10_000;

export type StreamStatus = 'connecting' | 'open' | 'closed' | 'unsupported';

export type StreamHandlers = {
  /** 去抖后的刷新通知。 */
  onRefresh: () => void;
  onStatus: (status: StreamStatus) => void;
};

export type StreamHandle = { close: () => void };

export function openStream(handlers: StreamHandlers, url = '/api/v1/stream'): StreamHandle {
  let timer = 0;
  let reconnect = 0;
  let closed = false;
  let source: EventSource | null = null;

  const schedule = () => {
    window.clearTimeout(timer);
    timer = window.setTimeout(() => { timer = 0; handlers.onRefresh(); }, DEBOUNCE_MS);
  };

  if (typeof window === 'undefined' || typeof window.EventSource !== 'function') {
    handlers.onStatus('unsupported');
    return { close: () => { closed = true; } };
  }

  const connect = () => {
    if (closed) return;
    handlers.onStatus('connecting');
    try {
      source = new EventSource(url);
    } catch {
      handlers.onStatus('closed');
      scheduleReconnect();
      return;
    }
    source.onopen = () => { if (!closed) handlers.onStatus('open'); };
    source.onmessage = () => { if (!closed) schedule(); };
    source.onerror = () => {
      if (closed) return;
      handlers.onStatus('closed');
      // EventSource 自带重连；readyState 为 CLOSED 时说明它已放弃，这里自建重连。
      if (source && source.readyState === EventSource.CLOSED) scheduleReconnect();
    };
  };

  const scheduleReconnect = () => {
    window.clearTimeout(reconnect);
    reconnect = window.setTimeout(() => {
      if (closed) return;
      source?.close();
      connect();
    }, RECONNECT_MS);
  };

  connect();

  return {
    close: () => {
      closed = true;
      window.clearTimeout(timer);
      window.clearTimeout(reconnect);
      source?.close();
      source = null;
    },
  };
}
