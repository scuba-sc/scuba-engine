import { describe, expect, it } from 'vitest';
import { readMockCamera } from './connection';

describe('mock camera adapter', () => {
  it('maps SDK z-up positions into the viewport y-up frame', () => {
    const pose = readMockCamera({ camera: { pos: [2, -5, 1.7], rot: [0, 0, 0, 1] } });
    expect(pose).toEqual({ pos: [2, 1.7, 5], rot: [0, 0, 0, 1] });
  });

  it('refuses incomplete or nonfinite boundary data', () => {
    expect(readMockCamera({ camera: { pos: [0, 1], rot: [0, 0, 0, 1] } })).toBeNull();
    expect(readMockCamera({ camera: { pos: [0, 1, Infinity], rot: [0, 0, 0, 1] } })).toBeNull();
  });

  it('rebases the world camera to the player zone before projection', () => {
    const pose = readMockCamera({
      camera: { pos: [1002, -5, 9.7], rot: [0, 0, 0, 1] },
      player: { zone: 4 },
      zones: [{ id: 4, parent: 1, origin: [2, 0, 8] }, { id: 1, parent: 0, origin: [1000, 0, 0] }],
    });
    expect(pose?.pos[0]).toBe(0);
    expect(pose?.pos[1]).toBeCloseTo(1.7);
    expect(pose?.pos[2]).toBe(5);
  });
});
