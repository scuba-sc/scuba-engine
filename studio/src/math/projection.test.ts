import { describe, expect, it } from 'vitest';
import { Matrix4, PerspectiveCamera, Vector3 } from 'three';
import { add, anchorOccluded, billboardScale, intersectPanel, panelCorners, projectPoint,
  projectQuad, scale, screenRay } from './projection';
import type { PanelPlane, Vec2, Viewport } from './projection';
import { computeHomography, homographyMatrix3d, inverseHomography, mapPoint } from './homography';

const plane: PanelPlane = { origin: [-0.9, 2.7, 0], right: [1.8, 0, 0.2], down: [0.15, -2.1, -0.3] };
const local = [[0, 0], [360, 0], [360, 420], [0, 420]] as const;
const close = (actual: Vec2 | null, expected: Vec2) => {
  expect(actual).not.toBeNull();
  if (!actual) return;
  expect(Math.abs(actual[0] - expected[0])).toBeLessThanOrEqual(1e-6);
  expect(Math.abs(actual[1] - expected[1])).toBeLessThanOrEqual(1e-6);
};

describe('projection conventions', () => {
  it('projects corners against a known perspective matrix', () => {
    const matrix = [0.5, 0, 0, 0, 0, 1, 0, 0, 0, 0, -11 / 9, -1, 0, 0, -20 / 9, 0];
    const viewport = { width: 800, height: 400 };
    const fixture: PanelPlane = { origin: [-1, 1, -2], right: [2, 0, 0], down: [0, -2, 0] };
    const corners = projectQuad(fixture, matrix, viewport);
    expect(corners).toEqual([[300, 100], [500, 100], [500, 300], [300, 300]]);
    expect(projectPoint([1, 1, -2], matrix, viewport)?.depth).toBeCloseTo(1 / 9, 12);
    expect(projectPoint([0, 0, 2], matrix, viewport)).toBeNull();
    expect(projectPoint([0, 0, -0.5], matrix, viewport)).toBeNull();
    expect(projectPoint([0, 0, -11], matrix, viewport)).toBeNull();
    expect(projectPoint([10, 0, -2], matrix, viewport)?.inside).toBe(false);
    expect(projectQuad({ ...fixture, right: [2, 0, 3] }, matrix, viewport)).toBeNull();
  });

  it('uses full world edges in TL TR BR BL order', () => {
    expect(panelCorners(plane)).toEqual([
      [-0.9, 2.7, 0], [0.9, 2.7, 0.2], [1.05, 0.6000000000000001, -0.09999999999999998],
      [-0.75, 0.6000000000000001, -0.3],
    ]);
  });

  it('halves billboard scale when distance doubles', () => {
    const first = billboardScale(4, 900, Math.PI / 3, 0.004);
    expect(billboardScale(8, 900, Math.PI / 3, 0.004)).toBe(first / 2);
    expect(billboardScale(0, 900, Math.PI / 3, 0.004)).toBe(0);
  });

  it('hides only anchors with a ray hit before the anchor', () => {
    expect(anchorOccluded(5, 2)).toBe(true);
    expect(anchorOccluded(5, null)).toBe(false);
    expect(anchorOccluded(5, 5)).toBe(false);
    expect(anchorOccluded(5, 6)).toBe(false);
    expect(anchorOccluded(5, -1)).toBe(false);
    expect(anchorOccluded(5, 0)).toBe(true);
  });

  it('rejects parallel rays, behind-camera hits, outside bounds and collapsed planes', () => {
    expect(intersectPanel({ origin: [0, 0, 2], direction: [1, 0, 0] },
      { origin: [0, 0, 0], right: [1, 0, 0], down: [0, 1, 0] })).toBeNull();
    expect(intersectPanel({ origin: [0, 0, 2], direction: [0, 0, 1] }, plane)).toBeNull();
    expect(intersectPanel({ origin: [20, 20, 2], direction: [0, 0, -1] }, plane)).toBeNull();
    expect(intersectPanel({ origin: [0, 0, 2], direction: [0, 0, -1] },
      { origin: [0, 0, 0], right: [1, 0, 0], down: [2, 0, 0] })).toBeNull();
  });
});

describe('homography and ray-plane agreement', () => {
  it('maps all corners and round-trips hits across seeded random yaw pitch FOV and distance', () => {
    let seed = 0x5c0ba;
    const random = () => {
      seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0;
      return seed / 0x100000000;
    };
    for (let sample = 0; sample < 160; sample++) {
      const viewport: Viewport = { width: sample % 2 ? 390 : 1440, height: sample % 2 ? 844 : 900 };
      const yaw = (random() - 0.5) * 2.1;
      const pitch = (random() - 0.5) * 1.3;
      const distance = 4 + random() * 6;
      const camera = new PerspectiveCamera(30 + random() * 55, viewport.width / viewport.height, 0.1, 50);
      camera.position.set(Math.sin(yaw) * Math.cos(pitch) * distance,
        1.65 + Math.sin(pitch) * distance, Math.cos(yaw) * Math.cos(pitch) * distance);
      camera.lookAt(0, 1.65, 0);
      camera.updateMatrixWorld(true);
      const vp = new Matrix4().multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse);
      const corners = projectQuad(plane, vp.elements, viewport);
      expect(corners).not.toBeNull();
      if (!corners) continue;
      const h = computeHomography(local, corners);
      expect(h).not.toBeNull();
      if (!h) continue;
      for (let i = 0; i < 4; i++) {
        close(mapPoint(h, local[i]), corners[i]);
        const projected = new Vector3(...panelCorners(plane)[i]).project(camera);
        close(corners[i], [(projected.x + 1) * viewport.width / 2, (1 - projected.y) * viewport.height / 2]);
      }
      const inverse = inverseHomography(h);
      expect(inverse).not.toBeNull();
      if (!inverse) continue;
      for (const uv of [[0, 0], [1, 1], [random(), random()], [0.5, 0.5]] as const) {
        const panelPoint: Vec2 = [uv[0] * 360, uv[1] * 420];
        const screen = mapPoint(h, panelPoint);
        expect(screen).not.toBeNull();
        if (!screen) continue;
        close(mapPoint(inverse, screen), panelPoint);
        const ray = screenRay(screen, vp.clone().invert().elements, viewport);
        const hit = intersectPanel(ray, plane);
        close(hit, uv);
        if (hit) {
          const world = add(plane.origin, add(scale(plane.right, hit[0]), scale(plane.down, hit[1])));
          close(projectPoint(world, vp.elements, viewport)?.screen ?? null, screen);
        }
        const css = homographyMatrix3d(h);
        const w = css[3] * panelPoint[0] + css[7] * panelPoint[1] + css[15];
        close([(css[0] * panelPoint[0] + css[4] * panelPoint[1] + css[12]) / w,
          (css[1] * panelPoint[0] + css[5] * panelPoint[1] + css[13]) / w], screen);
      }
    }
  });

  it('rejects singular mappings and points at projective infinity', () => {
    expect(computeHomography(local, [[0, 0], [1, 0], [2, 0], [3, 0]])).toBeNull();
    expect(inverseHomography([0, 0, 0, 0, 0, 0, 0, 0, 1])).toBeNull();
    expect(mapPoint([1, 0, 0, 0, 1, 0, 1, 0, 1], [-1, 0])).toBeNull();
  });
});
