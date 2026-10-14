import type { ConnectionState } from '../bridge';
import type { JsonValue, SystemStatus } from '../protocol';
import type { PanelModule } from './registry';

function el<K extends keyof HTMLElementTagNameMap>(tag: K, text?: string, className?: string): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}

export const homePanel: PanelModule = {
  id: 'home',
  title: 'Home',
  order: 0,
  mount(container, { bridge }) {
    const root = el('section', undefined, 'home');
    root.append(el('h2', 'scuba-hostd'));
    const connection = el('p', undefined, 'connection');
    const details = el('dl');
    const plugins = el('ul', undefined, 'plugins');
    const refresh = el('button', 'Refresh');
    const logTitle = el('h3', 'Kernel log');
    const log = el('pre', '', 'log');
    root.append(connection, refresh, details, el('h3', 'Plugins'), plugins, logTitle, log);
    container.append(root);

    const row = (k: string, v: string) => {
      details.append(el('dt', k), el('dd', v));
    };

    const load = async () => {
      try {
        const s = await bridge.call<SystemStatus>('system.status');
        details.replaceChildren();
        row('Version', `${s.name} ${s.version}`);
        row('sco-core SDK', s.sdk_version);
        row('Kernel', s.kernel.started ? 'started' : 'not started');
        row('Plugin root', s.kernel.plugin_root || '(none)');
        row('Clients', String(s.clients));
        row('Uptime', `${Math.round(s.uptime_ms / 1000)} s`);
        plugins.replaceChildren(
          ...(s.kernel.plugins.length
            ? s.kernel.plugins.map((p) => el('li', `${p.folder}: ${p.state}${p.reason ? ` (${p.reason})` : ''}`))
            : [el('li', 'no plugins loaded')]),
        );
      } catch (err) {
        details.replaceChildren(el('dt', 'Error'), el('dd', err instanceof Error ? err.message : String(err)));
      }
    };

    const offState = bridge.onStateChange((state: ConnectionState) => {
      connection.textContent = `Bridge: ${state}`;
      connection.dataset.state = state;
      if (state === 'open') void load();
    });
    const offLog = bridge.on('kernel.log', (data: JsonValue) => {
      if (data && typeof data === 'object' && !Array.isArray(data) && typeof data.line === 'string') {
        log.textContent = `${log.textContent ?? ''}${data.line}\n`.split('\n').slice(-200).join('\n');
      }
    });
    refresh.addEventListener('click', () => void load());

    return () => {
      offState();
      offLog();
    };
  },
};
