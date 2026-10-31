// 工厂总览：六区分区列表 + 三维总览 + 选中设备状态。设备状态一律取服务端汇总与快照。

import { lazy, Suspense, useMemo } from 'react';
import type { FactoryAsset } from '../api/types';
import type { DashboardState } from '../state/store';
import type { SceneHighlight } from '../scene/FactoryScene';
import { AssetList, HealthCard, SelectedAssetCard, ZoneList } from '../components/assets';
import { ErrorNotice, Panel, PanelHead, StateView } from '../components/primitives';
import { useNow } from '../lib/useNow';

const FactoryScene = lazy(() => import('../scene/FactoryScene').then((module) => ({ default: module.FactoryScene })));

export type FactoryActions = {
  selectAsset: (assetId: string) => void;
  setZoneFilter: (zone: string | null) => void;
  openMonitor: (deviceId: string) => void;
  openEvents: (deviceId: string) => void;
  refresh: () => void;
};

export function FactoryPage({ state, actions }: {
  state: DashboardState;
  actions: FactoryActions;
}) {
  const now = useNow();
  const assets = state.summary?.assets ?? [];
  const selected = useMemo(
    () => (state.summary?.assets ?? []).find((asset) => asset.asset_id === state.selectedAssetId) ?? null,
    [state.summary, state.selectedAssetId],
  );
  const visibleAssets = state.zoneFilter ? assets.filter((asset) => asset.zone === state.zoneFilter) : assets;

  const highlight = useMemo<SceneHighlight | null>(() => {
    if (!selected) return null;
    // 与服务端 /api/v1/query/status 同源：只看该设备最近一条事件，
    // 保证场景高亮、侧栏文案与状态接口三者一致。
    const latestEvent = selected.device_id
      ? state.events.find((event) => event.device_id === selected.device_id) ?? null
      : null;
    if (latestEvent?.condition === 'ACTIVE') {
      return latestEvent.severity === 'ALARM'
        ? { tone: 'critical', label: '活动严重报警' }
        : { tone: 'warning', label: '活动预警' };
    }
    if (selected.status === 'ONLINE') return { tone: 'ok', label: '在线' };
    if (selected.status === 'STALE') return { tone: 'warning', label: '数据过期' };
    return { tone: 'unknown', label: '未接入' };
  }, [selected, state.events]);

  return (
    <div className="page-grid factory-grid">
      <div className="stack">
        <HealthCard summary={state.summary} nowMs={now} />
        <ZoneList summary={state.summary} selectedZone={state.zoneFilter} onSelect={actions.setZoneFilter} />
        <AssetList
          assets={visibleAssets}
          selectedAssetId={state.selectedAssetId}
          onSelect={actions.selectAsset}
          zoneFilter={state.zoneFilter}
        />
      </div>

      <Panel className="scene-panel">
        <PanelHead
          eyebrow="Factory Digital Twin"
          title="六区工厂总览"
          meta={selected ? `选中：${selected.display_name_zh}` : '未选择设备'}
          actions={<button className="btn ghost" type="button" onClick={actions.refresh}>刷新数据</button>}
        />
        {state.service === 'UNAVAILABLE' && state.serviceError && (
          <div style={{ marginBottom: 12 }}>
            <ErrorNotice error={state.serviceError} onRetry={actions.refresh} />
          </div>
        )}
        <Suspense fallback={<StateView tone="loading" title="正在准备三维场景" />}>
          <FactoryScene
            assets={assets}
            selectedAssetId={state.selectedAssetId}
            onSelect={actions.selectAsset}
            focusZone={state.zoneFilter}
            highlight={highlight}
            onFocusZone={actions.setZoneFilter}
          />
        </Suspense>
        {assets.length === 0 && (
          <p className="panel-note">
            资产绑定配置为空或尚未读取成功。三维模型可以浏览，但不显示任何在线设备。
          </p>
        )}
      </Panel>

      <div className="stack factory-detail-column">
        <SelectedAssetCard
          asset={selected}
          snapshot={state.snapshot}
          deviceEvents={selected?.device_id ? state.events.filter((event) => event.device_id === selected.device_id) : []}
          nowMs={now}
          thresholdMs={state.summary?.online_threshold_ms ?? 30_000}
          onOpenMonitor={actions.openMonitor}
          onOpenEvents={actions.openEvents}
        />
        <Panel>
          <PanelHead eyebrow="Scene Binding" title="绑定一览" meta={`${assets.length} 项`} />
          {assets.length === 0
            ? <StateView title="没有绑定资产">请在 asset-bindings.json 中声明资产后重新构建。</StateView>
            : (
              <table className="table">
                <thead>
                  <tr>
                    <th scope="col">场景实例</th>
                    <th scope="col">资产</th>
                    <th scope="col">设备</th>
                    <th scope="col">绑定</th>
                  </tr>
                </thead>
                <tbody>
                  {assets.map((asset: FactoryAsset) => (
                    <tr key={asset.asset_id}>
                      <td className="num">{asset.scene_instance}</td>
                      <td>{asset.display_name_zh}</td>
                      <td className="num">{asset.device_id ?? '—'}</td>
                      <td>{asset.binding_state === 'verified' ? '已验证' : asset.binding_state === 'provisional' ? '待核验' : '未接入'}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
          <p className="panel-note">
            场景实例在构建期由 scripts/build-bindings.mjs 对照工厂模型节点校验，
            节点不存在的配置会直接让构建失败，因此这里不再重复检查。
          </p>
        </Panel>
      </div>
    </div>
  );
}
