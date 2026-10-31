import { describe, expect, it } from 'vitest';
import { assetView, bindingView, commandView, eventView, freshnessView, serviceView, snapshotView } from './status';

describe('状态文案', () => {
  it('NO_DATA 不给出健康结论', () => {
    const view = snapshotView('NO_DATA');
    expect(view.label).toBe('等待设备数据');
    expect(view.tone).toBe('unknown');
    expect(`${view.label}${view.hint}`).not.toContain('正常');
  });

  it('尚未取到快照时也是未知，不得显示为在线', () => {
    expect(snapshotView(null).tone).toBe('unknown');
    expect(snapshotView(undefined).label).toBe('状态未知');
  });

  it('恢复与确认是两件事', () => {
    expect(snapshotView('RECOVERED').label).toBe('已恢复');
    expect(eventView({ condition: 'RECOVERED', ack: 'UNACKNOWLEDGED' }).ackLabel).toBe('待确认');
    expect(eventView({ condition: 'RECOVERED', ack: 'ACKNOWLEDGED' }).ackLabel).toBe('已确认');
  });

  it('资产状态与未接入语义', () => {
    expect(assetView('ONLINE').label).toBe('在线');
    expect(assetView('STALE').tone).toBe('warning');
    expect(assetView('UNKNOWN').label).toBe('未接入');
  });

  it('绑定状态区分待核验与未接入', () => {
    expect(bindingView('provisional').label).toBe('待核验绑定');
    expect(bindingView('provisional').tone).toBe('warning');
    expect(bindingView('unbound').label).toBe('未接入设备');
  });

  it('命令回执只在 APPLIED 时算已应用', () => {
    expect(commandView('PENDING').label).toBe('等待设备回执');
    expect(commandView('APPLIED').tone).toBe('ok');
    expect(commandView('REJECTED').tone).toBe('critical');
    expect(commandView('EXPIRED').tone).toBe('warning');
  });

  it('服务不可用不表示数据为空', () => {
    expect(serviceView('UNAVAILABLE').hint).toContain('保留');
  });

  it('新鲜度按服务端阈值判定', () => {
    const now = 1_700_000_000_000;
    expect(freshnessView(now - 29_000, now, 30_000).label).toBe('数据新鲜');
    expect(freshnessView(now - 31_000, now, 30_000).label).toBe('数据过期');
    expect(freshnessView(null, now, 30_000).label).toBe('暂无遥测');
  });
});
