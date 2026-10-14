import type { PanelModule } from '../registry';
import type { Sandbox, Peer, JsonObject } from './types';
import { element as el, renderWidgets } from './widgets';
import './netsim.css';

export const netSandboxPanel: PanelModule = {
  id: 'netsim',
  title: 'NetSandbox',
  order: 70,
  mount(container, { bridge }) {
    const root = el('section', undefined, 'netsim');
    const heading = el('div', undefined, 'ns-heading');
    heading.append(el('div', 'NETWORK LAB / LOCALHOST', 'ns-eyebrow'), el('h2', 'NetSandbox'),
      el('p', 'Separate kernels. Real UDP. Inspect each peer side by side.', 'ns-muted'));
    const controls = el('form', undefined, 'ns-controls');
    const countLabel = el('label', 'Peers');
    const count = el('select');
    for (let n = 2; n <= 8; ++n) {
      const option = el('option', String(n)); option.value = String(n); count.append(option);
    }
    countLabel.append(count);
    const pluginLabel = el('label', 'Plugin folder (optional)');
    const plugins = el('input');
    plugins.placeholder = 'Folder on the host machine'; pluginLabel.append(plugins);
    const sampleLabel = el('label', undefined, 'ns-check');
    const sample = el('input'); sample.type = 'checkbox'; sample.checked = true;
    sampleLabel.append(sample, el('span', 'Synthetic chat plugin'));
    const start = el('button', 'Start sandbox', 'ns-primary'); start.type = 'submit';
    const stop = el('button', 'Stop all'); stop.type = 'button'; stop.disabled = true;
    controls.append(countLabel, pluginLabel, sampleLabel, start, stop);
    const status = el('p', 'Ready to start 2-8 isolated hosts.', 'ns-status');
    status.setAttribute('role', 'status');
    const error = el('p', undefined, 'ns-error'); error.setAttribute('role', 'alert');
    const roster = el('div', undefined, 'ns-actions');
    const grid = el('div', undefined, 'ns-grid');
    root.append(heading, controls, status, error, roster, grid);
    container.append(root);
    let disposed = false, polling = false, mutating = false;
    let current: Sandbox = { peers: [] };
    let queue = Promise.resolve();
    const cards = new Map<number, { root: HTMLElement; header: HTMLElement; metrics: HTMLElement; widgets: HTMLElement; signature: string }>();

    const render = (sandbox: Sandbox) => {
      current = sandbox;
      const running = sandbox.peers.length > 0;
      count.disabled = plugins.disabled = sample.disabled = running || mutating;
      start.disabled = running || mutating;
      stop.disabled = !running || mutating;
      status.textContent = running
        ? `${sandbox.peers.length} isolated hosts / loopback UDP / host port ${sandbox.peers.find(p => p.slot === 1)?.port ?? '-'}`
        : 'No peers running. Start a sandbox to compare panels and traffic.';
      roster.replaceChildren();
      if (running) for (let slot = 2; slot <= 8; ++slot) {
        if (sandbox.peers.some(peer => peer.slot === slot)) continue;
        const join = el('button', `Join peer ${slot}`); join.disabled = mutating;
        join.onclick = () => action('netsim.join', { slot }); roster.append(join);
      }
      for (const [slot, card] of cards) if (!sandbox.peers.some(p => p.slot === slot)) { card.root.remove(); cards.delete(slot); }
      if (!running) grid.replaceChildren(el('div', 'Start with the synthetic chat plugin, or choose your own plugin folder. Each peer gets private storage and a separate kernel.', 'ns-empty'));
      else if (cards.size === 0) grid.replaceChildren();
      for (const peer of sandbox.peers) renderPeer(peer);
    };
    const renderPeer = (peer: Peer) => {
      let card = cards.get(peer.slot);
      if (!card) {
        card = { root: el('article', undefined, 'ns-peer'), header: el('div', undefined, 'ns-peer-head'),
          metrics: el('div', undefined, 'ns-metrics'), widgets: el('div', undefined, 'ns-widgets'), signature: '' };
        card.root.append(card.header, card.metrics, card.widgets);
        cards.set(peer.slot, card); grid.append(card.root);
      }
      const label = el('div');
      label.append(el('h3', `Peer ${peer.slot}`), el('span', peer.status === 'failed' ? peer.error
        : `${peer.slot === 1 ? 'SESSION HOST' : 'JOINER'} / PID ${peer.pid} / ${peer.members} members`, 'ns-muted'));
      const actions = el('div', undefined, 'ns-actions');
      const burst = el('button', 'Send 16 + 16');
      burst.title = 'Send 16 reliable and 16 unreliable synthetic messages';
      burst.disabled = !peer.probe || mutating || !peer.active;
      burst.onclick = () => action('netsim.send', { slot: peer.slot, count: 16 });
      actions.append(burst);
      if (peer.slot !== 1) {
        const leave = el('button', 'Leave'); leave.disabled = mutating;
        leave.onclick = () => action('netsim.leave', { slot: peer.slot }); actions.append(leave);
      }
      card.header.replaceChildren(label, actions);
      const traffic = peer.traffic;
      card.metrics.replaceChildren();
      if (traffic) {
        const metric = (title: string, value: string) => {
          const box = el('div'); box.append(el('span', title), el('strong', value)); card.metrics.append(box);
        };
        metric('UDP IN / OUT', `${traffic.datagramsIn} / ${traffic.datagramsOut}`);
        metric('RETRANSMITS', String(traffic.retransmits));
        metric('DROPS / QUOTA', `${traffic.sendDropped + traffic.inboxDropped} / ${traffic.quotaRefused}`);
        if (peer.probe) {
          metric('RELIABLE TX / RX', `${peer.probe.reliableSent} / ${peer.probe.reliableReceived}`);
          metric('UNRELIABLE TX / RX', `${peer.probe.unreliableSent} / ${peer.probe.unreliableReceived}`);
        }
      }
      const signature = JSON.stringify([peer.panels, peer.plugins]);
      const active = document.activeElement;
      const editing = active instanceof HTMLInputElement || active instanceof HTMLSelectElement;
      if (card.signature !== signature && !(editing && active && card.widgets.contains(active))) {
        card.signature = signature;
        card.widgets.replaceChildren(...(peer.panels ?? []).map(panel => renderWidgets(panel,
          event => action('netsim.input', { slot: peer.slot, ...event }))));
        if (!peer.panels?.length) card.widgets.append(el('p', 'No widget panels published by this peer.', 'ns-muted'));
        for (const plugin of peer.plugins ?? []) if (plugin.state !== 'loaded') {
          card.widgets.append(el('p', `${plugin.id}: ${plugin.state} - ${plugin.reason}`, 'ns-error'));
        }
      }
    };
    const action = (method: string, params?: JsonObject) => {
      queue = queue.then(async () => {
        if (disposed) return;
        mutating = true; error.textContent = ''; render(current);
        try { const result = await bridge.call<Sandbox>(method, params); if (!disposed) current = result; }
        catch (err) { if (!disposed) error.textContent = err instanceof Error ? err.message : String(err); }
        finally { mutating = false; if (!disposed) render(current); }
      });
    };
    const load = async () => {
      if (polling || disposed || mutating || bridge.state !== 'open') return;
      polling = true;
      try { const result = await bridge.call<Sandbox>('netsim.status'); if (!disposed) render(result); }
      catch (err) { if (!disposed) error.textContent = err instanceof Error ? err.message : String(err); }
      finally { polling = false; }
    };
    controls.onsubmit = event => {
      event.preventDefault();
      action('netsim.start', { count: Number(count.value), pluginRoot: plugins.value, sample: sample.checked });
    };
    stop.onclick = () => action('netsim.stop');
    const off = bridge.onStateChange(state => {
      if (state === 'open') void load();
      else status.textContent = `Bridge ${state}. Connect hostd to control the sandbox.`;
    });
    const timer = setInterval(() => void load(), 400);
    render(current); void load();
    return () => { disposed = true; clearInterval(timer); off(); root.remove(); };
  },
};
