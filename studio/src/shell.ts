import type { HostdBridge } from './bridge';
import { getPanel, listPanels } from './panels/registry';

export function createShell(root: HTMLElement, bridge: HostdBridge): void {
  const header = document.createElement('header');
  const brand = document.createElement('span');
  brand.className = 'brand';
  brand.textContent = 'scuba studio';
  const tabs = document.createElement('nav');
  tabs.setAttribute('role', 'tablist');
  const state = document.createElement('span');
  state.className = 'bridge-state';
  header.append(brand, tabs, state);

  const main = document.createElement('main');
  main.setAttribute('role', 'tabpanel');
  root.replaceChildren(header, main);

  bridge.onStateChange((s) => {
    state.textContent = s;
    state.dataset.state = s;
  });

  let unmount: (() => void) | undefined;
  const show = (id: string) => {
    const panel = getPanel(id) ?? listPanels()[0];
    if (!panel) return;
    unmount?.();
    unmount = undefined;
    main.replaceChildren();
    for (const b of tabs.querySelectorAll('button')) b.setAttribute('aria-selected', String(b.dataset.panel === panel.id));
    unmount = panel.mount(main, { bridge }) ?? undefined;
    if (location.hash !== `#${panel.id}`) history.replaceState(null, '', `#${panel.id}`);
  };

  for (const panel of listPanels()) {
    const b = document.createElement('button');
    b.setAttribute('role', 'tab');
    b.dataset.panel = panel.id;
    b.textContent = panel.title;
    b.addEventListener('click', () => show(panel.id));
    tabs.append(b);
  }
  window.addEventListener('hashchange', () => show(location.hash.slice(1)));
  show(location.hash.slice(1));
}
