import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { loadGlb } from './loader';

function dispose(root: THREE.Object3D): void {
  const textures = new Set<THREE.Texture>();
  root.traverse((child) => {
    if (!(child instanceof THREE.Mesh)) return;
    child.geometry.dispose();
    const materials = Array.isArray(child.material) ? child.material : [child.material];
    for (const material of materials) {
      for (const value of Object.values(material)) if (value instanceof THREE.Texture) textures.add(value);
      material.dispose();
    }
  });
  for (const texture of textures) texture.dispose();
}

export function createPreview(container: HTMLElement) {
  const renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
  renderer.setClearColor(0x101823);
  renderer.domElement.setAttribute('aria-label', '3D model preview; drag to orbit, scroll to zoom');
  renderer.domElement.tabIndex = 0;
  container.append(renderer.domElement);
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(45, 1, 0.01, 1000);
  camera.position.set(2, 1, 3);
  const controls = new OrbitControls(camera, renderer.domElement);
  controls.listenToKeyEvents(renderer.domElement);
  const light = new THREE.DirectionalLight(0xffffff, 3);
  light.position.set(3, 5, 7);
  scene.add(light, new THREE.HemisphereLight(0xddeeff, 0x445566, 2));
  const render = () => renderer.render(scene, camera);
  controls.addEventListener('change', render);
  const observer = new ResizeObserver(() => {
    renderer.setSize(Math.max(container.clientWidth, 1), Math.max(container.clientHeight, 1), false);
    camera.aspect = Math.max(container.clientWidth, 1) / Math.max(container.clientHeight, 1);
    camera.updateProjectionMatrix();
    render();
  });
  observer.observe(container);
  const supported = new Set(renderer.getContext().getParameter(renderer.getContext().COMPRESSED_TEXTURE_FORMATS));
  let model: THREE.Object3D | undefined;
  let disposed = false, generation = 0;
  return {
    async load(bytes: ArrayBuffer): Promise<string> {
      const current = ++generation;
      const loaded = await loadGlb(bytes, (format) => supported.has(format));
      if (disposed || current !== generation) { dispose(loaded.scene); return ''; }
      const box = new THREE.Box3().setFromObject(loaded.scene);
      const center = box.getCenter(new THREE.Vector3());
      const size = box.getSize(new THREE.Vector3());
      const radius = Math.max(size.length() / 2, 0.01);
      if (!Number.isFinite(radius) || !Number.isFinite(center.length())) {
        dispose(loaded.scene);
        throw new Error('Model has invalid bounds');
      }
      if (model) { scene.remove(model); dispose(model); }
      model = loaded.scene;
      scene.add(model);
      camera.near = Math.max(radius / 1000, 0.0001);
      camera.far = radius * 100;
      camera.position.copy(center).add(new THREE.Vector3(0.5, 0.3, 1).normalize().multiplyScalar(radius * 3.2));
      camera.updateProjectionMatrix();
      controls.target.copy(center);
      controls.update();
      render();
      let triangles = 0;
      model.traverse((child) => {
        if (child instanceof THREE.Mesh) triangles += (child.geometry.index?.count ?? child.geometry.getAttribute('position').count) / 3;
      });
      return `${triangles.toLocaleString()} triangles | Drag to orbit | Scroll to zoom`;
    },
    dispose(): void {
      disposed = true;
      generation++;
      observer.disconnect();
      controls.dispose();
      if (model) dispose(model);
      renderer.dispose();
      renderer.domElement.remove();
    },
  };
}
