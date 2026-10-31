// 按设备读取趋势序列。事件中心与信号分析页关注的可能不是当前路由设备，
// 因此这里独立取数，并把设备号一起存进结果，避免切换设备时闪现上一台设备的数据。

import { useEffect, useRef, useState } from 'react';
import { ApiError, api, isAbortError } from '../api/client';
import type { Telemetry } from '../api/types';

export type DeviceSeries = {
  samples: Telemetry[];
  loading: boolean;
  error: ApiError | null;
};

type Result = { deviceId: string; samples: Telemetry[]; error: ApiError | null };

export function useDeviceSeries(deviceId: string | null, limit = 2000): DeviceSeries {
  const [result, setResult] = useState<Result | null>(null);
  const generation = useRef(0);

  useEffect(() => {
    const current = ++generation.current;
    if (!deviceId) return undefined;
    const controller = new AbortController();
    api.trends(deviceId, limit, controller.signal)
      .then((samples) => {
        if (generation.current !== current) return;
        setResult({ deviceId, samples, error: null });
      })
      .catch((cause: unknown) => {
        if (generation.current !== current || isAbortError(cause)) return;
        setResult({ deviceId, samples: [], error: cause instanceof ApiError ? cause : null });
      });
    return () => controller.abort();
  }, [deviceId, limit]);

  if (!deviceId) return { samples: [], loading: false, error: null };
  if (!result || result.deviceId !== deviceId) return { samples: [], loading: true, error: null };
  return { samples: result.samples, loading: false, error: result.error };
}
