// @vitest-environment happy-dom
import { describe, expect, it, vi } from 'vitest';
import { mountAssetLab } from './panel';
import type { HostdBridge } from '../../bridge';

describe('mountAssetLab', () => {
  it('mounts asset lab DOM structure without innerHTML', () => {
    const container = document.createElement('div');
    const mockBridge: HostdBridge = {
      call: vi.fn().mockResolvedValue({}),
      on: vi.fn(),
    } as unknown as HostdBridge;

    const unmount = mountAssetLab(container, mockBridge);

    const root = container.querySelector('.asset-lab');
    expect(root).not.toBeNull();

    // Verify key headers, forms, inputs, buttons
    expect(container.querySelector('h1')?.textContent).toBe('Asset Lab');
    expect(container.querySelector('input[name="database"]')).not.toBeNull();
    expect(container.querySelector('button[data-action="structs"]')).not.toBeNull();
    expect(container.querySelector('textarea[name="pack"]')).not.toBeNull();
    expect(container.querySelector('input[name="model"]')).not.toBeNull();
    expect(container.querySelector('button[data-action="glb"]')).not.toBeNull();

    // Verify cleanup
    unmount();
    expect(container.querySelector('.asset-lab')).toBeNull();
  });
});
