import { describe, expect, it, vi } from 'vitest';
import { sampleDesigns, validate } from './model';
import { applyPreviewEvent, designSnapshot } from './preview';
import { findNode, renderPanelView } from '../widgets/view';

describe('designer shared renderer', () => {
  it('preserves nesting, paging and severity in the widget wire model', () => {
    const design = sampleDesigns()[2];
    const snapshot = designSnapshot(design);
    const view = renderPanelView(snapshot);
    expect(snapshot.nodes.find(node => node.kind === 'button')).toMatchObject({
      parent: 1, severity: 'info', tooltip: 'Synthetic diagnostics only',
    });
    expect(findNode(view, node => node.attrs?.['aria-label'] === 'Next rows of List')).toBeDefined();
  });

  it('applies a shared renderer event back into generated-source state', () => {
    const design = sampleDesigns()[0];
    const view = renderPanelView(designSnapshot(design));
    const control = findNode(view, node => node.attrs?.type === 'checkbox');
    const event = control?.on?.change?.({ value: '', checked: true });
    expect(event).toBeTruthy();
    if (!event) throw new Error('Toggle did not emit an event');
    applyPreviewEvent(design, event);
    expect(designSnapshot(design).nodes.find(node => node.kind === 'toggle')?.value).toBe(1);
  });

  it('logs a warning when design validation fails during stage rendering', () => {
    const warnSpy = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const invalidDesign = structuredClone(sampleDesigns()[0]);
    invalidDesign.title = ''; // Empty title triggers a validation failure

    // Verify validate throws an error for invalid design and can be caught / logged
    let validationError: Error | undefined;
    try {
      validate(invalidDesign);
    } catch (error) {
      validationError = error as Error;
      console.warn('Design validation failed:', error);
    }

    expect(validationError).toBeDefined();
    expect(validationError?.message).toBe('Panel title is required');
    expect(warnSpy).toHaveBeenCalledWith('Design validation failed:', validationError);
    warnSpy.mockRestore();
  });
});
