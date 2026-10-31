// 应用外壳：顶栏（服务就绪、数据源、最后刷新）、底部导航、低分辨率守卫、提示条。

import { useState } from 'react';
import type { DashboardState } from '../state/store';
import { clockText } from '../lib/format';
import { serviceView } from '../lib/status';
import { StatusDot } from './primitives';
import type { Route } from '../router';

const NAV: { page: Route['page']; label: string; icon: string }[] = [
  { page: 'factory', label: '工厂总览', icon: '🏭' },
  { page: 'monitor', label: '实时监测', icon: '📈' },
  { page: 'events', label: '事件中心', icon: '🔔' },
  { page: 'analysis', label: '信号分析', icon: '〰️' },
];

const SOURCE_LABEL: Record<DashboardState['lastSource'], string> = {
  INITIAL: '首次加载', SSE: '实时推送', POLLING: '轮询', MANUAL: '手动刷新', ACTION: '命令回执后刷新',
};

export function TopBar({ state, route, onNavigate, onSetToken }: {
  state: DashboardState;
  route: Route;
  onNavigate: (route: Route) => void;
  onSetToken: (token: string) => void;
}) {
  const [tokenDraft, setTokenDraft] = useState(state.token);
  const counts = state.summary?.counts;
  const total = state.summary?.assets.length ?? null;
  const service = serviceView(state.service);

  const stats = [
    { key: 'total', icon: '🗄️', tone: '', value: total, label: '绑定资产' },
    { key: 'online', icon: '✓', tone: 'ok', value: counts?.ONLINE ?? null, label: '在线' },
    { key: 'stale', icon: '!', tone: 'warn', value: counts?.STALE ?? null, label: '数据过期' },
    { key: 'unknown', icon: '·', tone: '', value: counts?.UNKNOWN ?? null, label: '未接入' },
  ];

  return (
    <header className="topbar">
      <a className="brand" href="#/factory" onClick={(event) => { event.preventDefault(); onNavigate({ page: 'factory' }); }}>
        <span className="brand-mark" aria-hidden="true">E</span>
        <span className="brand-text">
          <strong>EdgeDoctor-RT</strong>
          <small>Smart Factory Predictive Maintenance</small>
        </span>
      </a>
      <p className="topbar-tagline">让设备更健康 · 让生产更可靠<br />数据由服务端持久化状态驱动</p>

      <div className="stat-strip" role="group" aria-label="工厂统计（来自服务端汇总接口）">
        {stats.map((stat) => (
          <div className={`stat ${stat.tone}`} key={stat.key}>
            <span className="stat-icon" aria-hidden="true">{stat.icon}</span>
            <div>
              <div className="stat-value num">{stat.value ?? '—'}</div>
              <div className="stat-label">{stat.label}</div>
            </div>
          </div>
        ))}
      </div>

      <div className="topbar-side">
        <span className="service-chip">
          <StatusDot tone={state.service === 'READY' ? 'ok' : state.service === 'LOADING' ? 'info' : 'critical'} label={service.label} />
          <span>{service.label}</span>
          <span className="service-time">
            {state.lastRefreshMs ? `${SOURCE_LABEL[state.lastSource]} · ${clockText(state.lastRefreshMs)}` : '尚未刷新'}
          </span>
        </span>
        <label className="token-field">
          API Token（仅当前页面内存）
          <input
            type="password"
            value={tokenDraft}
            autoComplete="off"
            aria-label="API Token，仅保存在当前页面内存，不写入存储"
            placeholder="可选"
            onChange={(event) => setTokenDraft(event.target.value)}
            onBlur={() => { if (tokenDraft !== state.token) onSetToken(tokenDraft); }}
            onKeyDown={(event) => { if (event.key === 'Enter' && tokenDraft !== state.token) onSetToken(tokenDraft); }}
          />
        </label>
      </div>
      <span className="sr-only" aria-live="polite">{`当前页面：${NAV.find((item) => item.page === route.page)?.label ?? ''}`}</span>
    </header>
  );
}

export function BottomNav({ route, activeDeviceId, onNavigate }: {
  route: Route;
  activeDeviceId: string | null;
  onNavigate: (route: Route) => void;
}) {
  return (
    <nav className="bottom-nav" aria-label="主导航">
      {NAV.map((item) => (
        <button
          key={item.page}
          type="button"
          aria-current={route.page === item.page ? 'page' : undefined}
          onClick={() => onNavigate({
            page: item.page,
            deviceId: item.page === 'monitor' || item.page === 'analysis' ? activeDeviceId ?? undefined : undefined,
          })}
        >
          <span aria-hidden="true">{item.icon}</span>
          <span>{item.label}</span>
        </button>
      ))}
    </nav>
  );
}

export function DesktopGuard({ state }: { state: DashboardState }) {
  const service = serviceView(state.service);
  return (
    <div className="desktop-guard">
      <h1>请使用桌面浏览器</h1>
      <p className="muted" style={{ marginTop: 8 }}>
        电脑版控制台的最小布局宽度为 1024 px，包含六区三维总览与三栏事件视图。
        在窄屏下不会强行压缩为手机布局，以免图表与操作按钮互相遮挡。
      </p>
      <p style={{ marginTop: 12 }}>
        <StatusDot tone={state.service === 'READY' ? 'ok' : state.service === 'LOADING' ? 'info' : 'critical'} label={service.label} />
        <span style={{ marginLeft: 8 }}>{service.label}</span>
      </p>
      <p className="faint" style={{ marginTop: 6 }}>
        {state.lastRefreshMs ? `最后刷新 ${clockText(state.lastRefreshMs)}（${SOURCE_LABEL[state.lastSource]}）` : '尚未读取服务端数据'}
      </p>
    </div>
  );
}

export function ToastStack({ toasts, onDismiss }: { toasts: DashboardState['toasts']; onDismiss: (id: number) => void }) {
  return (
    <div className="toast-stack" aria-live="polite" aria-atomic="false">
      {toasts.map((toast) => (
        <div className={`toast ${toast.tone}`} key={toast.id} role="status">
          <span>{toast.text}</span>
          <button type="button" aria-label="关闭提示" onClick={() => onDismiss(toast.id)}>×</button>
        </div>
      ))}
    </div>
  );
}
