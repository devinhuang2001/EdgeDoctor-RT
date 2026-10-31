// 三维场景的只读探针。
//
// 存在的唯一理由是验收测试（DD-3D-02/03/04/05）需要在真实浏览器里点击某个场景实例、
// 读取相机与资源计数；否则只能靠固定坐标猜测，测不出真实交互。
// 探针只暴露读取接口，不提供任何改变业务状态的入口。

export type SceneProbe = {
  /** 模型是否已加载完成并可交互。 */
  ready: boolean;
  /** 场景实例在页面坐标系中的投影位置；不可见或不存在时返回 null。 */
  projectInstance: (name: string) => { x: number; y: number; visible: boolean } | null;
  /** 当前选中的场景实例名；没有选中或选中资产未绑定实例时返回 null。 */
  selectedInstance: () => string | null;
  /** 分区中心的投影位置。 */
  projectZone: (zone: string) => { x: number; y: number } | null;
  camera: () => { position: [number, number, number]; target: [number, number, number] };
  highlight: () => { name: string; tone: string } | null;
  rendererInfo: () => { geometries: number; textures: number; programs: number | null };
  diagnostics: () => Record<string, unknown>;
};

let current: SceneProbe | null = null;

export function publishProbe(probe: SceneProbe | null): void {
  current = probe;
  if (typeof window !== 'undefined') {
    (window as unknown as { __edgedoctorSceneProbe?: SceneProbe | null }).__edgedoctorSceneProbe = probe;
  }
}

export function readProbe(): SceneProbe | null {
  return current;
}
