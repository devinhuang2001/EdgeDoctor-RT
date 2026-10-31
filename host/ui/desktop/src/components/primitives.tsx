// 通用展示组件。所有状态同时用文字与图标表达，颜色只是辅助（规格 §4 / §8）。

import type { ReactNode } from 'react';
import type { ApiError } from '../api/client';
import type { Tone } from '../lib/status';

const ICON: Record<Tone, string> = { ok: '✓', warning: '!', critical: '✕', unknown: '·', info: 'i' };

export function Panel({ children, className = '', id }: { children: ReactNode; className?: string; id?: string }) {
  return <section className={`panel ${className}`} id={id}>{children}</section>;
}

export function PanelHead({ eyebrow, title, meta, actions }: { eyebrow?: string; title: string; meta?: ReactNode; actions?: ReactNode }) {
  return (
    <header className="panel-head">
      <div>
        {eyebrow && <p className="eyebrow">{eyebrow}</p>}
        <h2>{title}</h2>
      </div>
      {(meta || actions) && <div className="panel-meta">{actions ?? meta}</div>}
    </header>
  );
}

export function Badge({ tone, children }: { tone: Tone; children: ReactNode }) {
  return <span className={`badge ${tone}`}>{children}</span>;
}

export function StatusDot({ tone, label }: { tone: Tone; label: string }) {
  return <span className={`dot ${tone}`} role="img" aria-label={label} />;
}

/** 状态徽章：图标 + 文字，读屏软件读到完整状态。 */
export function StatusBadge({ tone, label }: { tone: Tone; label: string }) {
  return (
    <span className={`badge ${tone}`}>
      <span aria-hidden="true">{ICON[tone]}</span>
      {label}
    </span>
  );
}

export function Metric({ label, value, unit, hint, tone, small }: {
  label: string; value: ReactNode; unit?: string; hint?: string; tone?: 'warning' | 'critical' | 'unknown'; small?: boolean;
}) {
  return (
    <div className="metric">
      <span className="metric-label">{label}</span>
      <span className={`metric-value ${tone ?? ''} ${small ? 'small' : ''}`}>
        {value}
        {unit && <span className="metric-unit">{unit}</span>}
      </span>
      {hint && <span className="metric-hint">{hint}</span>}
    </div>
  );
}

export function MetricGrid({ columns = 3, children }: { columns?: 2 | 3 | 4; children: ReactNode }) {
  return <div className={`metric-grid cols-${columns}`}>{children}</div>;
}

export function DetailRow({ term, children }: { term: string; children: ReactNode }) {
  return (
    <div className="detail-row">
      <dt>{term}</dt>
      <dd>{children}</dd>
    </div>
  );
}

export function DetailList({ children }: { children: ReactNode }) {
  return <dl className="detail-list">{children}</dl>;
}

export function Banner({ tone, title, children }: { tone: Tone; title: string; children?: ReactNode }) {
  return (
    <div className={`banner ${tone}`} role={tone === 'critical' ? 'alert' : undefined}>
      <span className="banner-icon" aria-hidden="true">{ICON[tone]}</span>
      <div>
        <strong>{title}</strong>
        {children && <span>{children}</span>}
      </div>
    </div>
  );
}

/** 空态 / 加载 / 错误三合一状态视图。 */
export function StateView({ tone = 'idle', title, children, action }: {
  tone?: 'idle' | 'loading' | 'error' | 'warning'; title: string; children?: ReactNode; action?: ReactNode;
}) {
  const className = tone === 'loading' ? 'loading' : tone;
  return (
    <div className={`state-view ${className}`} role={tone === 'error' ? 'alert' : undefined} aria-busy={tone === 'loading'}>
      {tone === 'loading'
        ? <span className="spinner" aria-hidden="true" />
        : <span className="state-icon" aria-hidden="true">{tone === 'error' ? '✕' : tone === 'warning' ? '!' : '·'}</span>}
      <strong>{title}</strong>
      {children && <p>{children}</p>}
      {action}
    </div>
  );
}

export function Notice({ tone = 'info', title, children, action }: {
  tone?: 'info' | 'warning' | 'error'; title?: string; children: ReactNode; action?: ReactNode;
}) {
  return (
    <div className={`notice ${tone}`} role={tone === 'error' ? 'alert' : 'status'}>
      <span aria-hidden="true">{ICON[tone === 'error' ? 'critical' : tone]}</span>
      <div style={{ flex: 1 }}>
        {title && <span className="notice-title">{title}&nbsp;</span>}
        {children}
      </div>
      {action}
    </div>
  );
}

export function Segmented<T extends string>({ value, options, onChange, label }: {
  value: T; options: { value: T; label: string }[]; onChange: (value: T) => void; label: string;
}) {
  return (
    <div className="seg" role="group" aria-label={label}>
      {options.map((option) => (
        <button
          key={option.value}
          type="button"
          aria-pressed={option.value === value}
          onClick={() => onChange(option.value)}
        >
          {option.label}
        </button>
      ))}
    </div>
  );
}

/** 统一的请求错误展示：把 ApiError 分类翻译成可恢复的操作。 */
export function ErrorNotice({ error, onRetry }: { error: ApiError | Error | null; onRetry?: () => void }) {
  if (!error) return null;
  const kind = (error as ApiError).kind;
  const title = kind === 'auth' ? '认证被拒绝'
    : kind === 'forbidden' ? '没有操作权限'
      : kind === 'conflict' ? '数据已过期'
        : kind === 'schema' ? '数据不可用'
          : kind === 'not-found' ? '记录不存在'
            : kind === 'rate-limited' ? '请求被限流'
              : '请求失败';
  const tone = kind === 'auth' || kind === 'forbidden' || kind === 'not-found' ? 'warning' : 'error';
  return (
    <Notice
      tone={tone}
      title={title}
      action={onRetry ? <button className="btn ghost" type="button" onClick={onRetry}>重试</button> : undefined}
    >
      {error.message}
    </Notice>
  );
}

export function Field({ term, children }: { term: string; children: ReactNode }) {
  return <div><dt>{term}</dt><dd>{children}</dd></div>;
}
