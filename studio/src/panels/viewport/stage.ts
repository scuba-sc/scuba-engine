import * as THREE from 'three';
import { computeHomography, homographyMatrix3d } from '../../math/homography';
import { anchorOccluded, billboardScale, cross, dot, intersectPanel, length, projectPoint,
  projectQuad, scale, screenRay, subtract } from '../../math/projection';
import type { Quad, Vec2, Vec3, Viewport } from '../../math/projection';
import { createControls } from './controls';
import type { StageState } from './controls';
import { terminalPresets } from './presets';
import { createEnvironment, disposeEnvironment } from './scene';
import { element, terminalSurface } from './surface';
import type { PanelSurface } from './surface';
import './viewport.css';

export interface StageOptions {
  /** A DOM renderer can mount arbitrary HTML or sco.widgets into this surface. */
  readonly surface?: PanelSurface;
}

export function mountDiegeticStage(container: HTMLElement, options: StageOptions = {}): () => void {
  const state: StageState = { preset: 1, mode: 'quad', yaw: 25, pitch: 12, distance: 3.8, fov: 50, occlusion: false };
  const root = element('section', undefined, 'diegetic');
  const title = element('div', undefined, 'vp-title');
  const text = element('div');
  text.append(element('h2', 'Diegetic stage'), element('p', 'World-space panels / inspect projection and input without a game'));
  title.append(text, element('span', 'SYNTHETIC / LOCAL', 'vp-tag'));
  const stage = element('div', undefined, 'vp-stage');
  const overlay = element('div', undefined, 'vp-overlay');
  const wrapper = element('div', undefined, 'vp-surface');
  overlay.append(wrapper);
  const stageLabel = element('div', undefined, 'vp-stage-label');
  const readout = element('div', undefined, 'vp-readout');
  const visibility = element('strong');
  const projection = element('span');
  const inputReadout = element('span', 'Input: awaiting interaction');
  inputReadout.setAttribute('role', 'status');
  readout.append(visibility, projection, inputReadout);
  const controls = createControls(state, (presetChanged) => {
    if (presetChanged) rebuild();
    render();
  });
  root.append(title, controls, stage, readout);
  container.append(root);
  let renderer: THREE.WebGLRenderer;
  try {
    renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
  } catch (error) {
    stage.textContent = error instanceof Error ? `WebGL unavailable: ${error.message}` : 'WebGL unavailable.';
    controls.inert = true;
    return () => root.remove();
  }
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.domElement.setAttribute('aria-label', 'Synthetic terminal environment, controlled by the labeled camera sliders');
  stage.append(renderer.domElement, overlay, stageLabel);
  const style = getComputedStyle(root);
  const color = (name: string) => style.getPropertyValue(`--vp-${name}`).trim();
  const palette = { background: color('bg'), raised: color('raised'), floor: color('floor'),
    border: color('border'), accent: color('accent'), grid: color('grid'), gridMinor: color('grid-minor') };
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(palette.background);
  scene.fog = new THREE.Fog(palette.background, 9, 22);
  scene.add(new THREE.HemisphereLight(palette.accent, palette.floor, 2.2));
  const light = new THREE.DirectionalLight(palette.accent, 3);
  light.position.set(3, 6, 4);
  scene.add(light);
  const camera = new THREE.PerspectiveCamera(state.fov, 1, 0.1, 50);
  const raycaster = new THREE.Raycaster();
  const blocker = new THREE.Mesh(new THREE.BoxGeometry(2.4, 2.8, 0.18), new THREE.MeshStandardMaterial({ color: palette.border }));
  scene.add(blocker);
  let environment = createEnvironment(terminalPresets[state.preset], palette);
  scene.add(environment.group);
  let surface: PanelSurface;
  let dimensions: Viewport = { width: 1, height: 1 };
  let billboard: { center: Vec2; factor: number } | null = null;
  let pointer: Vec2 | null = null;
  let actionCount = 0;

  function installSurface(): void {
    surface = options.surface ?? terminalSurface(terminalPresets[state.preset], (action) => {
      actionCount++;
      const coordinates = pointer ? `${pointer[0].toFixed(1)}, ${pointer[1].toFixed(1)} px` : 'keyboard';
      inputReadout.textContent = `Input: ${action} / ${coordinates} / #${actionCount}`;
      const status = wrapper.querySelector('.vp-terminal-status');
      if (status) status.textContent = `${action} acknowledged`;
      pointer = null;
    });
    wrapper.replaceChildren(surface.element);
    wrapper.style.width = `${surface.width}px`;
    wrapper.style.height = `${surface.height}px`;
  }
  function rebuild(): void {
    scene.remove(environment.group);
    disposeEnvironment(environment.group);
    environment = createEnvironment(terminalPresets[state.preset], palette);
    scene.add(environment.group);
    pointer = null;
    installSurface();
  }
  installSurface();

  function render(): void {
    const preset = terminalPresets[state.preset];
    const yaw = THREE.MathUtils.degToRad(state.yaw);
    const pitch = THREE.MathUtils.degToRad(state.pitch);
    camera.fov = state.fov;
    camera.aspect = dimensions.width / dimensions.height;
    camera.updateProjectionMatrix();
    const target = new THREE.Vector3(...environment.center);
    camera.position.copy(target).add(new THREE.Vector3(
      Math.sin(yaw) * Math.cos(pitch), Math.sin(pitch), Math.cos(yaw) * Math.cos(pitch),
    ).multiplyScalar(state.distance));
    camera.lookAt(target);
    camera.updateMatrixWorld(true);
    blocker.visible = state.occlusion;
    blocker.position.copy(target).lerp(camera.position, 0.5);
    blocker.lookAt(camera.position);
    scene.updateMatrixWorld(true);
    renderer.render(scene, camera);
    const viewProjection = new THREE.Matrix4().multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse);
    const position: Vec3 = [camera.position.x, camera.position.y, camera.position.z];
    const delta = subtract(environment.center, position);
    const distance = length(delta);
    raycaster.set(camera.position, new THREE.Vector3(...scale(delta, 1 / distance)));
    const intersections = raycaster.intersectObjects(state.occlusion ? [...environment.obstacles, blocker] : environment.obstacles);
    const occluded = anchorOccluded(distance, intersections[0]?.distance ?? null);
    const anchor = projectPoint(environment.center, viewProjection.elements, dimensions);
    const facing = dot(cross(environment.plane.down, environment.plane.right), subtract(position, environment.center)) > 1e-8;
    let visible = !occluded && facing && anchor !== null;
    billboard = null;
    if (visible && anchor) {
      switch (state.mode) {
        case 'billboard': {
          const factor = billboardScale(distance, dimensions.height, THREE.MathUtils.degToRad(state.fov), preset.height / surface.height);
          billboard = { center: anchor.screen, factor };
          wrapper.style.transform = `translate(${anchor.screen[0] - surface.width * factor / 2}px, ${anchor.screen[1] - surface.height * factor / 2}px) scale(${factor})`;
          visible = anchor.inside;
          break;
        }
        case 'quad': {
          const corners = projectQuad(environment.plane, viewProjection.elements, dimensions);
          const local: Quad<Vec2> = [[0, 0], [surface.width, 0], [surface.width, surface.height], [0, surface.height]];
          const homography = corners && computeHomography(local, corners);
          visible = homography !== null;
          if (homography) wrapper.style.transform = `matrix3d(${homographyMatrix3d(homography).join(',')})`;
          break;
        }
      }
    }
    wrapper.hidden = !visible;
    root.dataset.ready = 'true';
    root.dataset.mode = state.mode;
    root.dataset.visible = String(visible);
    visibility.textContent = occluded ? 'OCCLUDED / ray blocked' : visible ? 'VISIBLE / input enabled' : 'CLIPPED / input disabled';
    projection.textContent = `${state.mode === 'quad' ? 'QUAD_3D' : 'BILLBOARD'} / ${distance.toFixed(1)} m / ${state.fov} deg`;
    stageLabel.textContent = `${preset.category} / ${preset.title.toUpperCase()}`;
  }

  wrapper.addEventListener('pointerdown', (event) => {
    if (wrapper.hidden) return;
    const rect = stage.getBoundingClientRect();
    const screen: Vec2 = [event.clientX - rect.left, event.clientY - rect.top];
    let local: Vec2 | null = null;
    switch (state.mode) {
      case 'quad': {
        const inverse = new THREE.Matrix4().multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse).invert();
        const uv = intersectPanel(screenRay(screen, inverse.elements, dimensions), environment.plane);
        if (uv) local = [uv[0] * surface.width, uv[1] * surface.height];
        break;
      }
      case 'billboard':
        if (billboard) local = [
          (screen[0] - billboard.center[0]) / billboard.factor + surface.width / 2,
          (screen[1] - billboard.center[1]) / billboard.factor + surface.height / 2,
        ];
        break;
    }
    pointer = local;
    if (local) {
      inputReadout.textContent = `Hit: ${local[0].toFixed(1)}, ${local[1].toFixed(1)} px`;
      surface.onPointer?.(local, event);
      wrapper.dispatchEvent(new CustomEvent<Vec2>('diegetic-pointer', { detail: local, bubbles: true }));
    }
  });
  wrapper.addEventListener('keydown', () => { pointer = null; });
  const resize = () => {
    dimensions = { width: Math.max(stage.clientWidth, 1), height: Math.max(stage.clientHeight, 1) };
    renderer.setSize(dimensions.width, dimensions.height, false);
    render();
  };
  const observer = new ResizeObserver(resize);
  observer.observe(stage);
  resize();
  return () => {
    observer.disconnect();
    disposeEnvironment(environment.group);
    blocker.geometry.dispose();
    blocker.material.dispose();
    renderer.dispose();
    root.remove();
  };
}
