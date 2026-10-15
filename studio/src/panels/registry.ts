// The studio's tab/panel registry. Each feature exports a PanelModule from its own file under
// src/panels/ and is registered with one line in src/panels/index.ts:
//
//   registerPanel(widgetDesignerPanel);
import type { HostdBridge } from '../bridge';

export interface PanelContext {
  bridge: HostdBridge;
}

export interface PanelModule {
  id: string;
  title: string;
  order?: number;
  // Renders into container; the returned function (if any) runs when the panel is hidden.
  mount(container: HTMLElement, ctx: PanelContext): void | (() => void);
}

const panels = new Map<string, PanelModule>();

export function registerPanel(panel: PanelModule): void {
  if (!/^[a-z][a-z0-9-]*$/.test(panel.id)) throw new Error(`bad panel id "${panel.id}"`);
  if (panels.has(panel.id)) throw new Error(`panel "${panel.id}" registered twice`);
  panels.set(panel.id, panel);
}

export function listPanels(): PanelModule[] {
  return [...panels.values()].sort((a, b) => (a.order ?? 100) - (b.order ?? 100) || a.id.localeCompare(b.id));
}

export function getPanel(id: string): PanelModule | undefined {
  return panels.get(id);
}

export function clearPanelsForTest(): void {
  panels.clear();
}
