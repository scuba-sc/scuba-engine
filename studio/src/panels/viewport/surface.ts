import type { Vec2 } from '../../math/projection';
import type { TerminalPreset } from './presets';

export interface PanelSurface {
  readonly element: HTMLElement;
  readonly width: number;
  readonly height: number;
  readonly onPointer?: (pixels: Vec2, event: PointerEvent) => void;
}

export function element<K extends keyof HTMLElementTagNameMap>(tag: K, text?: string, className?: string): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}

export function terminalSurface(preset: TerminalPreset, onAction: (action: string) => void): PanelSurface {
  const root = element('section', undefined, 'vp-terminal');
  root.setAttribute('aria-label', `${preset.title} synthetic panel`);
  const top = element('div', undefined, 'vp-terminal-top');
  top.append(element('span', preset.category), element('span', 'SIM'));
  const heading = element('h3', preset.heading);
  const summary = element('p', preset.summary, 'vp-terminal-summary');
  const meter = element('div', undefined, 'vp-meter');
  meter.append(element('span', 'LINK READY'), element('span', '100%'));
  const actions = element('div', undefined, 'vp-terminal-actions');
  for (const action of preset.actions) {
    const button = element('button', action);
    button.type = 'button';
    button.addEventListener('click', () => onAction(action));
    actions.append(button);
  }
  const status = element('p', 'Awaiting input', 'vp-terminal-status');
  root.append(top, heading, summary, meter, actions, status);
  return { element: root, width: 360, height: 420 };
}
