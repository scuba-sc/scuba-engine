import type { HostdBridge } from '../../bridge';
import type { JsonValue } from '../../protocol';
import type { Vec3 } from '../../math/projection';
import { toDom } from '../widgets/dom';
import { eventToJson, isWidgetPanel, type WidgetPanel, type WidgetsListResult } from '../widgets/types';
import { renderPanelView } from '../widgets/view';
import { element } from './surface';
import '../widgets/widgets.css';

export interface MockCamera {
  readonly pos: Vec3;
  readonly rot: readonly [number, number, number, number];
}

export function readMockCamera(state: JsonValue): MockCamera | null {
  if (!state || typeof state !== 'object' || Array.isArray(state)) return null;
  const camera = state.camera;
  if (!camera || typeof camera !== 'object' || Array.isArray(camera)) return null;
  const pos = camera.pos, rot = camera.rot;
  if (!Array.isArray(pos) || !Array.isArray(rot) || pos.length !== 3 || rot.length !== 4) return null;
  const [x, y, z] = pos, [qx, qy, qz, qw] = rot;
  if (typeof x !== 'number' || typeof y !== 'number' || typeof z !== 'number' ||
      typeof qx !== 'number' || typeof qy !== 'number' || typeof qz !== 'number' || typeof qw !== 'number' ||
      ![x, y, z, qx, qy, qz, qw].every(Number.isFinite)) return null;
  const origin = [0, 0, 0];
  const player = state.player;
  if (player && typeof player === 'object' && !Array.isArray(player) && Array.isArray(state.zones)) {
    let zone = player.zone;
    const seen = new Set<JsonValue>();
    while (typeof zone === 'number' && zone !== 0) {
      if (seen.has(zone)) return null;
      seen.add(zone);
      const frame = state.zones.find(entry => entry && typeof entry === 'object' &&
        !Array.isArray(entry) && entry.id === zone);
      if (!frame || typeof frame !== 'object' || Array.isArray(frame) || !Array.isArray(frame.origin)) return null;
      const [ox, oy, oz] = frame.origin;
      if (typeof ox !== 'number' || typeof oy !== 'number' || typeof oz !== 'number' ||
          ![ox, oy, oz].every(Number.isFinite)) return null;
      origin[0] += ox; origin[1] += oy; origin[2] += oz;
      zone = frame.parent;
    }
  }
  return { pos: [x - origin[0], z - origin[2], -(y - origin[1])], rot: [qx, qy, qz, qw] };
}

export function connectStage(bridge: HostdBridge, onCamera: (pose: MockCamera | null) => void) {
  let active = true;
  let follow = false;
  let currentCamera: MockCamera | null = null;
  const panels = new Map<string, WidgetPanel>();
  const controls = element('div', undefined, 'vp-control');
  const label = element('label', 'Plugin surface');
  const picker = element('select');
  picker.setAttribute('aria-label', 'Plugin surface');
  const cameraLabel = element('label', undefined, 'vp-occlusion');
  const cameraToggle = element('input');
  cameraToggle.type = 'checkbox';
  cameraLabel.append(cameraToggle, element('span', 'Follow mock camera'));
  const status = element('span', 'Connecting to hostd');
  status.setAttribute('role', 'status');
  label.append(picker);
  controls.append(label, cameraLabel, status);
  const surface = element('div', undefined, 'vp-plugin-surface');
  function show(): void {
    const panel = panels.get(picker.value);
    if (!panel) { surface.textContent = 'No plugin panels. Build the APP plugins or select a plugin root.'; return; }
    surface.replaceChildren(toDom(renderPanelView(panel), document, event => {
      void bridge.call<{ result: string }>('widgets.event', eventToJson(event)).then(result => {
        if (active) status.textContent = result.result;
      }).catch((error: unknown) => {
        if (active) status.textContent = error instanceof Error ? error.message : String(error);
      });
    }));
  }
  function choices(): void {
    const previous = picker.value;
    picker.replaceChildren(...[...panels.values()].map(panel => {
      const option = element('option', panel.title);
      option.value = panel.id;
      return option;
    }));
    if (panels.has(previous)) picker.value = previous;
    show();
  }
  async function camera(): Promise<void> {
    try {
      const state = await bridge.call<JsonValue>('mock_game.state');
      if (!active) return;
      currentCamera = readMockCamera(state);
      if (follow) onCamera(currentCamera);
    } catch (error: unknown) {
      if (active) {
        currentCamera = null;
        onCamera(null);
        status.textContent = error instanceof Error ? error.message : String(error);
      }
    }
  }
  async function load(): Promise<void> {
    try {
      const result = await bridge.call<WidgetsListResult>('widgets.list');
      if (!active) return;
      panels.clear();
      for (const panel of result.panels) panels.set(panel.id, panel);
      choices();
      status.textContent = `${panels.size} plugin panels / synthetic only`;
      await camera();
    } catch (error: unknown) {
      if (active) status.textContent = error instanceof Error ? error.message : String(error);
    }
  }
  picker.addEventListener('change', show);
  cameraToggle.addEventListener('change', () => {
    follow = cameraToggle.checked;
    onCamera(follow ? currentCamera : null);
    if (follow) void camera();
  });
  const offSnapshot = bridge.on('widgets.snapshot', data => {
    if (!isWidgetPanel(data)) return;
    const known = panels.has(data.id);
    panels.set(data.id, data);
    if (!known) choices();
    else if (picker.value === data.id) show();
  });
  const offRemoved = bridge.on('widgets.removed', data => {
    if (data && typeof data === 'object' && !Array.isArray(data) && typeof data.id === 'string') {
      panels.delete(data.id); choices();
    }
  });
  const offCamera = bridge.on('mock_game.changed', () => { void camera(); });
  const offState = bridge.onStateChange(state => {
    if (state === 'open') void load();
    else {
      status.textContent = `Hostd ${state}`;
      currentCamera = null;
      onCamera(null);
    }
  });
  return {
    controls, surface: { element: surface, width: 480, height: 560 },
    cleanup() { active = false; offSnapshot(); offRemoved(); offCamera(); offState(); },
  };
}
