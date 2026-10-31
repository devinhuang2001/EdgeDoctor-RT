/* EdgeDoctor 设备诊断台 · 前端逻辑
 * 只读消费服务端 API；写操作（确认 / 留证 / 测试通知）在配置了
 * EDGEDOCTOR_API_TOKEN 时需服务端放行，失败会优雅降级为提示。
 */

'use strict';

const DEVICE_ID = 'ed84-001';
const POLL_MS = 10000;

const COLOR = {
  accent: '#059669',
  accentInk: '#047857',
  crit: '#be123c',
};

/* ---------- DOM ---------- */
const $ = (id) => document.getElementById(id);

const el = {
  health: $('health'),
  healthDot: $('healthDot'),
  statusCard: $('status-section'),
  state: $('state'),
  stateGlyph: $('stateGlyph'),
  stateDetail: $('stateDetail'),
  advice: $('advice'),
  quality: $('quality'),
  eventCount: $('eventCount'),
  eventNote: $('eventNote'),
  events: $('events'),
  source: $('source'),
  notifyState: $('notifyState'),
  trend: $('trend'),
  trendMeta: $('trendMeta'),
  rms: $('rms'),
  rmsSpark: $('rmsSpark'),
  score: $('score'),
  scoreSpark: $('scoreSpark'),
  deviation: $('deviation'),
  deviceId: $('deviceId'),
  updated: $('updated'),
  notifyTest: $('notifyTest'),
  refresh: $('refresh'),
};

/* ---------- 工具 ---------- */
const esc = (value) => String(value ?? '')
  .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
  .replace(/"/g, '&quot;').replace(/'/g, '&#39;');

const num = (value, digits = 3) => {
  const n = Number(value);
  return Number.isFinite(n) ? n.toFixed(digits) : '-';
};

const fmt = (n) => (Number.isFinite(Number(n)) ? Number(n) : null);

/* 自适应小数位：小量级（振动 g）需要更多位才有意义 */
const axisNum = (v) => {
  const a = Math.abs(v);
  if (a === 0) return '0';
  if (a < 0.001) return v.toExponential(1);
  if (a < 1) return v.toFixed(4);
  return v.toFixed(2);
};

const DOT_STATE = ['is-unknown', 'is-ok', 'is-warn', 'is-crit'];
function setDot(dot, state, live) {
  dot.classList.remove(...DOT_STATE);
  if (DOT_STATE.includes(state)) dot.classList.add(state);
  dot.classList.toggle('is-live', Boolean(live));
}

function ageText(ms) {
  const value = fmt(ms);
  if (value === null) return null;
  const seconds = Math.max(0, Math.round(value / 1000));
  if (seconds < 5) return '刚刚';
  if (seconds < 60) return `${seconds} 秒前`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes} 分钟前`;
  return `${Math.floor(minutes / 60)} 小时前`;
}

/* 数值变化时短暂高亮，提示"数据已刷新"（首次渲染不闪） */
let firstPaint = true;
function setValue(node, text) {
  if (node.textContent === text) return;
  node.textContent = text;
  if (firstPaint || !node.classList.contains('num')) return;
  node.classList.remove('is-flash');
  void node.offsetWidth;
  node.classList.add('is-flash');
}

/* Catmull-Rom 近似的平滑曲线（参考图表库的 monotone 观感） */
function smoothPath(points) {
  if (points.length < 3) {
    return points.map((p, i) => `${i ? 'L' : 'M'} ${p[0].toFixed(1)} ${p[1].toFixed(1)}`).join(' ');
  }
  let d = `M ${points[0][0].toFixed(1)} ${points[0][1].toFixed(1)}`;
  for (let i = 0; i < points.length - 1; i++) {
    const p0 = points[i - 1] || points[i];
    const p1 = points[i];
    const p2 = points[i + 1];
    const p3 = points[i + 2] || p2;
    const t = 0.18; // 低张力，抑制离群尖峰的过冲
    const c1x = p1[0] + (p2[0] - p0[0]) * t;
    const c1y = p1[1] + (p2[1] - p0[1]) * t;
    const c2x = p2[0] - (p3[0] - p1[0]) * t;
    const c2y = p2[1] - (p3[1] - p1[1]) * t;
    d += ` C ${c1x.toFixed(1)} ${c1y.toFixed(1)}, ${c2x.toFixed(1)} ${c2y.toFixed(1)}, ` +
         `${p2[0].toFixed(1)} ${p2[1].toFixed(1)}`;
  }
  return d;
}

/* ---------- 状态卡 ---------- */
const DIAGNOSIS_LABEL = {
  NO_BASELINE: '尚未建立基线',
  BASELINE_MISMATCH: '基线不匹配',
  WITHIN_BASELINE: '基线范围内',
  UNKNOWN_ANOMALY: '未知异常',
  KNOWN_CLASS: '检出已知故障类',
  UNCERTAIN: '诊断不确定',
  UNAVAILABLE: '诊断不可用',
  UNKNOWN: '诊断未知',
};

function eventTitle(event) {
  const diagnosis = (event.diagnosis || 'UNKNOWN').toUpperCase();
  if (DIAGNOSIS_LABEL[diagnosis]) return DIAGNOSIS_LABEL[diagnosis];
  return event.condition === 'ACTIVE' ? '振动偏离基线' : '设备事件';
}

function severityTag(event) {
  if (event.severity === 'ALARM') return '<span class="state-tag is-crit">严重报警</span>';
  return '<span class="state-tag is-warn">预警</span>';
}

function conditionText(event) {
  if (event.condition !== 'ACTIVE') return '已恢复';
  return event.ack === 'ACKNOWLEDGED' ? '已确认' : '待处理';
}

function conditionClass(event) {
  if (event.condition !== 'ACTIVE') return 'is-muted';
  return event.ack === 'ACKNOWLEDGED' ? 'is-accent' : 'is-warn';
}

function severityClass(event) {
  if (event.condition !== 'ACTIVE') return 'is-ok';
  return event.severity === 'ALARM' ? 'is-alarm' : 'is-warn';
}

function setQuality(label, kind) {
  el.quality.textContent = label;
  el.quality.classList.remove('is-ok', 'is-warn', 'is-crit');
  if (kind) el.quality.classList.add(kind);
}

function setUpdated() {
  el.updated.textContent = '更新于 ' + new Date().toLocaleTimeString('zh-CN', { hour12: false });
}

function bannerStatus(q) {
  if (q.status === 'NO_DATA') {
    // 无事件但有遥测：设备在报数，只是尚未形成事件
    if (q.telemetry) {
      return {
        text: '运行中', dot: 'is-ok', live: true, glyph: '✓', cls: 'is-ok',
        advice: '持续接收遥测，未触发告警',
      };
    }
    return { text: '等待设备数据', dot: 'is-unknown', live: false, glyph: '-', cls: '' };
  }
  if (q.status === 'WARNING') return { text: '振动偏离基线', dot: 'is-warn', live: true, glyph: '!', cls: 'is-warn' };
  if (q.status === 'ALARM') return { text: '严重报警', dot: 'is-crit', live: true, glyph: '!', cls: 'is-crit' };
  if (q.status === 'RECOVERED') return { text: '已恢复', dot: 'is-ok', live: true, glyph: '✓', cls: 'is-ok' };
  return { text: '状态未知', dot: 'is-unknown', live: false, glyph: '-', cls: '' };
}

async function loadSnapshot() {
  const response = await fetch(`/api/v1/query/status?device_id=${encodeURIComponent(DEVICE_ID)}`);
  if (!response.ok) return null;
  return response.json();
}

function applySnapshot(q) {
  if (!q) return;
  const view = bannerStatus(q);

  el.state.textContent = view.text;
  el.stateGlyph.textContent = view.glyph;
  el.statusCard.classList.remove('is-ok', 'is-warn', 'is-crit');
  if (view.cls) el.statusCard.classList.add(view.cls);

  el.deviceId.textContent = q.device_id || DEVICE_ID;
  el.source.textContent = (q.event && q.event.source) || (q.telemetry ? 'LIVE_IMU' : '-');

  const age = q.snapshot_age_ms != null ? ageText(q.snapshot_age_ms) : null;
  const src = (q.event && q.event.source) || (q.telemetry ? 'LIVE_IMU' : null);
  el.stateDetail.textContent = age && src
    ? `来源 ${src}，最近快照 ${age}`
    : (src ? `来源 ${src}` : '服务端会在收到板端快照后更新');

  // 派生状态下的建议以本地推导为准，避免与服务端 NO_DATA 文案矛盾
  const advice = view.advice || q.advice || '';
  el.advice.textContent = advice;
  el.advice.hidden = !advice;

  // 信号质量
  const t = q.telemetry;
  const e = q.event;
  if (t && t.alarm) setQuality('需关注', 'is-crit');
  else if (e && (e.condition === 'ACTIVE' || e.quality === 'VALID')) setQuality('有效', 'is-ok');
  else if (e) setQuality('等待有效信号', 'is-warn');
  else if (t) setQuality('有效', 'is-ok');
  else setQuality('等待数据', null);

  // 指标
  if (t) {
    setValue(el.rms, num(t.rms_vector_g, 4));
    setValue(el.score, num(t.anomaly_score, 1));
    el.deviation.textContent = t.alarm ? '超过阈值' : '在基线内';
  }
}

/* ---------- 事件 ---------- */
async function loadEvents() {
  const data = await fetch('/api/v1/events?limit=10').then((r) => r.json());
  const list = Array.isArray(data) ? data : [];

  const active = list.filter((x) => x.condition === 'ACTIVE');
  setValue(el.eventCount, String(active.length));
  el.eventNote.textContent = `${list.length} 条记录`;

  if (list.length === 0) {
    el.events.innerHTML =
      '<div class="empty-state"><strong>暂无事件</strong>' +
      '<span>设备上报偏离基线的快照后，会在此形成告警记录。</span></div>';
    return;
  }

  el.events.innerHTML = list.map((event) => {
    const isActive = event.condition === 'ACTIVE';
    const unacked = isActive && event.ack !== 'ACKNOWLEDGED';
    const peak = fmt(event.peak_deviation);
    const rev = fmt(event.revision);

    let actions = `<span class="state-tag ${conditionClass(event)}">${conditionText(event)}</span>`;
    if (unacked) {
      actions +=
        '<button type="button" class="btn is-small is-ghost" data-action="ack" data-event="' +
        esc(event.event_id) + '" data-revision="' + (rev == null ? '' : rev) + '">确认</button>' +
        '<button type="button" class="btn is-small is-ghost" data-action="evidence" data-event="' +
        esc(event.event_id) + '">留证</button>';
    }

    return (
      '<div class="event ' + severityClass(event) + '">' +
        '<div class="event-head">' +
          '<div class="event-title">' +
            '<span class="name">' + esc(eventTitle(event)) + '</span>' +
            severityTag(event) +
          '</div>' +
          '<div class="event-actions">' + actions + '</div>' +
        '</div>' +
        '<div class="event-meta">' +
          '<code>rev ' + (rev == null ? '-' : rev) + '</code>' +
          (peak != null ? '<code>峰值偏离 ' + num(event.peak_deviation, 3) + '</code>' : '') +
          '<span>' + esc(event.source || 'LIVE_IMU') + '</span>' +
          '<span class="id" title="' + esc(event.event_id) + '">' + esc(event.event_id) + '</span>' +
        '</div>' +
      '</div>'
    );
  }).join('');
}

/* ---------- 趋势 ---------- */
const TREND_MAX = 200;

/* KPI 迷你趋势图：无坐标轴，只表达形状 */
function sparkline(values, gid) {
  const vals = values.filter((v) => v !== null);
  if (vals.length < 2) return '';

  const min = Math.min(...vals);
  const max = Math.max(...vals);
  const span = (max - min) || Math.abs(max) || 1;
  const W = 100;
  const H = 34;
  const pad = 3;

  const points = vals.map((v, i) => [
    (i / (vals.length - 1)) * W,
    pad + (1 - (v - min) / span) * (H - pad * 2),
  ]);

  const line = smoothPath(points);
  const area = `${line} L ${W} ${H} L 0 ${H} Z`;

  return (
    `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" aria-hidden="true">` +
      `<defs><linearGradient id="${gid}" x1="0" y1="0" x2="0" y2="1">` +
        `<stop offset="0%" stop-color="${COLOR.accent}" stop-opacity="0.26"/>` +
        `<stop offset="100%" stop-color="${COLOR.accent}" stop-opacity="0"/>` +
      '</linearGradient></defs>' +
      `<path d="${area}" fill="url(#${gid})"/>` +
      `<path d="${line}" fill="none" stroke="${COLOR.accent}" stroke-width="1.5" ` +
        'vector-effect="non-scaling-stroke" stroke-linecap="round" stroke-linejoin="round"/>' +
    '</svg>'
  );
}

function renderSparks(samples) {
  el.rmsSpark.innerHTML = sparkline(samples.map((s) => fmt(s.rms_vector_g)), 'sp-rms');
  el.scoreSpark.innerHTML = sparkline(samples.map((s) => fmt(s.anomaly_score)), 'sp-score');
}

function renderTrendChart(samples) {
  const values = samples.map((s) => fmt(s.rms_vector_g));
  const nums = values.filter((v) => v !== null);
  if (nums.length === 0) return false;

  let min = Math.min(...nums);
  let max = Math.max(...nums);
  if (max - min < 1e-9) {
    const center = max || 0.001;
    min = center - Math.abs(center) * 0.1;
    max = center + Math.abs(center) * 0.1;
  }
  const pad = (max - min) * 0.14;
  max += pad;
  min = Math.max(0, min - pad); // RMS 是幅值，纵轴不出现负值

  const W = 1000;
  const H = 260;
  const L = 64;
  const R = 18;
  const T = 22;
  const B = 26;

  const px = (i) => L + (i / Math.max(1, values.length - 1)) * (W - L - R);
  const py = (v) => T + (1 - (v - min) / (max - min)) * (H - T - B);

  // 水平网格线 + 刻度（顶部刻度带单位，建立量纲）
  const levels = [max, (max + min) / 2, min];
  let grid = '';
  let labels = '';
  levels.forEach((level, index) => {
    const y = py(level);
    grid += `<line class="grid-line" x1="${L}" y1="${y.toFixed(1)}" x2="${W - R}" y2="${y.toFixed(1)}"/>`;
    labels += `<text x="${L - 10}" y="${(y + 4).toFixed(1)}" text-anchor="end">` +
              axisNum(level) + (index === 0 ? ' g' : '') + '</text>';
  });

  // 折线（平滑）+ 渐变面积
  const points = [];
  values.forEach((v, i) => { if (v !== null) points.push([px(i), py(v)]); });
  if (points.length === 0) return false;
  const line = smoothPath(points);
  const area = `${line} L ${points[points.length - 1][0].toFixed(1)} ${(H - B).toFixed(1)} ` +
               `L ${points[0][0].toFixed(1)} ${(H - B).toFixed(1)} Z`;

  // 告警采样点
  let alarms = '';
  values.forEach((v, i) => {
    if (v !== null && samples[i] && samples[i].alarm) {
      alarms += `<circle cx="${px(i).toFixed(1)}" cy="${py(v).toFixed(1)}" r="3.2" fill="${COLOR.crit}"/>`;
    }
  });

  // 最新点标注
  let lastIndex = -1;
  values.forEach((v, i) => { if (v !== null) lastIndex = i; });
  const lastX = px(lastIndex);
  const lastY = py(values[lastIndex]);
  const tagAnchor = lastX > W - 150 ? 'end' : 'start';
  const tagX = lastX + (tagAnchor === 'end' ? -9 : 9);
  const marker =
    `<circle cx="${lastX.toFixed(1)}" cy="${lastY.toFixed(1)}" r="4.5" fill="#fff" ` +
      `stroke="${COLOR.accentInk}" stroke-width="2.5"/>` +
    `<text class="value-tag" x="${tagX.toFixed(1)}" y="${(lastY - 10).toFixed(1)}" ` +
      `text-anchor="${tagAnchor}">${axisNum(values[lastIndex])}</text>`;

  el.trend.innerHTML =
    `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="RMS 振动趋势图">` +
      `<defs><linearGradient id="grad-rms" x1="0" y1="0" x2="0" y2="1">` +
        `<stop offset="0%" stop-color="${COLOR.accent}" stop-opacity="0.22"/>` +
        `<stop offset="100%" stop-color="${COLOR.accent}" stop-opacity="0"/>` +
      '</linearGradient></defs>' +
      grid +
      labels +
      `<path d="${area}" fill="url(#grad-rms)"/>` +
      `<path d="${line}" fill="none" stroke="${COLOR.accent}" stroke-width="2" ` +
        'vector-effect="non-scaling-stroke" stroke-linejoin="round" stroke-linecap="round"/>' +
      alarms +
      marker +
    '</svg>' +
    '<div class="trend-legend">' +
      '<span><i class="line-ok"></i>RMS 振动</span>' +
      '<span><i class="dot-crit"></i>告警采样</span>' +
    '</div>';

  // 卡片副标题：采样数 + 跨度
  const firstMono = fmt(samples[0] && samples[0].mono_ms);
  const lastMono = fmt(samples[lastIndex] && samples[lastIndex].mono_ms);
  let meta = `${values.length} 个采样点`;
  if (firstMono !== null && lastMono !== null && lastMono > firstMono) {
    const span = (lastMono - firstMono) / 1000;
    if (span < 3600) meta += ` · 跨度 ${span < 60 ? span.toFixed(1) + ' 秒' : (span / 60).toFixed(1) + ' 分钟'}`;
  }
  el.trendMeta.textContent = meta;

  return true;
}

async function loadTrend() {
  const data = await fetch(
    `/api/v1/trends?device_id=${encodeURIComponent(DEVICE_ID)}&limit=${TREND_MAX}`
  ).then((r) => r.json());
  const samples = Array.isArray(data) ? data.filter((s) => s && s.rms_vector_g != null) : [];

  if (samples.length === 0) {
    el.trend.innerHTML =
      '<div class="empty-state"><strong>暂无遥测</strong>' +
      '<span>开启 edgeimu 采样，或运行 edgeai 回放后，RMS 数据会显示在这里。</span></div>';
    el.trendMeta.textContent = '遥测';
    el.rmsSpark.innerHTML = '';
    el.scoreSpark.innerHTML = '';
    return;
  }
  renderTrendChart(samples);
  renderSparks(samples);
}

/* ---------- 服务健康 / 通知 ---------- */
async function loadService() {
  try {
    const response = await fetch('/health/ready');
    const ok = response.ok;
    el.health.textContent = ok ? 'READY' : 'OFFLINE';
    setDot(el.healthDot, ok ? 'is-ok' : 'is-crit', ok);
  } catch (_) {
    el.health.textContent = 'SERVICE ERROR';
    setDot(el.healthDot, 'is-crit', false);
  }
}

async function loadNotify() {
  try {
    const response = await fetch('/api/v1/notifications/status');
    if (response.status === 401) { el.notifyState.textContent = '需认证'; return; }
    if (!response.ok) { el.notifyState.textContent = '-'; return; }
    const info = await response.json();
    el.notifyState.textContent = info.configured ? '已配置' : '未配置';
  } catch (_) {
    el.notifyState.textContent = '不可用';
  }
}

/* ---------- 刷新主流程 ---------- */
let busy = false;

async function load() {
  if (busy) return;
  busy = true;
  setUpdated();
  try {
    await Promise.allSettled([loadService(), loadNotify(), loadEvents(), loadTrend()]);
    await loadSnapshot().then(applySnapshot).catch(() => null);
  } catch (_) {
    el.stateDetail.textContent = '无法读取服务端数据';
  } finally {
    firstPaint = false;
    busy = false;
  }
}

/* ---------- 写操作（带优雅降级） ---------- */
async function postAction(button, url, options) {
  button.disabled = true;
  const original = button.textContent;
  button.textContent = '请求中…';
  try {
    const response = await fetch(url, options || { method: 'POST' });
    if (response.status === 401) { button.textContent = '需认证'; return; }
    if (!response.ok) {
      const body = await response.json().catch(() => null);
      const detail = body && body.detail;
      button.textContent = detail && typeof detail === 'string' ? detail : '失败';
      return;
    }
    button.textContent = '已请求';
  } catch (_) {
    button.textContent = '服务异常';
  }
  setTimeout(() => { button.textContent = original; button.disabled = false; }, 3000);
}

/* ---------- 事件绑定 ---------- */
el.refresh.addEventListener('click', load);

el.notifyTest.addEventListener('click', () => {
  postAction(el.notifyTest, '/api/v1/notifications/test').then(() => {
    if (el.notifyTest.textContent === '已请求') {
      el.notifyTest.textContent = '已发送';
      el.notifyTest.disabled = true;
      setTimeout(() => { el.notifyTest.textContent = '测试通知'; el.notifyTest.disabled = false; }, 3000);
    }
  });
});

el.events.addEventListener('click', (event) => {
  const button = event.target.closest('button[data-action]');
  if (!button) return;
  const eventId = button.dataset.event;

  if (button.dataset.action === 'ack') {
    postAction(button, `/api/v1/events/${encodeURIComponent(eventId)}/ack`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ actor: 'dashboard', expected_revision: Number(button.dataset.revision) }),
    });
    return;
  }
  postAction(button, `/api/v1/events/${encodeURIComponent(eventId)}/evidence-request`, { method: 'POST' });
});

/* ---------- 导航高亮 ---------- */
document.querySelectorAll('.rail-nav a').forEach((link) => {
  link.addEventListener('click', () => {
    document.querySelectorAll('.rail-nav a').forEach((l) => l.classList.remove('is-active'));
    link.classList.add('is-active');
  });
});

/* ---------- SSE：审计事件触发即时刷新 ---------- */
if (window.EventSource) {
  let reloadTimer = null;
  const stream = new EventSource('/api/v1/stream');
  stream.onmessage = () => {
    clearTimeout(reloadTimer);
    reloadTimer = setTimeout(load, 250);
  };
  stream.onerror = () => stream.close();
}

/* ---------- 启动 ---------- */
el.events.innerHTML = '<div class="loading-line">正在加载事件…</div>';
el.trend.innerHTML = '<div class="loading-line">正在加载遥测…</div>';

load();
setInterval(load, POLL_MS);
