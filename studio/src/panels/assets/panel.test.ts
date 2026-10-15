// @vitest-environment happy-dom
import { describe, expect, it, vi } from 'vitest';
import type { HostdBridge } from '../../bridge';
import { mountAssetLab } from './panel';

function mockBridge(handlers: Record<string, (params: unknown) => unknown>): HostdBridge {
  return {
    call: vi.fn(async (method: string, params?: unknown) => {
      if (handlers[method]) return handlers[method](params);
      throw new Error(`Unhandled bridge call: ${method}`);
    }),
    subscribe: vi.fn(() => () => undefined),
    close: vi.fn(),
  } as unknown as HostdBridge;
}

describe('mountAssetLab panel', () => {
  it('mounts the asset lab HTML and initializes default element state', () => {
    const container = document.createElement('div');
    const bridge = mockBridge({});

    const unmount = mountAssetLab(container, bridge);

    const root = container.querySelector('.asset-lab');
    expect(root).not.toBeNull();
    expect(container.querySelector('h1')?.textContent).toBe('Asset Lab');
    expect(container.querySelector('.asset-status')?.textContent).toContain('Open a local database');

    const prevButton = container.querySelector<HTMLButtonElement>('[data-action="previous"]');
    const nextButton = container.querySelector<HTMLButtonElement>('[data-action="next"]');
    const glbButton = container.querySelector<HTMLButtonElement>('[data-action="glb"]');

    expect(prevButton?.disabled).toBe(true);
    expect(nextButton?.disabled).toBe(true);
    expect(glbButton?.disabled).toBe(true);

    unmount();
    expect(container.querySelector('.asset-lab')).toBeNull();
  });

  it('handles opening a database and populating records', async () => {
    const container = document.createElement('div');
    const bridge = mockBridge({
      'assets.open': () => ({ kind: 'dcb', pack: 'format = 1\n' }),
      'assets.records': () => ({
        total: 1,
        items: [{ name: 'TestRecord', struct: 'ItemStruct', guid: '1234' }],
      }),
    });

    const unmount = mountAssetLab(container, bridge);

    const sourceForm = container.querySelector<HTMLFormElement>('.asset-source')!;
    const dbInput = container.querySelector<HTMLInputElement>('input[name="database"]')!;
    dbInput.value = 'Game2.dcb';

    sourceForm.dispatchEvent(new Event('submit', { cancelable: true, bubbles: true }));

    await new Promise((resolve) => setTimeout(resolve, 10));

    expect(bridge.call).toHaveBeenCalledWith('assets.open', { path: 'Game2.dcb' });
    expect(bridge.call).toHaveBeenCalledWith('assets.records', { offset: 0, limit: 100, filter: '' });

    expect(container.querySelector('.asset-records')?.textContent).toContain('TestRecord');
    expect(container.querySelector('[data-count]')?.textContent).toBe('1-1 / 1');

    unmount();
  });

  it('inspects record fields when a record is selected', async () => {
    const container = document.createElement('div');
    const bridge = mockBridge({
      'assets.open': () => ({ kind: 'dcb', pack: '' }),
      'assets.records': () => ({
        total: 1,
        items: [{ name: 'TestRecord', struct: 'ItemStruct', guid: '1234' }],
      }),
      'assets.fields': () => [
        { path: 'health', value: '100', type: 'uint32' },
      ],
    });

    const unmount = mountAssetLab(container, bridge);

    const sourceForm = container.querySelector<HTMLFormElement>('.asset-source')!;
    container.querySelector<HTMLInputElement>('input[name="database"]')!.value = 'Game2.dcb';
    sourceForm.dispatchEvent(new Event('submit', { cancelable: true, bubbles: true }));
    await new Promise((resolve) => setTimeout(resolve, 10));

    const recordBtn = container.querySelector<HTMLButtonElement>('.asset-records button')!;
    recordBtn.click();
    await new Promise((resolve) => setTimeout(resolve, 10));

    expect(bridge.call).toHaveBeenCalledWith('assets.fields', { record: '1234' });
    expect(container.querySelector('.asset-fields')?.textContent).toContain('health');
    expect(container.querySelector('.asset-fields')?.textContent).toContain('100');

    unmount();
  });
});
