// 哈希路由（规格 §4）：#/factory、#/monitor/:deviceId、#/events、#/analysis/:deviceId。
// 使用哈希是为了让同一份静态产物在 /dashboard-v2/ 下深链接刷新也能恢复。

import { useCallback, useEffect, useState } from 'react';

export type Page = 'factory' | 'monitor' | 'events' | 'analysis';
export type Route = { page: Page; deviceId?: string };

export const PAGE_LABEL: Record<Page, string> = {
  factory: '工厂总览',
  monitor: '实时监测',
  events: '事件中心',
  analysis: '信号分析',
};

const PAGES: Page[] = ['factory', 'monitor', 'events', 'analysis'];

export function parseHash(hash: string): Route {
  const segments = hash.replace(/^#\/?/, '').split('/').filter(Boolean);
  const page = segments[0] as Page;
  if (!PAGES.includes(page)) return { page: 'factory' };
  const deviceId = segments[1] ? decodeURIComponent(segments[1]) : undefined;
  return deviceId ? { page, deviceId } : { page };
}

export function routeHash(route: Route): string {
  return `#/${route.page}${route.deviceId ? `/${encodeURIComponent(route.deviceId)}` : ''}`;
}

export function navigate(route: Route): void {
  const next = routeHash(route);
  if (window.location.hash !== next) window.location.hash = next;
}

export function useRoute(): [Route, (route: Route) => void] {
  const [route, setRoute] = useState<Route>(() => parseHash(window.location.hash));

  useEffect(() => {
    const update = () => setRoute(parseHash(window.location.hash));
    window.addEventListener('hashchange', update);
    update();
    return () => window.removeEventListener('hashchange', update);
  }, []);

  const go = useCallback((next: Route) => {
    const hash = routeHash(next);
    if (window.location.hash === hash) setRoute(parseHash(hash));
    else window.location.hash = hash;
  }, []);

  return [route, go];
}
