// @vitest-environment happy-dom
// The designer preview must say why it stopped updating instead of swallowing the validation error.
import { afterEach, describe, expect, it } from 'vitest';
import type { HostdBridge } from '../../bridge';
import { designerPanel } from './index';

function mount(): { root: HTMLElement; unmount: () => void } {
  const root = document.createElement('div');
  document.body.append(root);
  const unmount = designerPanel.mount(root, { bridge: {} as HostdBridge });
  return { root, unmount: typeof unmount === 'function' ? unmount : () => {} };
}

function type(root: HTMLElement, label: string, value: string): void {
  const input = [...root.querySelectorAll('label.designer-field')]
    .find(el => el.querySelector('span')?.textContent === label)?.querySelector('input');
  if (!input) throw new Error(`no field "${label}"`);
  input.value = value;
  input.dispatchEvent(new Event('input'));
}

afterEach(() => { document.body.replaceChildren(); });

describe('designer preview validation', () => {
  it('reports the validation error in the status line when the preview cannot update', () => {
    const { root, unmount } = mount();
    const status = root.querySelector<HTMLElement>('.designer-status')!;
    const stage = root.querySelector('.designer-stage')!;
    expect(stage).not.toBeNull();

    type(root, 'Panel title', '');
    expect(status.dataset.error).toBe('true');

    // Selecting a node only redraws the outline and preview, so the stage is the sole reporter here.
    status.textContent = '';
    status.dataset.error = 'false';
    root.querySelector<HTMLButtonElement>('.designer-tree button')!.click();
    expect(status.textContent).toBe('Preview not updated: Panel title is required');
    expect(status.dataset.error).toBe('true');
    // The last good preview stays in place.
    expect(root.querySelector('.designer-stage')).toBe(stage);
    unmount();
  });

  it('stays quiet when the design is valid', () => {
    const { root, unmount } = mount();
    const status = root.querySelector<HTMLElement>('.designer-status')!;
    status.textContent = 'sentinel';
    root.querySelector<HTMLButtonElement>('.designer-tree button')!.click();
    expect(status.textContent).toBe('sentinel');
    unmount();
  });
});
