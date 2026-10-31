// 三维资源的加载与释放工具。
// 释放必须覆盖几何、材质、材质上的贴图以及 WebGL 上下文，
// 否则连续进出场景会累积 GPU 资源（验收 DD-3D-05 的硬门禁）。

import * as THREE from 'three';

export type SceneDiagnostics = {
  contextsCreated: number;
  contextsDisposed: number;
  geometriesDisposed: number;
  materialsDisposed: number;
  texturesDisposed: number;
  scenesMounted: number;
  scenesUnmounted: number;
  failures: string[];
};

export const sceneDiagnostics: SceneDiagnostics = {
  contextsCreated: 0,
  contextsDisposed: 0,
  geometriesDisposed: 0,
  materialsDisposed: 0,
  texturesDisposed: 0,
  scenesMounted: 0,
  scenesUnmounted: 0,
  failures: [],
};

/** 把计数挂到 window，便于浏览器验收脚本读取真实释放次数。 */
function publish(): void {
  if (typeof window === 'undefined') return;
  (window as unknown as { __edgedoctorScene?: SceneDiagnostics }).__edgedoctorScene = { ...sceneDiagnostics, failures: [...sceneDiagnostics.failures] };
}
publish();

/** 挂载一次场景 = 创建一个 WebGL 上下文，两者必须成对计数才能发现上下文泄漏。 */
export function noteSceneMounted(): void {
  sceneDiagnostics.scenesMounted += 1;
  sceneDiagnostics.contextsCreated += 1;
  publish();
}
export function noteSceneUnmounted(): void { sceneDiagnostics.scenesUnmounted += 1; publish(); }
export function noteSceneFailure(message: string): void {
  sceneDiagnostics.failures.push(message);
  publish();
}

const TEXTURE_KEYS = [
  'map', 'normalMap', 'bumpMap', 'roughnessMap', 'metalnessMap', 'emissiveMap',
  'alphaMap', 'aoMap', 'displacementMap', 'lightMap', 'specularMap', 'envMap',
] as const;

function disposeMaterial(material: unknown): void {
  if (!material || typeof material !== 'object') return;
  const record = material as Record<string, unknown>;
  for (const key of TEXTURE_KEYS) {
    const texture = record[key];
    if (texture && typeof (texture as { dispose?: unknown }).dispose === 'function') {
      (texture as { dispose: () => void }).dispose();
      sceneDiagnostics.texturesDisposed += 1;
    }
  }
  if (typeof (material as { dispose?: unknown }).dispose === 'function') {
    (material as { dispose: () => void }).dispose();
    sceneDiagnostics.materialsDisposed += 1;
  }
}

/** 递归释放场景图上的几何、材质与贴图。 */
export function disposeObjectTree(root: unknown): void {
  if (!root || typeof root !== 'object') return;
  const node = root as { traverse?: (callback: (child: unknown) => void) => void };
  node.traverse?.((child: unknown) => {
    const mesh = child as { geometry?: { dispose?: () => void }; material?: unknown };
    if (mesh.geometry && typeof mesh.geometry.dispose === 'function') {
      mesh.geometry.dispose();
      sceneDiagnostics.geometriesDisposed += 1;
    }
    const material = mesh.material;
    if (Array.isArray(material)) material.forEach(disposeMaterial);
    else if (material) disposeMaterial(material);
  });
  publish();
}

/** 释放 renderer：先主动丢弃上下文，避免浏览器上下文数量上限被占满。 */
export function disposeRenderer(renderer: unknown): void {
  const target = renderer as { forceContextLoss?: () => void; dispose?: () => void; renderLists?: { dispose?: () => void } } | null;
  if (!target) return;
  try { target.renderLists?.dispose?.(); } catch { /* 某些版本没有该方法 */ }
  try { target.forceContextLoss?.(); } catch { /* 忽略上下文丢失异常 */ }
  try { target.dispose?.(); } catch { /* 忽略重复释放 */ }
  sceneDiagnostics.contextsDisposed += 1;
  publish();
}

/** 从任意子节点向上查找具有语义名称的根节点。 */
export function findSemanticRoot(object: unknown, isRoot: (name: string) => boolean): string | null {
  let current = object as { name?: string; parent?: unknown } | null;
  while (current) {
    const name = typeof current.name === 'string' ? current.name.split('/')[0] : '';
    if (name && isRoot(name)) return name;
    current = (current.parent ?? null) as { name?: string; parent?: unknown } | null;
  }
  return null;
}

export type CameraFrame = {
  /** 相机应当移动到的位置。 */
  position: THREE.Vector3;
  /** 相机应当朝向的目标点。 */
  target: THREE.Vector3;
};

/**
 * 计算“取景”而不是“转向”所需的相机位姿：
 * 保持当前视线方向，按包围球半径与视场角求出恰好容纳目标的距离。
 */
export function frameObject(
  object: unknown,
  camera: { fov: number; position: { clone: () => THREE.Vector3 } },
  offsetFactor = 1.9,
): CameraFrame | null {
  if (!object) return null;
  const box = new THREE.Box3().setFromObject(object as THREE.Object3D);
  if (box.isEmpty()) return null;
  const sphere = box.getBoundingSphere(new THREE.Sphere());
  const radius = Math.max(sphere.radius, 0.5);
  const fov = (camera.fov * Math.PI) / 180;
  const distance = (radius / Math.sin(fov / 2)) * offsetFactor;
  const direction = camera.position.clone().sub(sphere.center);
  if (direction.lengthSq() < 1e-8) direction.set(1, 1, 1);
  direction.normalize();
  return {
    position: sphere.center.clone().add(direction.multiplyScalar(distance)),
    target: sphere.center.clone(),
  };
}
