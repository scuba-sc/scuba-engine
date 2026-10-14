export type Vec2 = readonly [number, number];
export type Vec3 = readonly [number, number, number];
/** Column-major 4x4 matrix, matching Three.js and OpenGL clip coordinates. */
export type Matrix4 = readonly number[];
export type Quad<T> = readonly [T, T, T, T];
export interface Viewport {
  readonly width: number;
  readonly height: number;
}
export interface PanelPlane {
  readonly origin: Vec3;
  readonly right: Vec3;
  readonly down: Vec3;
}
export interface Ray {
  readonly origin: Vec3;
  readonly direction: Vec3;
}

export const add = (a: Vec3, b: Vec3): Vec3 => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
export const subtract = (a: Vec3, b: Vec3): Vec3 => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
export const scale = (v: Vec3, s: number): Vec3 => [v[0] * s, v[1] * s, v[2] * s];
export const dot = (a: Vec3, b: Vec3): number => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
export const cross = (a: Vec3, b: Vec3): Vec3 => [
  a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0],
];
export const length = (v: Vec3): number => Math.hypot(...v);

export function transformPoint(point: Vec3, matrix: Matrix4): readonly [number, number, number, number] {
  const [x, y, z] = point;
  return [
    matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12],
    matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13],
    matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14],
    matrix[3] * x + matrix[7] * y + matrix[11] * z + matrix[15],
  ];
}

export function projectPoint(point: Vec3, viewProjection: Matrix4, viewport: Viewport) {
  const [x, y, z, w] = transformPoint(point, viewProjection);
  if (w <= 1e-9 || z < -w || z > w) return null;
  return {
    screen: [(x / w + 1) * viewport.width / 2, (1 - y / w) * viewport.height / 2] satisfies Vec2,
    depth: z / w,
    inside: Math.abs(x) <= w && Math.abs(y) <= w,
  };
}

/** TL, TR, BR, BL; right/down are full world-space panel edges, not unit vectors. */
export function panelCorners(plane: PanelPlane): Quad<Vec3> {
  return [plane.origin, add(plane.origin, plane.right),
    add(add(plane.origin, plane.right), plane.down), add(plane.origin, plane.down)];
}

export function projectQuad(plane: PanelPlane, viewProjection: Matrix4, viewport: Viewport): Quad<Vec2> | null {
  const points = panelCorners(plane).map((p) => projectPoint(p, viewProjection, viewport));
  const [a, b, c, d] = points;
  if (!a || !b || !c || !d) return null;
  return [a.screen, b.screen, c.screen, d.screen];
}

export function screenRay(screen: Vec2, inverseViewProjection: Matrix4, viewport: Viewport): Ray {
  const x = screen[0] / viewport.width * 2 - 1;
  const y = 1 - screen[1] / viewport.height * 2;
  const near = transformPoint([x, y, -1], inverseViewProjection);
  const far = transformPoint([x, y, 1], inverseViewProjection);
  const origin: Vec3 = [near[0] / near[3], near[1] / near[3], near[2] / near[3]];
  const end: Vec3 = [far[0] / far[3], far[1] / far[3], far[2] / far[3]];
  const direction = subtract(end, origin);
  return { origin, direction: scale(direction, 1 / length(direction)) };
}

/** Returns normalized panel-local coordinates, including skewed parallelogram planes. */
export function intersectPanel(ray: Ray, plane: PanelPlane): Vec2 | null {
  const normal = cross(plane.right, plane.down);
  const denominator = dot(normal, ray.direction);
  if (Math.abs(denominator) < 1e-9) return null;
  const t = dot(normal, subtract(plane.origin, ray.origin)) / denominator;
  if (t < 0) return null;
  const relative = subtract(add(ray.origin, scale(ray.direction, t)), plane.origin);
  const rr = dot(plane.right, plane.right);
  const dd = dot(plane.down, plane.down);
  const rd = dot(plane.right, plane.down);
  const determinant = rr * dd - rd * rd;
  if (Math.abs(determinant) < 1e-12) return null;
  const u = (dot(relative, plane.right) * dd - dot(relative, plane.down) * rd) / determinant;
  const v = (dot(relative, plane.down) * rr - dot(relative, plane.right) * rd) / determinant;
  const tolerance = 1e-8;
  return u >= -tolerance && u <= 1 + tolerance && v >= -tolerance && v <= 1 + tolerance ? [u, v] : null;
}

export function billboardScale(distance: number, viewportHeight: number, fovRadians: number, worldUnitsPerPixel: number): number {
  return distance > 0 ? viewportHeight * worldUnitsPerPixel / (2 * Math.tan(fovRadians / 2) * distance) : 0;
}

export function anchorOccluded(anchorDistance: number, firstHitDistance: number | null): boolean {
  return firstHitDistance !== null && firstHitDistance >= 0 && firstHitDistance < anchorDistance - 1e-6;
}
