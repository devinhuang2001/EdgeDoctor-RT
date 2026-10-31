// 六区工厂数字孪生。规格 §6：
//   - 首屏资源加载完成后才允许操作，加载失败降级为静态说明且不影响数据侧栏；
//   - 受限 OrbitControls：旋转、缩放、复位、分区聚焦，禁止自由平移；
//   - 点击射线命中后向上查找绑定实例；未绑定实例也明确标识；
//   - 活动报警只高亮绑定设备或 sensor_node，不给故障部件定位结论；
//   - 页面不可见时暂停渲染循环，卸载时释放 renderer、几何、材质与监听。

import { useEffect, useMemo, useRef, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import type { FactoryAsset } from '../api/types';
import { zoneMeta } from '../config/zones';
import {
  disposeObjectTree, disposeRenderer, findSemanticRoot, frameObject,
  noteSceneFailure, noteSceneMounted, noteSceneUnmounted, sceneDiagnostics,
} from './modelUtils';
import { publishProbe } from './probe';
import type { SceneProbe } from './probe';

const MODEL_URL = `${import.meta.env.BASE_URL}models/factory-web.glb`;
const HOME_POSITION = new THREE.Vector3(18, 18, 24);

export type SceneHighlight = { tone: 'ok' | 'warning' | 'critical' | 'unknown'; label: string };

type Props = {
  assets: FactoryAsset[];
  selectedAssetId: string | null;
  onSelect: (assetId: string) => void;
  focusZone: string | null;
  highlight: SceneHighlight | null;
  /** 分区变更通知（null 表示清除分区聚焦），便于列表联动。 */
  onFocusZone?: (zone: string | null) => void;
};

export function FactoryScene({ assets, selectedAssetId, onSelect, focusZone, highlight, onFocusZone }: Props) {
  const hostRef = useRef<HTMLDivElement>(null);
  const [progress, setProgress] = useState(0);
  const [ready, setReady] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const sceneRef = useRef<THREE.Scene | null>(null);
  const modelRef = useRef<THREE.Object3D | null>(null);
  const rendererRef = useRef<THREE.WebGLRenderer | null>(null);
  const controlsRef = useRef<InstanceType<typeof OrbitControls> | null>(null);
  const cameraRef = useRef<THREE.PerspectiveCamera | null>(null);
  const highlightRef = useRef<THREE.Box3Helper | null>(null);
  const assetsRef = useRef(assets);
  const onSelectRef = useRef(onSelect);
  const onFocusZoneRef = useRef(onFocusZone);
  const selectedAssetIdRef = useRef(selectedAssetId);
  const flightRef = useRef<{ position: THREE.Vector3; target: THREE.Vector3 } | null>(null);
  // 最新值引用统一在 effect 中更新；渲染期间不写 ref。
  useEffect(() => {
    assetsRef.current = assets;
    onSelectRef.current = onSelect;
    onFocusZoneRef.current = onFocusZone;
    selectedAssetIdRef.current = selectedAssetId;
  }, [assets, onSelect, onFocusZone, selectedAssetId]);

  // ---------- 场景生命周期 ----------
  // 依赖数组为空：场景只挂载一次。资产列表每次刷新都会换新数组，
  // 若把它放进依赖会导致每 10 秒重建 WebGL 上下文与模型下载。
  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;
    let disposed = false;
    let animationId = 0;
    let loadedModel: THREE.Object3D | null = null;

    const scene = new THREE.Scene();
    scene.background = new THREE.Color('#eaf1f9');
    sceneRef.current = scene;

    const camera = new THREE.PerspectiveCamera(38, 1, 0.1, 2000);
    camera.position.copy(HOME_POSITION);
    cameraRef.current = camera;

    const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false, powerPreference: 'high-performance' });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    rendererRef.current = renderer;
    host.replaceChildren(renderer.domElement);
    renderer.domElement.tabIndex = 0;
    renderer.domElement.setAttribute('role', 'application');
    renderer.domElement.setAttribute(
      'aria-label',
      '六区工厂三维场景。方向键旋转视角，加减号缩放，Home 复位，回车聚焦选中设备所在分区。',
    );
    noteSceneMounted();

    const controls = new OrbitControls(camera, renderer.domElement);
    controlsRef.current = controls;
    controls.enablePan = false;
    controls.enableDamping = true;
    controls.dampingFactor = 0.08;
    controls.minDistance = 8;
    controls.maxDistance = 60;
    controls.maxPolarAngle = Math.PI * 0.48;
    controls.target.set(0, 0, 0);

    scene.add(new THREE.HemisphereLight(0xffffff, 0x8ca4b7, 2.1));
    const key = new THREE.DirectionalLight(0xffffff, 2.4);
    key.position.set(12, 18, 8);
    scene.add(key);

    const resize = () => {
      const { width, height } = host.getBoundingClientRect();
      if (!width || !height) return;
      camera.aspect = width / height;
      camera.updateProjectionMatrix();
      renderer.setSize(width, height, false);
    };
    resize();
    const observer = new ResizeObserver(resize);
    observer.observe(host);

    const raycaster = new THREE.Raycaster();
    const pointer = new THREE.Vector2();
    let pointerStart = { x: 0, y: 0 };
    const onPointerDown = (event: PointerEvent) => { pointerStart = { x: event.clientX, y: event.clientY }; };
    const onPointerUp = (event: PointerEvent) => {
      // 拖动旋转不应触发选择。
      if (Math.abs(event.clientX - pointerStart.x) > 4 || Math.abs(event.clientY - pointerStart.y) > 4) return;
      const rect = renderer.domElement.getBoundingClientRect();
      pointer.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
      pointer.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;
      raycaster.setFromCamera(pointer, camera);
      const hit = raycaster.intersectObjects(scene.children, true)[0];
      // 点击空白区域保持上次选择（行为对所有目标一致，不乱跳）。
      if (!hit) return;
      const selectable = new Set(assetsRef.current.map((asset) => asset.scene_instance));
      const instance = findSemanticRoot(hit.object, (name) => selectable.has(name));
      if (!instance) return;
      const asset = assetsRef.current.find((item) => item.scene_instance === instance);
      if (!asset) return;
      onSelectRef.current(asset.asset_id);
      onFocusZoneRef.current?.(asset.zone);
    };
    renderer.domElement.addEventListener('pointerdown', onPointerDown);
    renderer.domElement.addEventListener('pointerup', onPointerUp);

    const resetView = () => {
      flightRef.current = null;
      camera.position.copy(HOME_POSITION);
      controls.target.set(0, 0, 0);
      controls.update();
    };
    const rotateBy = (azimuth: number, polar: number) => {
      const offset = camera.position.clone().sub(controls.target);
      const spherical = new THREE.Spherical().setFromVector3(offset);
      spherical.theta += azimuth;
      spherical.phi = THREE.MathUtils.clamp(spherical.phi + polar, 0.15, Math.PI * 0.48);
      camera.position.copy(controls.target).add(new THREE.Vector3().setFromSpherical(spherical));
      controls.update();
    };
    const zoomBy = (factor: number) => {
      const offset = camera.position.clone().sub(controls.target);
      const distance = THREE.MathUtils.clamp(offset.length() * factor, controls.minDistance, controls.maxDistance);
      camera.position.copy(controls.target).add(offset.setLength(distance));
      controls.update();
    };
    const onKeyDown = (event: KeyboardEvent) => {
      switch (event.key) {
        case 'Home': resetView(); break;
        case 'ArrowLeft': rotateBy(0.15, 0); break;
        case 'ArrowRight': rotateBy(-0.15, 0); break;
        case 'ArrowUp': rotateBy(0, -0.12); break;
        case 'ArrowDown': rotateBy(0, 0.12); break;
        case '+': case '=': zoomBy(0.86); break;
        case '-': case '_': zoomBy(1.16); break;
        case 'Enter': {
          const zone = assetsRef.current.find((asset) => asset.asset_id === selectedAssetIdRef.current)?.zone;
          if (!zone || !loadedModel) return;
          const frame = frameObject(loadedModel.getObjectByName(`ZONE_${zone}`), camera);
          if (frame) flightRef.current = frame;
          break;
        }
        default: return;
      }
      event.preventDefault();
    };
    renderer.domElement.addEventListener('keydown', onKeyDown);

    const loader = new GLTFLoader();
    loader.load(MODEL_URL, (gltf: { scene: THREE.Object3D }) => {
      if (disposed) {
        disposeObjectTree(gltf.scene);
        return;
      }
      loadedModel = gltf.scene;
      modelRef.current = gltf.scene;
      const bounds = new THREE.Box3().setFromObject(gltf.scene);
      if (!bounds.isEmpty()) gltf.scene.position.sub(bounds.getCenter(new THREE.Vector3()));
      scene.add(gltf.scene);
      setProgress(100);
      setReady(true);
      controls.update();
    }, (event: { loaded: number; total: number }) => {
      if (event.total > 0 && !disposed) setProgress(Math.min(99, Math.round((event.loaded / event.total) * 100)));
    }, () => {
      if (disposed) return;
      const message = '工厂模型未能加载（网络或文件缺失）。数据侧栏与事件功能不受影响。';
      noteSceneFailure(message);
      setError(message);
    });

    // 验收探针：只读投影与计数，供 tests/ui 断言真实交互与资源释放。
    const project = (object: THREE.Object3D | null | undefined) => {
      if (!object) return null;
      const box = new THREE.Box3().setFromObject(object);
      if (box.isEmpty()) return null;
      const center = box.getCenter(new THREE.Vector3()).project(camera);
      const rect = renderer.domElement.getBoundingClientRect();
      return {
        x: window.scrollX + rect.left + ((center.x + 1) / 2) * rect.width,
        y: window.scrollY + rect.top + ((1 - center.y) / 2) * rect.height,
        visible: center.z < 1,
      };
    };
    publishProbe({
      get ready() { return Boolean(loadedModel) && !disposed; },
      projectInstance: (name: string) => project(loadedModel?.getObjectByName(name)),
      // 只读：让验收用例能确认"点击真的选中了目标实例"。
      // 投影可见不等于射线命中了目标——没有这个读数时，一次落空的点击会被
      // 静默吞掉，测试转而断言到默认选中的资产上。
      selectedInstance: () => {
        const asset = assetsRef.current.find((item) => item.asset_id === selectedAssetIdRef.current);
        return asset ? asset.scene_instance : null;
      },
      projectZone: (zone: string) => {
        const point = project(loadedModel?.getObjectByName(`ZONE_${zone}`));
        return point ? { x: point.x, y: point.y } : null;
      },
      camera: () => ({
        position: [camera.position.x, camera.position.y, camera.position.z],
        target: [controls.target.x, controls.target.y, controls.target.z],
      }),
      highlight: () => (highlightRef.current
        ? { name: highlightRef.current.name, tone: String((highlightRef.current.material as THREE.Material | undefined)?.userData.tone ?? '') }
        : null),
      rendererInfo: () => ({
        geometries: renderer.info.memory.geometries,
        textures: renderer.info.memory.textures,
        programs: renderer.info.programs?.length ?? null,
      }),
      diagnostics: () => ({ ...sceneDiagnostics }),
    } satisfies SceneProbe);

    const render = () => {
      animationId = window.requestAnimationFrame(render);
      if (document.hidden) return;
      // 相机飞行插值：分区聚焦与复位都走同一条平滑路径。
      const flight = flightRef.current;
      if (flight) {
        camera.position.lerp(flight.position, 0.12);
        controls.target.lerp(flight.target, 0.12);
        if (camera.position.distanceTo(flight.position) < 0.05 && controls.target.distanceTo(flight.target) < 0.05) {
          camera.position.copy(flight.position);
          controls.target.copy(flight.target);
          flightRef.current = null;
        }
      }
      controls.update();
      renderer.render(scene, camera);
    };
    render();

    return () => {
      disposed = true;
      window.cancelAnimationFrame(animationId);
      observer.disconnect();
      renderer.domElement.removeEventListener('pointerdown', onPointerDown);
      renderer.domElement.removeEventListener('pointerup', onPointerUp);
      renderer.domElement.removeEventListener('keydown', onKeyDown);
      controls.dispose();
      if (highlightRef.current) {
        scene.remove(highlightRef.current);
        disposeObjectTree(highlightRef.current);
        highlightRef.current = null;
      }
      if (loadedModel) disposeObjectTree(loadedModel);
      scene.clear();
      disposeRenderer(renderer);
      sceneRef.current = null;
      modelRef.current = null;
      rendererRef.current = null;
      controlsRef.current = null;
      cameraRef.current = null;
      host.replaceChildren();
      publishProbe(null);
      noteSceneUnmounted();
    };
  }, []);

  // ---------- 选中高亮 ----------
  useEffect(() => {
    const scene = sceneRef.current;
    const model = modelRef.current;
    if (!scene || !model || !ready) return;
    if (highlightRef.current) {
      scene.remove(highlightRef.current);
      disposeObjectTree(highlightRef.current);
      highlightRef.current = null;
    }
    const asset = assets.find((item) => item.asset_id === selectedAssetId);
    if (!asset) return;
    // 优先高亮传感器安装位置；模型中没有该节点时高亮整个绑定设备。
    const node = (asset.sensor_node ? model.getObjectByName(asset.sensor_node) : null)
      ?? model.getObjectByName(asset.scene_instance);
    if (!node) return;
    const colors: Record<SceneHighlight['tone'], number> = { critical: 0xde3555, warning: 0xf59e0b, ok: 0x1e6be5, unknown: 0x94a3b8 };
    const tone = highlight?.tone ?? 'ok';
    const helper = new THREE.Box3Helper(new THREE.Box3().setFromObject(node), colors[tone]);
    helper.name = `selection-highlight:${node.name}`;
    const helperMaterial = Array.isArray(helper.material) ? helper.material[0] : helper.material;
    helperMaterial.userData.tone = tone;
    scene.add(helper);
    highlightRef.current = helper;
  }, [assets, selectedAssetId, highlight, ready]);

  // ---------- 分区聚焦 ----------
  useEffect(() => {
    const model = modelRef.current;
    const camera = cameraRef.current;
    if (!focusZone || !model || !camera || !ready) return;
    const frame = frameObject(model.getObjectByName(`ZONE_${focusZone}`), camera);
    if (frame) flightRef.current = frame;
  }, [focusZone, ready]);

  const zones = useMemo(() => assets.map((asset) => asset.zone).filter((zone, index, list) => list.indexOf(zone) === index), [assets]);

  return (
    <div className="scene" aria-label="六区工厂数字孪生场景">
      <div className="scene-host" ref={hostRef} />

      {!ready && !error && (
        <div className="scene-overlay scene-loading" role="status">
          正在加载工厂模型 {progress}%
        </div>
      )}

      {ready && (
        <>
          <div className="scene-overlay scene-zone-chips" role="group" aria-label="分区聚焦">
            {zones.map((zone) => (
              <button
                key={zone}
                type="button"
                aria-pressed={focusZone === zone}
                onClick={() => onFocusZoneRef.current?.(focusZone === zone ? null : zone)}
              >
                {zoneMeta(zone).display_name_zh}
              </button>
            ))}
          </div>
          <div className="scene-overlay scene-tools">
            <button
              type="button"
              onClick={() => {
                const camera = cameraRef.current;
                const controls = controlsRef.current;
                if (!camera || !controls) return;
                flightRef.current = null;
                camera.position.copy(HOME_POSITION);
                controls.target.set(0, 0, 0);
                controls.update();
              }}
            >
              复位视角
            </button>
          </div>
          <div className="scene-overlay scene-tip">
            拖动旋转 · 滚轮缩放 · 点击设备 · 键盘方向键旋转 · +/- 缩放 · Home 复位 · Enter 聚焦所选分区
          </div>
        </>
      )}

      {error && (
        <div className="scene-fallback" role="alert">
          <strong>模型暂不可用</strong>
          <span>{error}</span>
          <span>静态预览：六区布局为 A 生产装置 / B 旋转设备 / C 能源与公用工程 / D 仓储物流 / E 维护检修 / F IT·OT 中心。</span>
        </div>
      )}
    </div>
  );
}
