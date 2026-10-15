import * as THREE from 'three';
import { computeHomography, homographyMatrix3d } from '../../math/homography';
import { anchorOccluded, billboardScale, cross, dot, intersectPanel, length, projectPoint,
  projectQuad, scale, screenRay, subtract } from '../../math/projection';
import type { Quad, Vec2, Vec3, Viewport } from '../../math/projection';
import { createControls } from './controls';
import type { StageState } from './controls';
import { terminalPresets } from './presets';
import { createEnvironment, disposeEnvironment, type StagePalette } from './scene';
import { element, terminalSurface } from './surface';
import type { PanelSurface } from './surface';
import type { HostdBridge } from '../../bridge';
import { connectStage, type MockCamera } from './connection';
import './viewport.css';

export interface StageOptions {
  /** A DOM renderer can mount arbitrary HTML or sco.widgets into this surface. */
  readonly surface?: PanelSurface;
  readonly bridge?: HostdBridge;
}

export interface StageDOM {
  root: HTMLElement;
  stage: HTMLElement;
  overlay: HTMLElement;
  wrapper: HTMLElement;
  stageLabel: HTMLElement;
  visibility: HTMLElement;
  projection: HTMLSpanElement;
  inputReadout: HTMLSpanElement;
  controls: HTMLElement;
}

export interface StageScene {
  palette: StagePalette;
  scene: THREE.Scene;
  camera: THREE.PerspectiveCamera;
  raycaster: THREE.Raycaster;
  blocker: THREE.Mesh;
  environment: ReturnType<typeof createEnvironment>;
}

export interface SurfaceProjectionResult {
  occluded: boolean;
  visible: boolean;
  billboard: { center: Vec2; factor: number } | null;
  distance: number;
}

function createStageDOM(state: StageState, onControlsChange: (presetChanged: boolean) => void): StageDOM {
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
  const controls = createControls(state, onControlsChange);
  root.append(title, controls, stage, readout);

  return { root, stage, overlay, wrapper, stageLabel, visibility, projection, inputReadout, controls };
}

function createStageScene(root: HTMLElement, state: StageState): StageScene {
  const style = getComputedStyle(root);
  const color = (name: string) => style.getPropertyValue(`--vp-${name}`).trim();
  const palette: StagePalette = {
    background: color('bg'),
    raised: color('raised'),
    floor: color('floor'),
    border: color('border'),
    accent: color('accent'),
    grid: color('grid'),
    gridMinor: color('grid-minor'),
  };
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
  const environment = createEnvironment(terminalPresets[state.preset], palette);
  scene.add(environment.group);

  return { palette, scene, camera, raycaster, blocker, environment };
}

function updateCamera(
  camera: THREE.PerspectiveCamera,
  state: StageState,
  target: THREE.Vector3,
  dimensions: Viewport,
  mockCamera: MockCamera | null,
): void {
  const yaw = THREE.MathUtils.degToRad(state.yaw);
  const pitch = THREE.MathUtils.degToRad(state.pitch);
  camera.fov = state.fov;
  camera.aspect = dimensions.width / dimensions.height;
  camera.updateProjectionMatrix();
  camera.position.copy(target).add(new THREE.Vector3(
    Math.sin(yaw) * Math.cos(pitch), Math.sin(pitch), Math.cos(yaw) * Math.cos(pitch),
  ).multiplyScalar(state.distance));
  camera.lookAt(target);
  if (mockCamera) {
    camera.position.set(...mockCamera.pos);
    const basis = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(1, 0, 0), -Math.PI / 2);
    camera.quaternion.set(...mockCamera.rot).premultiply(basis).multiply(basis.clone().invert());
  }
  camera.updateMatrixWorld(true);
}

function updateSurfaceProjection(
  camera: THREE.PerspectiveCamera,
  environment: ReturnType<typeof createEnvironment>,
  state: StageState,
  dimensions: Viewport,
  surface: PanelSurface,
  raycaster: THREE.Raycaster,
  blocker: THREE.Mesh,
  wrapper: HTMLElement,
): SurfaceProjectionResult {
  const preset = terminalPresets[state.preset];
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
  let billboard: { center: Vec2; factor: number } | null = null;

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
  return { occluded, visible, billboard, distance };
}

function calculatePointerHit(
  event: PointerEvent,
  stage: HTMLElement,
  mode: 'quad' | 'billboard',
  camera: THREE.PerspectiveCamera,
  dimensions: Viewport,
  environment: ReturnType<typeof createEnvironment>,
  surface: PanelSurface,
  billboard: { center: Vec2; factor: number } | null,
): Vec2 | null {
  const rect = stage.getBoundingClientRect();
  const screen: Vec2 = [event.clientX - rect.left, event.clientY - rect.top];
  switch (mode) {
    case 'quad': {
      const inverse = new THREE.Matrix4().multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse).invert();
      const uv = intersectPanel(screenRay(screen, inverse.elements, dimensions), environment.plane);
      return uv ? [uv[0] * surface.width, uv[1] * surface.height] : null;
    }
    case 'billboard':
      if (billboard) {
        return [
          (screen[0] - billboard.center[0]) / billboard.factor + surface.width / 2,
          (screen[1] - billboard.center[1]) / billboard.factor + surface.height / 2,
        ];
      }
      return null;
  }
}

export function mountDiegeticStage(container: HTMLElement, options: StageOptions = {}): () => void {
  const state: StageState = { preset: 1, mode: 'quad', yaw: 25, pitch: 12, distance: 3.8, fov: 50, occlusion: false };

  const dom = createStageDOM(state, (presetChanged) => {
    if (presetChanged) rebuild();
    render();
  });
  container.append(dom.root);

  let renderer: THREE.WebGLRenderer;
  try {
    renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
  } catch (error) {
    dom.stage.textContent = error instanceof Error ? `WebGL unavailable: ${error.message}` : 'WebGL unavailable.';
    dom.controls.inert = true;
    return () => dom.root.remove();
  }

  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  renderer.domElement.setAttribute('aria-label', 'Synthetic terminal environment, controlled by the labeled camera sliders');
  dom.stage.append(renderer.domElement, dom.overlay, dom.stageLabel);

  const sceneState = createStageScene(dom.root, state);
  let { environment } = sceneState;
  const { scene, camera, raycaster, blocker, palette } = sceneState;

  let surface: PanelSurface;
  let dimensions: Viewport = { width: 1, height: 1 };
  let billboard: { center: Vec2; factor: number } | null = null;
  let pointer: Vec2 | null = null;
  let actionCount = 0;
  let mockCamera: MockCamera | null = null;
  let surfaceReady = false;

  const connection = options.bridge ? connectStage(options.bridge, pose => {
    mockCamera = pose;
    if (surfaceReady) render();
  }) : null;
  if (connection) dom.controls.append(connection.controls);

  function installSurface(): void {
    surface = options.surface ?? connection?.surface ?? terminalSurface(terminalPresets[state.preset], (action) => {
      actionCount++;
      const coordinates = pointer ? `${pointer[0].toFixed(1)}, ${pointer[1].toFixed(1)} px` : 'keyboard';
      dom.inputReadout.textContent = `Input: ${action} / ${coordinates} / #${actionCount}`;
      const status = dom.wrapper.querySelector('.vp-terminal-status');
      if (status) status.textContent = `${action} acknowledged`;
      pointer = null;
    });
    dom.wrapper.replaceChildren(surface.element);
    dom.wrapper.style.width = `${surface.width}px`;
    dom.wrapper.style.height = `${surface.height}px`;
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
  surfaceReady = true;

  function render(): void {
    const preset = terminalPresets[state.preset];
    const target = new THREE.Vector3(...environment.center);

    updateCamera(camera, state, target, dimensions, mockCamera);

    blocker.visible = state.occlusion;
    blocker.position.copy(target).lerp(camera.position, 0.5);
    blocker.lookAt(camera.position);
    scene.updateMatrixWorld(true);
    renderer.render(scene, camera);

    const { occluded, visible, billboard: newBillboard, distance } = updateSurfaceProjection(
      camera, environment, state, dimensions, surface, raycaster, blocker, dom.wrapper,
    );
    billboard = newBillboard;

    dom.root.dataset.ready = 'true';
    dom.root.dataset.mode = state.mode;
    dom.root.dataset.visible = String(visible);
    dom.root.dataset.camera = mockCamera ? 'mock' : 'orbit';
    dom.root.dataset.cameraPosition = camera.position.toArray().join(',');
    dom.visibility.textContent = occluded ? 'OCCLUDED / ray blocked' : visible ? 'VISIBLE / input enabled' : 'CLIPPED / input disabled';
    dom.projection.textContent = `${state.mode === 'quad' ? 'QUAD_3D' : 'BILLBOARD'} / ${distance.toFixed(1)} m / ${state.fov} deg`;
    dom.stageLabel.textContent = `${preset.category} / ${preset.title.toUpperCase()}`;
  }

  dom.wrapper.addEventListener('pointerdown', (event) => {
    if (dom.wrapper.hidden) return;
    const local = calculatePointerHit(event, dom.stage, state.mode, camera, dimensions, environment, surface, billboard);
    pointer = local;
    if (local) {
      dom.inputReadout.textContent = `Hit: ${local[0].toFixed(1)}, ${local[1].toFixed(1)} px`;
      surface.onPointer?.(local, event);
      dom.wrapper.dispatchEvent(new CustomEvent<Vec2>('diegetic-pointer', { detail: local, bubbles: true }));
    }
  });

  dom.wrapper.addEventListener('keydown', () => { pointer = null; });

  const resize = () => {
    dimensions = { width: Math.max(dom.stage.clientWidth, 1), height: Math.max(dom.stage.clientHeight, 1) };
    renderer.setSize(dimensions.width, dimensions.height, false);
    render();
  };

  const observer = new ResizeObserver(resize);
  observer.observe(dom.stage);
  resize();

  return () => {
    connection?.cleanup();
    observer.disconnect();
    disposeEnvironment(environment.group);
    blocker.geometry.dispose();
    if (Array.isArray(blocker.material)) {
      for (const mat of blocker.material) mat.dispose();
    } else {
      blocker.material.dispose();
    }
    renderer.dispose();
    dom.root.remove();
  };
}
