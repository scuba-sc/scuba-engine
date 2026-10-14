import { BridgeError, type ConnectionState } from '../../bridge';
import type { JsonValue } from '../../protocol';
import type { PanelModule } from '../registry';
import { toDom } from './dom';
import {
  eventToJson,
  isWidgetPanel,
  type WidgetEventParams,
  type WidgetEventResult,
  type WidgetPanel,
  type WidgetsListResult,
} from './types';
import { renderPanelView } from './view';
import './widgets.css';

export const widgetsPanel: PanelModule = {
  id: 'widgets',
  title: 'Widgets',
  order: 20,
  mount(container, { bridge }) {
    const doc = container.ownerDocument;
    const root = doc.createElement('section');
    root.className = 'widgets';
    const status = doc.createElement('p');
    status.className = 'widgets-status';
    status.setAttribute('role', 'status');
    const list = doc.createElement('div');
    list.className = 'widgets-panels';
    root.append(status, list);
    container.append(root);

    const panels = new Map<string, WidgetPanel>();
    const elements = new Map<string, HTMLElement>();

    const send = (e: WidgetEventParams) => {
      bridge
        .call<WidgetEventResult>('widgets.event', eventToJson(e))
        .then((r) => {
          status.textContent = r.result === 'OK' ? '' : `${e.panel}: ${r.result}`;
        })
        .catch((err: unknown) => {
          status.textContent = `${e.panel} node ${e.node_id}: ${err instanceof BridgeError || err instanceof Error ? err.message : String(err)}`;
        });
    };

    const show = (panel: WidgetPanel) => {
      const previous = panels.get(panel.id);
      if (previous && previous.revision > panel.revision) return;
      panels.set(panel.id, panel);
      const el = toDom(renderPanelView(panel), doc, send);
      const old = elements.get(panel.id);
      if (old) old.replaceWith(el);
      else {
        const after = [...elements.keys()].sort().find((id) => id > panel.id);
        list.insertBefore(el, after ? (elements.get(after) ?? null) : null);
      }
      elements.set(panel.id, el);
      status.textContent = '';
    };

    const drop = (id: string) => {
      elements.get(id)?.remove();
      elements.delete(id);
      panels.delete(id);
      if (!panels.size) status.textContent = 'No plugin panels.';
    };

    const load = async () => {
      try {
        const r = await bridge.call<WidgetsListResult>('widgets.list');
        for (const id of [...elements.keys()]) if (!r.panels.some((p) => p.id === id)) drop(id);
        for (const p of r.panels) show(p);
        status.textContent = !r.started ? 'sco.widgets is not running.' : r.panels.length ? '' : 'No plugin panels.';
      } catch (err) {
        status.textContent = `widgets.list failed: ${err instanceof Error ? err.message : String(err)}`;
      }
    };

    const offSnapshot = bridge.on('widgets.snapshot', (data: JsonValue) => {
      if (isWidgetPanel(data)) show(data);
    });
    const offRemoved = bridge.on('widgets.removed', (data: JsonValue) => {
      if (data && typeof data === 'object' && !Array.isArray(data) && typeof data.id === 'string') drop(data.id);
    });
    const offState = bridge.onStateChange((state: ConnectionState) => {
      if (state === 'open') void load();
    });
    if (bridge.state === 'open') void load();

    return () => {
      offSnapshot();
      offRemoved();
      offState();
    };
  },
};
