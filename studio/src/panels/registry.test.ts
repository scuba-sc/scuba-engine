import { afterEach, describe, expect, it } from 'vitest';
import { clearPanelsForTest, getPanel, listPanels, registerPanel, type PanelModule } from './registry';

const panel = (id: string, order?: number): PanelModule => ({ id, title: id, order, mount: () => undefined });

describe('panel registry', () => {
  afterEach(() => clearPanelsForTest());

  it('lists panels by order, then id', () => {
    registerPanel(panel('zeta'));
    registerPanel(panel('home', 0));
    registerPanel(panel('alpha'));
    expect(listPanels().map((p) => p.id)).toEqual(['home', 'alpha', 'zeta']);
    expect(getPanel('alpha')?.title).toBe('alpha');
  });

  it('rejects duplicate and malformed ids', () => {
    registerPanel(panel('home'));
    expect(() => registerPanel(panel('home'))).toThrow(/twice/);
    expect(() => registerPanel(panel('Bad Id'))).toThrow(/bad panel id/);
  });
});
