// 设备详情模型。规格 §6：进入详情页后才加载，加载失败不影响状态与事件数据；
// 报警时只高亮传感器安装位置，不给故障部件级结论。

import { useEffect, useRef, useState } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { disposeObjectTree, disposeRenderer, noteSceneFailure, noteSceneMounted, noteSceneUnmounted } from './modelUtils';

type Props = {
  modelPath: string;
  sensorNode: string | null;
  highlight: 'ok' | 'warning' | 'critical' | 'unknown';
  label: string;
};

const HIGHLIGHT_COLORS: Record<Props['highlight'], string> = {
  ok: '#1e6be5',
  warning: '#f59e0b',
  critical: '#ef4444',
  unknown: '#94a3b8',
};

export function MotorScene({ modelPath, sensorNode, highlight, label }: Props) {
  const hostRef = useRef<HTMLDivElement>(null);
  const [error, setError] = useState<string | null>(null);
  const [ready, setReady] = useState(false);
  const modelRef = useRef<THREE.Object3D | null>(null);
  const highlightRef = useRef<Props['highlight']>(highlight);
  useEffect(() => { highlightRef.current = highlight; }, [highlight]);

  useEffect(() => {
    const host = hostRef.current;
    if (!host) return;
    let disposed = false;
    let frame = 0;
    let loadedModel: THREE.Object3D | null = null;

    const scene = new THREE.Scene();
    scene.background = new THREE.Color('#eef4fb');
    const camera = new THREE.PerspectiveCamera(38, 1, 0.05, 100);
    camera.position.set(0.55, 0.4, 0.75);

    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    host.replaceChildren(renderer.domElement);
    renderer.domElement.tabIndex = 0;
    renderer.domElement.setAttribute('role', 'application');
    renderer.domElement.setAttribute('aria-label', `${label}：方向键旋转，加减号缩放，Home 复位`);
    noteSceneMounted();

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.enablePan = false;
    controls.minDistance = 0.25;
    controls.maxDistance = 2.4;

    scene.add(new THREE.HemisphereLight(0xffffff, 0x9aa9bb, 2.4));
    const key = new THREE.DirectionalLight(0xffffff, 2.6);
    key.position.set(3, 5, 3);
    scene.add(key);

    const resize = () => {
      const rect = host.getBoundingClientRect();
      if (!rect.width || !rect.height) return;
      camera.aspect = rect.width / rect.height;
      camera.updateProjectionMatrix();
      renderer.setSize(rect.width, rect.height, false);
    };
    resize();
    const observer = new ResizeObserver(resize);
    observer.observe(host);

    const home = () => {
      camera.position.set(0.55, 0.4, 0.75);
      controls.target.set(0, 0, 0);
      controls.update();
    };
    const onKeyDown = (event: KeyboardEvent) => {
      const offset = camera.position.clone().sub(controls.target);
      const spherical = new THREE.Spherical().setFromVector3(offset);
      if (event.key === 'Home') home();
      else if (event.key === 'ArrowLeft') spherical.theta += 0.15;
      else if (event.key === 'ArrowRight') spherical.theta -= 0.15;
      else if (event.key === 'ArrowUp') spherical.phi = Math.max(0.2, spherical.phi - 0.12);
      else if (event.key === 'ArrowDown') spherical.phi = Math.min(Math.PI - 0.2, spherical.phi + 0.12);
      else if (event.key === '+' || event.key === '=') spherical.radius = Math.max(controls.minDistance, spherical.radius * 0.86);
      else if (event.key === '-' || event.key === '_') spherical.radius = Math.min(controls.maxDistance, spherical.radius * 1.16);
      else return;
      if (event.key !== 'Home') camera.position.copy(controls.target).add(new THREE.Vector3().setFromSpherical(spherical));
      controls.update();
      event.preventDefault();
    };
    renderer.domElement.addEventListener('keydown', onKeyDown);

    new GLTFLoader().load(`${import.meta.env.BASE_URL}${modelPath}`, (gltf: { scene: THREE.Object3D }) => {
      if (disposed) {
        disposeObjectTree(gltf.scene);
        return;
      }
      loadedModel = gltf.scene;
      modelRef.current = gltf.scene;
      const box = new THREE.Box3().setFromObject(gltf.scene);
      if (!box.isEmpty()) gltf.scene.position.sub(box.getCenter(new THREE.Vector3()));
      scene.add(gltf.scene);
      setReady(true);
      controls.update();
      applyHighlight(gltf.scene, sensorNode, highlightRef.current);
    }, undefined, () => {
      if (disposed) return;
      const message = `${label}模型未能加载。设备状态与趋势不受影响。`;
      noteSceneFailure(message);
      setError(message);
    });

    const render = () => {
      frame = window.requestAnimationFrame(render);
      if (document.hidden) return;
      controls.update();
      renderer.render(scene, camera);
    };
    render();

    return () => {
      disposed = true;
      window.cancelAnimationFrame(frame);
      observer.disconnect();
      renderer.domElement.removeEventListener('keydown', onKeyDown);
      controls.dispose();
      if (loadedModel) disposeObjectTree(loadedModel);
      scene.clear();
      disposeRenderer(renderer);
      modelRef.current = null;
      host.replaceChildren();
      noteSceneUnmounted();
    };
  }, [modelPath, sensorNode, label]);

  // 状态色调变化只更新材质高亮，不重建场景、不重新下载模型。
  useEffect(() => {
    const model = modelRef.current;
    if (!model) return;
    applyHighlight(model, sensorNode, highlight);
  }, [highlight, sensorNode]);

  return (
    <div className="motor-scene" aria-label={`${label}设备模型，可旋转缩放`}>
      <div className="scene-host" ref={hostRef} />
      {!ready && !error && <div className="scene-state-tag" role="status">正在加载设备模型…</div>}
      {error && (
        <div className="scene-fallback" role="alert">
          <strong>设备模型暂不可用</strong>
          <span>{error}</span>
        </div>
      )}
    </div>
  );
}

/** 只高亮传感器安装位置；模型缺少该节点时不做部件级提示。 */
function applyHighlight(model: THREE.Object3D, sensorNode: string | null, tone: Props['highlight']): void {
  const target = sensorNode ? model.getObjectByName(sensorNode) : null;
  const color = new THREE.Color(HIGHLIGHT_COLORS[tone]);
  model.traverse((node: THREE.Object3D) => {
    const mesh = node as THREE.Mesh;
    const material = mesh.material as THREE.MeshStandardMaterial | THREE.MeshStandardMaterial[] | undefined;
    if (!material) return;
    const list = Array.isArray(material) ? material : [material];
    const isTarget = target !== null && target !== undefined && (node === target || isDescendantOf(node, target));
    for (const item of list) {
      if (!('emissive' in item) || !item.emissive) continue;
      if (isTarget) {
        item.emissive.copy(color);
        item.emissiveIntensity = tone === 'ok' ? 0.25 : 0.55;
      } else if (item.emissiveIntensity && item.emissive.getHex() !== 0x000000) {
        item.emissive.setHex(0x000000);
        item.emissiveIntensity = 1;
      }
    }
  });
}

function isDescendantOf(node: THREE.Object3D, ancestor: THREE.Object3D): boolean {
  let current: THREE.Object3D | null = node.parent;
  while (current) {
    if (current === ancestor) return true;
    current = current.parent;
  }
  return false;
}
