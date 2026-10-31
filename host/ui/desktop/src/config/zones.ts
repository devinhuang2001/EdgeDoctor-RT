// 资产绑定与分区元数据（规格 §5.2）。构建期由 scripts/build-bindings.mjs 校验，
// 并通过 vite 别名指向同一份冻结配置，避免前后端出现两份定义。

import bindings from './asset-bindings.json';
import type { AssetBinding } from '../api/types';

export type FactoryZone = {
  zone: string;
  scene_root: string;
  display_name_zh: string;
  display_name_en: string;
  /** 分区图标取自区域语义，不表达设备状态。 */
  icon: string;
};

type BindingFile = {
  schema_version: number;
  frozen_at: string;
  zones: FactoryZone[];
  assets: AssetBinding[];
};

const file = bindings as unknown as BindingFile;

export const bindingSchemaVersion = file.schema_version;
export const bindingsFrozenAt = file.frozen_at;
export const zoneDefinitions: FactoryZone[] = file.zones;
export const frozenBindings: AssetBinding[] = file.assets;

const ZONE_ICONS: Record<string, string> = {
  A: '🏭', B: '⚙️', C: '🛢️', D: '📦', E: '🔧', F: '🖥️',
};

export const zoneMeta = (zone: string): FactoryZone => {
  const found = zoneDefinitions.find((item) => item.zone === zone);
  return found ?? { zone, scene_root: `ZONE_${zone}`, display_name_zh: `区域 ${zone}`, display_name_en: 'Unknown Zone', icon: '▫️' };
};

export const zoneIcon = (zone: string): string => ZONE_ICONS[zone] ?? '▫️';

export const bindingForAsset = (assetId: string | null | undefined): AssetBinding | null => (
  assetId ? frozenBindings.find((item) => item.asset_id === assetId) ?? null : null
);

/** 场景节点名到资产的映射；未绑定的实例点击后仍可被识别为“未接入”。 */
export const assetForSceneInstance = (instance: string, assets: { scene_instance: string }[]): number => (
  assets.findIndex((item) => item.scene_instance === instance)
);

export const detailModelFor = (binding: AssetBinding | null): string | null => (
  binding?.detail_model ? binding.detail_model : null
);
