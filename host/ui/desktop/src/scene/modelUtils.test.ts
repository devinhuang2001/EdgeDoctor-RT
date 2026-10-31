import { describe, expect, it } from 'vitest';
import * as THREE from 'three';
import { disposeObjectTree, findSemanticRoot, frameObject, sceneDiagnostics } from './modelUtils';

function buildMeshWithTexture(): { root: THREE.Group; texture: THREE.Texture } {
  const root = new THREE.Group();
  root.name = 'MOTOR_01';
  const texture = new THREE.Texture();
  const material = new THREE.MeshStandardMaterial({ map: texture });
  const mesh = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1), material);
  mesh.name = 'MOTOR_01/Body';
  root.add(mesh);
  return { root, texture };
}

describe('场景资源释放', () => {
  it('释放几何、材质与贴图并计数', () => {
    const { root, texture } = buildMeshWithTexture();
    const before = { geometries: sceneDiagnostics.geometriesDisposed, textures: sceneDiagnostics.texturesDisposed };
    let textureDisposed = false;
    texture.addEventListener('dispose', () => { textureDisposed = true; });

    disposeObjectTree(root);

    expect(sceneDiagnostics.geometriesDisposed).toBe(before.geometries + 1);
    expect(sceneDiagnostics.texturesDisposed).toBe(before.textures + 1);
    expect(textureDisposed).toBe(true);
  });

  it('对空输入与非法输入安全', () => {
    expect(() => disposeObjectTree(null)).not.toThrow();
    expect(() => disposeObjectTree({})).not.toThrow();
  });
});

describe('语义节点查找', () => {
  it('从子网格向上找到绑定根节点', () => {
    const { root } = buildMeshWithTexture();
    const child = root.children[0];
    const found = findSemanticRoot(child, (name) => name === 'MOTOR_01');
    expect(found).toBe('MOTOR_01');
  });

  it('没有匹配根节点时返回 null', () => {
    const { root } = buildMeshWithTexture();
    expect(findSemanticRoot(root.children[0], () => false)).toBeNull();
  });
});

describe('相机取景', () => {
  it('按包围球计算可容纳目标的距离，而不是只改朝向', () => {
    const mesh = new THREE.Mesh(new THREE.BoxGeometry(2, 2, 2), new THREE.MeshBasicMaterial());
    const camera = new THREE.PerspectiveCamera(38, 1, 0.1, 100);
    camera.position.set(10, 10, 10);
    const frame = frameObject(mesh, camera);
    expect(frame).not.toBeNull();
    const distance = frame!.position.distanceTo(frame!.target);
    expect(distance).toBeGreaterThan(2);
    expect(Number.isFinite(distance)).toBe(true);
  });

  it('空对象不产生取景结果', () => {
    const empty = new THREE.Group();
    const camera = new THREE.PerspectiveCamera(38, 1, 0.1, 100);
    expect(frameObject(empty, camera)).toBeNull();
    expect(frameObject(null, camera)).toBeNull();
  });
});
