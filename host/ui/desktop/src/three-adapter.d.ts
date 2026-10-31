// three 的 addons 入口（three/addons/*）在部分版本里没有随 @types/three 提供类型声明，
// 这里补上最小声明；核心 three 仍然使用官方类型，避免场景代码退化为 any。
declare module 'three/addons/controls/OrbitControls.js' {
  import type { Camera, EventDispatcher, Vector3 } from 'three';
  export class OrbitControls extends EventDispatcher {
    constructor(object: Camera, domElement?: HTMLElement);
    enablePan: boolean;
    enableDamping: boolean;
    dampingFactor: number;
    minDistance: number;
    maxDistance: number;
    maxPolarAngle: number;
    target: Vector3;
    update(): void;
    dispose(): void;
  }
}

declare module 'three/addons/loaders/GLTFLoader.js' {
  import type { Object3D } from 'three';
  export type GLTF = { scene: Object3D; scenes: Object3D[]; animations: unknown[]; asset: unknown };
  export type ProgressEvent = { loaded: number; total: number };
  export class GLTFLoader {
    load(
      url: string,
      onLoad: (gltf: GLTF) => void,
      onProgress?: (event: ProgressEvent) => void,
      onError?: (error: unknown) => void,
    ): void;
  }
}
