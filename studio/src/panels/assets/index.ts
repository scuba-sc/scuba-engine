import type { PanelModule } from '../registry';

export const assetsPanel: PanelModule = {
  id: 'assets',
  title: 'Asset Lab',
  order: 30,
  mount(container, context) {
    let closed = false;
    let cleanup: (() => void) | undefined;
    void import('./panel').then(({ mountAssetLab }) => {
      if (!closed) cleanup = mountAssetLab(container, context.bridge);
    }).catch((error: unknown) => { if (!closed) container.textContent = error instanceof Error ? error.message : String(error); });
    return () => { closed = true; cleanup?.(); };
  },
};
