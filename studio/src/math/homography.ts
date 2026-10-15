import type { Quad, Vec2 } from './projection';

/** Row-major 3x3 projective transform, normalized so h[8] = 1. */
export type Homography = readonly [number, number, number, number, number, number, number, number, number];

/** Solve eight linear equations with partial pivoting; singular quads have no mapping. */
export function computeHomography(source: Quad<Vec2>, target: Quad<Vec2>): Homography | null {
  const rows: number[][] = [];
  for (let i = 0; i < 4; i++) {
    const [x, y] = source[i];
    const [u, v] = target[i];
    rows.push([x, y, 1, 0, 0, 0, -u * x, -u * y, u]);
    rows.push([0, 0, 0, x, y, 1, -v * x, -v * y, v]);
  }
  for (let column = 0; column < 8; column++) {
    let pivot = column;
    for (let row = column + 1; row < 8; row++) {
      if (Math.abs(rows[row][column]) > Math.abs(rows[pivot][column])) pivot = row;
    }
    if (Math.abs(rows[pivot][column]) < 1e-12) return null;
    [rows[column], rows[pivot]] = [rows[pivot], rows[column]];
    const divisor = rows[column][column];
    for (let j = column; j <= 8; j++) rows[column][j] /= divisor;
    for (let row = 0; row < 8; row++) {
      if (row === column) continue;
      const factor = rows[row][column];
      for (let j = column; j <= 8; j++) rows[row][j] -= factor * rows[column][j];
    }
  }
  return [rows[0][8], rows[1][8], rows[2][8], rows[3][8], rows[4][8], rows[5][8], rows[6][8], rows[7][8], 1];
}

export function mapPoint(h: Homography, point: Vec2): Vec2 | null {
  const [x, y] = point;
  const w = h[6] * x + h[7] * y + h[8];
  if (Math.abs(w) < 1e-12) return null;
  return [(h[0] * x + h[1] * y + h[2]) / w, (h[3] * x + h[4] * y + h[5]) / w];
}

export function inverseHomography(h: Homography): Homography | null {
  const [a, b, c, d, e, f, g, k, i] = h;
  const cofactors: Homography = [
    e * i - f * k, c * k - b * i, b * f - c * e,
    f * g - d * i, a * i - c * g, c * d - a * f,
    d * k - e * g, b * g - a * k, a * e - b * d,
  ];
  const determinant = a * cofactors[0] + b * cofactors[3] + c * cofactors[6];
  if (Math.abs(determinant) < 1e-12) return null;
  return [
    cofactors[0] / determinant, cofactors[1] / determinant, cofactors[2] / determinant,
    cofactors[3] / determinant, cofactors[4] / determinant, cofactors[5] / determinant,
    cofactors[6] / determinant, cofactors[7] / determinant, cofactors[8] / determinant,
  ];
}

/** CSS column-major layout: (x,y,0,1) -> (u,v,0,w). No DOM dependency. */
export function homographyMatrix3d(h: Homography): readonly number[] {
  return [h[0], h[3], 0, h[6], h[1], h[4], 0, h[7], 0, 0, 1, 0, h[2], h[5], 0, h[8]];
}
