import type { PanelModule } from '../registry';

export const viewportPanel: PanelModule = {
  id: 'viewport',
  title: 'Viewport',
  order: 10,
  mount(container, { bridge }) {
    let active = true;
    let cleanup: (() => void) | undefined;
    void import('./stage').then(({ mountDiegeticStage }) => {
      if (active) cleanup = mountDiegeticStage(container, { bridge });
    }).catch((error: unknown) => {
      if (active) container.textContent = error instanceof Error ? error.message : 'Stage failed to load.';
    });
    return () => {
      active = false;
      cleanup?.();
    };
  },
};
