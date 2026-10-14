import * as THREE from 'three';
import type { PanelPlane, Vec3 } from '../../math/projection';
import type { TerminalPreset } from './presets';

export interface StagePalette {
  readonly background: string;
  readonly raised: string;
  readonly floor: string;
  readonly border: string;
  readonly accent: string;
  readonly grid: string;
  readonly gridMinor: string;
}

export function createEnvironment(preset: TerminalPreset, palette: StagePalette) {
  const group = new THREE.Group();
  const obstacles: THREE.Mesh[] = [];
  const box = (size: Vec3, position: Vec3, color: string, occludes = true) => {
    const mesh = new THREE.Mesh(new THREE.BoxGeometry(...size), new THREE.MeshStandardMaterial({ color, roughness: 0.6, metalness: 0.35 }));
    mesh.position.set(...position);
    group.add(mesh);
    if (occludes) obstacles.push(mesh);
    return mesh;
  };
  box([12, 0.1, 12], [0, -0.05, 0], palette.floor);
  const grid = new THREE.GridHelper(12, 24, palette.grid, palette.gridMinor);
  grid.position.y = 0.01;
  group.add(grid);
  const terminal = new THREE.Group();
  terminal.position.set(0, 1.8, 0);
  terminal.rotation.x = preset.tilt;
  group.add(terminal);
  if (preset.id !== 'hud') {
    const backing = box([preset.width + 0.12, preset.height + 0.12, 0.16], [0, 0, 0], palette.raised);
    terminal.add(backing);
    backing.position.z = -0.09;
    box([0.3, 1.1, 0.4], [0, 0.55, -0.25], palette.border);
  }
  switch (preset.id) {
    case 'elevator':
      box([2.5, 3.8, 0.25], [-2, 1.9, -0.65], palette.raised);
      box([2.5, 3.8, 0.25], [2, 1.9, -0.65], palette.raised);
      box([0.04, 3.4, 0.08], [-0.7, 1.7, -0.45], palette.accent);
      box([0.04, 3.4, 0.08], [0.7, 1.7, -0.45], palette.accent);
      break;
    case 'asop':
      box([2.3, 0.25, 1.4], [0, 0.2, 0], palette.border);
      box([0.2, 3.6, 0.3], [-2.5, 1.8, -1.5], palette.accent);
      box([0.2, 3.6, 0.3], [2.5, 1.8, -1.5], palette.accent);
      break;
    case 'shop':
      for (const side of [-1, 1]) {
        for (const y of [0.6, 1.4, 2.2]) box([1.4, 0.12, 0.8], [side * 2, y, -0.5], palette.border);
        box([0.35, 0.5, 0.4], [side * 2, 1.7, -0.5], palette.accent);
      }
      break;
    case 'engineering':
      box([3, 0.5, 1.8], [0, 0.5, -0.3], palette.border);
      for (const x of [-2.5, 2.5]) {
        box([0.7, 2.8, 1], [x, 1.4, -1.5], palette.raised);
        box([0.1, 2.4, 0.1], [x, 1.4, -0.95], palette.accent);
      }
      break;
    case 'cockpit':
      box([4, 0.6, 1.4], [0, 0.65, -0.3], palette.border);
      for (const x of [-2, 2]) {
        const pillar = box([0.18, 3.5, 0.2], [x, 2.2, -1.7], palette.grid);
        pillar.rotation.z = x > 0 ? -0.25 : 0.25;
      }
      break;
    case 'hud':
      for (const x of [-2, 2]) box([0.12, 0.12, 0.12], [x, 1.8, -0.4], palette.accent, false);
      break;
  }
  group.updateMatrixWorld(true);
  const world = (x: number, y: number): Vec3 => {
    const v = terminal.localToWorld(new THREE.Vector3(x, y, 0.01));
    return [v.x, v.y, v.z];
  };
  const origin = world(-preset.width / 2, preset.height / 2);
  const right = new THREE.Vector3(preset.width, 0, 0).transformDirection(terminal.matrixWorld).multiplyScalar(preset.width);
  const down = new THREE.Vector3(0, -1, 0).transformDirection(terminal.matrixWorld).multiplyScalar(preset.height);
  const plane: PanelPlane = { origin, right: [right.x, right.y, right.z], down: [down.x, down.y, down.z] };
  return { group, obstacles, plane, center: world(0, 0) };
}

export function disposeEnvironment(group: THREE.Group): void {
  group.traverse((object) => {
    if (object instanceof THREE.Mesh || object instanceof THREE.LineSegments) {
      object.geometry.dispose();
      const materials = Array.isArray(object.material) ? object.material : [object.material];
      for (const material of materials) material.dispose();
    }
  });
}
