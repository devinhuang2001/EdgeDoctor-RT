// 页面壳：路由、请求上下文、SSE/轮询回退与全局提示。
// 业务数据一律来自状态仓，页面不自行取数（规格 §3 的分层要求）。

import { useCallback, useEffect, useRef } from 'react';
import { dashboard, useDashboard } from './state/store';
import { openStream } from './state/stream';
import { navigate, PAGE_LABEL, useRoute } from './router';
import type { Route } from './router';
import { useIsDesktop } from './lib/useNow';
import { BottomNav, DesktopGuard, ToastStack, TopBar } from './components/shell';
import { FactoryPage } from './pages/FactoryPage';
import { MonitorPage } from './pages/MonitorPage';
import { EventsPage } from './pages/EventsPage';
import { AnalysisPage } from './pages/AnalysisPage';

const POLL_CONNECTED_MS = 60_000;
const POLL_DISCONNECTED_MS = 10_000;

export default function App() {
  const [route, go] = useRoute();
  const state = useDashboard();
  const isDesktop = useIsDesktop();
  const firstContext = useRef(true);

  // 路由 → 请求上下文：实时监测与信号分析绑定到具体设备。
  useEffect(() => {
    const needsDevice = route.page === 'monitor' || route.page === 'analysis';
    dashboard.setContext({
      deviceId: needsDevice ? route.deviceId ?? null : null,
      eventsScope: route.page === 'events' ? 'all' : 'device',
    });
    document.title = `${PAGE_LABEL[route.page]} · EdgeDoctor-RT 控制台`;
  }, [route.page, route.deviceId]);

  // 首次进入与手动刷新
  useEffect(() => {
    if (!firstContext.current) return;
    firstContext.current = false;
    void dashboard.refresh('INITIAL');
  }, []);

  // SSE 实时刷新；断流后回退到 10 秒轮询，连通时保留低频兜底轮询。
  useEffect(() => {
    const handle = openStream({
      onRefresh: () => { if (!document.hidden) void dashboard.refresh('SSE'); },
      onStatus: (status) => dashboard.setStreamStatus(status),
    });
    return () => handle.close();
  }, []);

  useEffect(() => {
    const interval = state.streamStatus === 'open' ? POLL_CONNECTED_MS : POLL_DISCONNECTED_MS;
    const timer = window.setInterval(() => {
      if (!document.hidden) void dashboard.refresh(state.streamStatus === 'open' ? 'SSE' : 'POLLING');
    }, interval);
    return () => window.clearInterval(timer);
  }, [state.streamStatus]);

  // 页面不可见时暂停刷新，回到前台立刻补一次。
  useEffect(() => {
    const onVisible = () => { if (!document.hidden) void dashboard.refresh('MANUAL'); };
    document.addEventListener('visibilitychange', onVisible);
    return () => document.removeEventListener('visibilitychange', onVisible);
  }, []);

  // 事件列表变化后保证有选中项，避免详情区空白。
  useEffect(() => {
    if (state.events.length === 0) {
      if (state.selectedEventId) dashboard.selectEvent(null);
      return;
    }
    const exists = state.events.some((event) => event.event_id === state.selectedEventId);
    if (!exists) dashboard.selectEvent(state.events[0].event_id);
  }, [state.events, state.selectedEventId]);

  const activeDeviceId = state.deviceId
    ?? state.summary?.assets.find((asset) => asset.asset_id === state.selectedAssetId)?.device_id
    ?? state.summary?.assets.find((asset) => asset.device_id)?.device_id
    ?? null;

  const streamConnected = state.streamStatus === 'open';
  const refresh = useCallback(() => { void dashboard.refresh('MANUAL'); }, []);
  const openMonitor = useCallback((deviceId: string) => navigate({ page: 'monitor', deviceId }), []);
  const openEvents = useCallback((deviceId?: string) => navigate({ page: 'events', deviceId }), []);

  const factoryActions = {
    selectAsset: (assetId: string) => dashboard.selectAsset(assetId),
    setZoneFilter: (zone: string | null) => dashboard.setZoneFilter(zone),
    openMonitor,
    openEvents: (deviceId: string) => openEvents(deviceId),
    refresh,
  };

  return (
    <div className="app-shell">
      <TopBar
        state={state}
        route={route}
        onNavigate={(next: Route) => go(next)}
        onSetToken={(token) => dashboard.setToken(token)}
      />

      <main className="app-main">
        <div className="page-head">
          <div>
            <p className="eyebrow">EdgeDoctor · 电脑版控制台</p>
            <h1>{PAGE_LABEL[route.page]}</h1>
            <p className="page-sub">
              {route.page === 'factory' && '六区分区状态与三维总览，选择设备后查看服务端状态与事件。'}
              {route.page === 'monitor' && '设备实时状态、真实 RMS 趋势与最近事件的闭环操作。'}
              {route.page === 'events' && '事件列表、服务端修订、确认与留证请求的完整状态。'}
              {route.page === 'analysis' && '可验证特征与已校验证据分析；无证据时明确说明不可用。'}
            </p>
          </div>
          <div className="page-head-actions">
            <span className="faint" style={{ fontSize: 11.5 }}>
              {streamConnected
                ? '实时推送已连接（兜底轮询 60 秒）'
                : `轮询模式 · ${Math.round(POLL_DISCONNECTED_MS / 1000)} 秒`}
            </span>
            <button className="btn secondary" type="button" onClick={refresh} disabled={state.refreshing}>
              {state.refreshing ? '刷新中…' : '刷新'}
            </button>
          </div>
        </div>

        {isDesktop && (
          <>
            {route.page === 'factory' && <FactoryPage state={state} actions={factoryActions} />}
            {route.page === 'monitor' && (
              <MonitorPage
                state={state}
                onRefresh={refresh}
                onOpenEvents={() => openEvents(state.deviceId ?? undefined)}
                onAck={(event) => void dashboard.submit(event, 'ack')}
                onEvidenceRequest={(event) => void dashboard.submit(event, 'evidence')}
              />
            )}
            {route.page === 'events' && (
              <EventsPage
                state={state}
                onRefresh={refresh}
                onSelectEvent={(eventId) => dashboard.selectEvent(eventId)}
                onAck={(event) => void dashboard.submit(event, 'ack')}
                onEvidenceRequest={(event) => void dashboard.submit(event, 'evidence')}
                onReloadEvidence={(eventId) => void dashboard.loadEventContext(eventId)}
              />
            )}
            {route.page === 'analysis' && (
              <AnalysisPage state={state} onSelectEvent={(eventId) => dashboard.selectEvent(eventId)} />
            )}
          </>
        )}
      </main>

      <DesktopGuard state={state} />
      <BottomNav route={route} activeDeviceId={activeDeviceId} onNavigate={go} />
      <ToastStack toasts={state.toasts} onDismiss={(id) => dashboard.dismissToast(id)} />
    </div>
  );
}
