import { element } from './surface';
import { terminalPresets } from './presets';

export type ProjectionMode = 'billboard' | 'quad';
export interface StageState {
  preset: number;
  mode: ProjectionMode;
  yaw: number;
  pitch: number;
  distance: number;
  fov: number;
  occlusion: boolean;
}

export function createControls(state: StageState, onChange: (presetChanged: boolean) => void): HTMLElement {
  const controls = element('div', undefined, 'vp-controls');
  const label = element('label', 'Environment', 'vp-control');
  const select = element('select');
  for (const preset of terminalPresets) select.append(element('option', preset.title));
  select.selectedIndex = state.preset;
  select.addEventListener('change', () => { state.preset = select.selectedIndex; onChange(true); });
  label.append(select);
  const modes = element('div', undefined, 'vp-modes');
  modes.setAttribute('aria-label', 'Projection mode');
  const modeButtons = (['billboard', 'quad'] as const).map((mode) => {
    const button = element('button', mode === 'billboard' ? 'A / Billboard' : 'B / Surface quad');
    button.type = 'button';
    button.dataset.mode = mode;
    button.setAttribute('aria-pressed', String(mode === state.mode));
    button.addEventListener('click', () => {
      state.mode = mode;
      for (const b of modeButtons) b.setAttribute('aria-pressed', String(b.dataset.mode === mode));
      onChange(false);
    });
    return button;
  });
  modes.append(...modeButtons);
  controls.append(label, modes);
  for (const spec of [
    { key: 'yaw', title: 'Yaw', min: -75, max: 75, step: 1, unit: 'deg' },
    { key: 'pitch', title: 'Pitch', min: -35, max: 45, step: 1, unit: 'deg' },
    { key: 'distance', title: 'Distance', min: 3, max: 10, step: 0.1, unit: 'm' },
    { key: 'fov', title: 'FOV', min: 30, max: 85, step: 1, unit: 'deg' },
  ] as const) {
    const control = element('label', spec.title, 'vp-control');
    const input = element('input');
    input.type = 'range';
    input.min = String(spec.min); input.max = String(spec.max); input.step = String(spec.step);
    input.value = String(state[spec.key]);
    input.setAttribute('aria-label', spec.title);
    const value = element('output', `${input.value} ${spec.unit}`);
    input.addEventListener('input', () => {
      state[spec.key] = input.valueAsNumber;
      value.textContent = `${input.value} ${spec.unit}`;
      onChange(false);
    });
    control.append(input, value);
    controls.append(control);
  }
  const occlusion = element('label', undefined, 'vp-occlusion');
  const check = element('input');
  check.type = 'checkbox';
  check.addEventListener('change', () => { state.occlusion = check.checked; onChange(false); });
  occlusion.append(check, element('span', 'Simulate occlusion'));
  controls.append(occlusion);
  return controls;
}
