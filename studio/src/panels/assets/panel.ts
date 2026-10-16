import type { HostdBridge } from '../../bridge';
import { decodeGlb } from './loader';
import { createPreview } from './preview';
import './style.css';

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}
function record(value: unknown): Record<string, unknown> {
  if (!isObject(value)) throw new Error('Invalid response from asset service');
  return value;
}
function text(value: unknown): string {
  if (typeof value !== 'string') throw new Error('Expected text from asset service');
  return value;
}
function list(value: unknown): unknown[] {
  if (!Array.isArray(value)) throw new Error('Expected list from asset service');
  return value;
}
function node<K extends keyof HTMLElementTagNameMap>(tag: K, content: string): HTMLElementTagNameMap[K] {
  const element = document.createElement(tag);
  element.textContent = content;
  return element;
}

type Child = Node | string;
// Builds static markup without an HTML parser: attribute values and children never become markup.
function el<K extends keyof HTMLElementTagNameMap>(tag: K, attrs: Record<string, string> = {}, ...children: Child[]): HTMLElementTagNameMap[K] {
  const element = document.createElement(tag);
  for (const [name, value] of Object.entries(attrs)) element.setAttribute(name, value);
  element.append(...children);
  return element;
}
const br = () => document.createElement('br');
const title = (heading: string, extra: Node) => el('div', { class: 'asset-section-title' }, el('h2', {}, heading), extra);
const field = (name: string, extra: Record<string, string> = {}) => el('input', { name, spellcheck: 'false', ...extra });

export function mountAssetLab(container: HTMLElement, bridge: HostdBridge): () => void {
  const root = document.createElement('section');
  root.className = 'asset-lab';
  root.append(
    el('div', { class: 'asset-heading' },
      el('div', {}, el('span', { class: 'asset-eyebrow' }, 'LOCAL AUTHORING'), el('h1', {}, 'Asset Lab')),
      el('p', {}, 'Your install. Your files.', br(), 'No game data is included.')),
    el('form', { class: 'asset-source' },
      el('label', {}, 'DataCore or override pack', field('database', { placeholder: 'Path to your Game2.dcb or overrides.toml', required: '' })),
      el('button', { type: 'submit', class: 'asset-primary' }, 'Open database')),
    el('p', { class: 'asset-status', role: 'status', 'aria-live': 'polite' }, 'Open a local database to browse records, structs and GUID references.'),
    el('div', { class: 'asset-workspace' },
      el('section', { class: 'asset-tree' },
        title('Record tree', el('button', { type: 'button', 'data-action': 'structs' }, 'Structs')),
        el('form', { class: 'asset-search' },
          el('label', {}, 'Filter name, type or GUID', field('filter', { placeholder: 'Search records' })),
          el('button', {}, 'Find')),
        el('div', { class: 'asset-records' }, el('p', { class: 'asset-empty' }, 'No database open')),
        el('div', { class: 'asset-pagination' },
          el('button', { type: 'button', 'data-action': 'previous' }, 'Previous'),
          el('span', { 'data-count': '' }, '0 records'),
          el('button', { type: 'button', 'data-action': 'next' }, 'Next'))),
      el('section', { class: 'asset-inspector' },
        title('Field inspector', el('span', { 'data-selection': '' }, 'Nothing selected')),
        el('div', { class: 'asset-fields' },
          el('p', { class: 'asset-empty' }, 'Select a record to inspect its typed fields.', br(), 'GUID references link to their target records.')),
        el('div', { class: 'asset-pack' },
          el('label', {}, 'Overrides (TOML)',
            el('textarea', { name: 'pack', spellcheck: 'false', 'aria-describedby': 'asset-pack-help' }, 'format = 1\natomic = true\n')),
          el('p', { id: 'asset-pack-help' }, 'Stage a field, edit its value, then check the pack. A loaded DCB also validates field types and record targets.'),
          el('div', { class: 'asset-actions' },
            el('button', { type: 'button', 'data-action': 'check' }, 'Check pack'),
            el('button', { type: 'button', 'data-action': 'export' }, 'Download TOML')))),
      el('section', { class: 'asset-preview-section' },
        title('Mesh preview', el('span', {}, 'glTF 2.0')),
        el('form', { class: 'asset-model' },
          el('label', {}, 'Mesh or texture path', field('model', { placeholder: 'Path to .cgf, .cga, .dds, .glb, .gltf or .obj', required: '' })),
          el('button', {}, 'Load preview')),
        el('div', { class: 'asset-canvas' }, el('p', { class: 'asset-empty' }, 'Local geometry and textures', br(), 'Preview without launching the game')),
        el('p', { class: 'asset-model-info', role: 'status' }, 'Static geometry | BC1-BC7 DDS | Orbit controls'),
        el('button', { type: 'button', 'data-action': 'glb', disabled: '' }, 'Download GLB'),
        el('details', { class: 'asset-import' },
          el('summary', {}, 'Import a custom model as a pack'),
          el('form', {},
            el('label', {}, 'New pack folder', field('destination', { placeholder: 'A new local folder', required: '' })),
            el('label', {}, 'VFS path inside files/', field('virtual', { value: 'objects/custom/model.glb', required: '' })),
            el('p', {}, 'Imports glTF/OBJ plus the checked TOML into files/ and datacore/. GLB is an authoring asset, not a game-native CGF replacement.'),
            el('button', {}, 'Create pack'))))));
  container.append(root);
  const required = <T extends HTMLElement>(selector: string): T => {
    const element = root.querySelector<T>(selector);
    if (!element) throw new Error(`Missing Asset Lab element: ${selector}`);
    return element;
  };
  const input = (name: string) => required<HTMLInputElement>(`input[name="${name}"]`);
  const pack = required<HTMLTextAreaElement>('textarea');
  const status = required<HTMLElement>('.asset-status');
  const records = required<HTMLElement>('.asset-records');
  const fields = required<HTMLElement>('.asset-fields');
  const selection = required<HTMLElement>('[data-selection]');
  const count = required<HTMLElement>('[data-count]');
  const canvas = required<HTMLElement>('.asset-canvas');
  const modelInfo = required<HTMLElement>('.asset-model-info');
  let preview: ReturnType<typeof createPreview> | undefined;
  let closed = false, busy = false, offset = 0, total = 0, kind = '', selected = '';
  let lastGlb: ArrayBuffer | undefined;
  const buttons = () => {
    for (const button of root.querySelectorAll<HTMLButtonElement>('button')) button.disabled = busy;
    required<HTMLButtonElement>('[data-action="previous"]').disabled = busy || offset === 0;
    required<HTMLButtonElement>('[data-action="next"]').disabled = busy || offset + 100 >= total;
    required<HTMLButtonElement>('[data-action="glb"]').disabled = busy || !lastGlb;
  };
  const run = (action: () => Promise<void>) => {
    if (busy || closed) return;
    busy = true;
    status.textContent = 'Working with local files...';
    status.dataset.error = 'false';
    buttons();
    void action().catch((error: unknown) => {
      if (closed) return;
      status.textContent = error instanceof Error ? error.message : String(error);
      status.dataset.error = 'true';
    }).finally(() => { busy = false; if (!closed) buttons(); });
  };
  const download = (data: BlobPart, name: string, type: string) => {
    const url = URL.createObjectURL(new Blob([data], { type }));
    const link = document.createElement('a');
    link.href = url; link.download = name; link.click();
    URL.revokeObjectURL(url);
  };
  const inspect = async (name: string) => {
    const response = list(await bridge.call<unknown>('assets.fields', { record: name }));
    if (closed) return;
    selected = name;
    selection.textContent = name;
    fields.replaceChildren();
    if (!response.length) fields.append(node('p', 'This record has no readable fields.'));
    for (const raw of response) {
      const field = record(raw), path = text(field['path']), value = text(field['value']);
      const row = document.createElement('div');
      row.className = 'asset-field';
      const label = document.createElement('div');
      label.append(node('strong', path), node('small', text(field['type'])));
      row.append(label, node('code', value));
      if (field['reference']) {
        const target = record(field['reference']);
        const jump = node('button', `Open ${text(target['name'])}`);
        jump.type = 'button';
        jump.onclick = () => run(async () => { await inspect(text(target['guid'])); status.textContent = 'Reference target opened.'; });
        row.append(jump);
      }
      if (!field['array'] && !['strong', 'weak'].includes(text(field['type']))) {
        const stage = node('button', 'Stage field');
        stage.type = 'button';
        stage.onclick = () => {
          pack.value += `\n[[set]]\n${selected.startsWith('guid:') || /^[0-9a-f-]{36}$/i.test(selected) ? 'guid' : 'record'} = ${JSON.stringify(selected.replace(/^guid:/, ''))}\nfield = ${JSON.stringify(path)}\nvalue = ${value}\n`;
          status.textContent = `Staged ${path}. Edit the TOML value and check the pack.`;
        };
        row.append(stage);
      }
      fields.append(row);
    }
  };
  const refresh = async () => {
    const response = record(await bridge.call<unknown>('assets.records', { offset, limit: 100, filter: input('filter').value }));
    const items = list(response['items']);
    if (typeof response['total'] !== 'number') throw new Error('Invalid record count');
    if (closed) return;
    total = response['total'];
    count.textContent = `${Math.min(offset + 1, total)}-${Math.min(offset + items.length, total)} / ${total}`;
    records.replaceChildren();
    const groups = new Map<string, HTMLElement>();
    for (const raw of items) {
      const item = record(raw), name = text(item['name']), type = text(item['struct']) || 'Pack operations';
      let group = groups.get(type);
      if (!group) {
        const branch = document.createElement('details');
        branch.open = true;
        branch.append(node('summary', type));
        group = document.createElement('div');
        branch.append(group); records.append(branch); groups.set(type, group);
      }
      const button = node('button', name || '(instance operation)');
      button.type = 'button';
      const guid = text(item['guid']);
      if (guid) button.append(node('small', guid));
      button.onclick = () => run(async () => {
        if (kind === 'toml') {
          fields.replaceChildren(node('p', `Pack operation at line ${String(item['line'])}: ${String(item['field'] ?? '')}. Open the base .dcb to resolve its fields.`));
          selection.textContent = name;
        } else await inspect(guid || name);
        status.textContent = kind === 'toml' ? 'TOML operations are shown in the pack editor.' : 'Record fields loaded. Source database is unchanged.';
      });
      group.append(button);
    }
    if (!items.length) records.append(node('p', 'No matching records.'));
  };
  required<HTMLFormElement>('.asset-source').onsubmit = (event) => {
    event.preventDefault();
    run(async () => {
      const response = record(await bridge.call<unknown>('assets.open', { path: input('database').value }));
      if (closed) return;
      kind = text(response['kind']); pack.value = text(response['pack']); offset = 0;
      selected = ''; selection.textContent = 'Nothing selected';
      fields.replaceChildren(node('p', 'Select a record from the tree.'));
      await refresh();
      status.textContent = `${total} ${kind === 'dcb' ? 'records' : 'pack operations'} loaded. Source files are read-only.`;
    });
  };
  required<HTMLFormElement>('.asset-search').onsubmit = (event) => {
    event.preventDefault();
    run(async () => { offset = 0; await refresh(); status.textContent = `${total} matches.`; });
  };
  required<HTMLButtonElement>('[data-action="previous"]').onclick = () => run(async () => {
    offset = Math.max(offset - 100, 0); await refresh(); status.textContent = 'Previous records loaded.';
  });
  required<HTMLButtonElement>('[data-action="next"]').onclick = () => run(async () => {
    offset += 100; await refresh(); status.textContent = 'Next records loaded.';
  });
  required<HTMLButtonElement>('[data-action="structs"]').onclick = () => run(async () => {
    const structs = list(await bridge.call<unknown>('assets.structs'));
    if (closed) return;
    fields.replaceChildren();
    selection.textContent = 'Struct definitions';
    for (const raw of structs) {
      const definition = record(raw), branch = document.createElement('details');
      branch.append(node('summary', text(definition['name'])));
      if (definition['properties']) for (const property of list(definition['properties'])) {
        const entry = record(property);
        branch.append(node('p', `${text(entry['name'])}: ${text(entry['type'])} (conversion ${String(entry['conversion'])})`));
      }
      fields.append(branch);
    }
    status.textContent = `${structs.length} struct definitions.`;
  });
  const check = async () => {
    const response = record(await bridge.call<unknown>('assets.pack', { text: pack.value }));
    if (!closed) pack.value = text(response['text']);
  };
  required<HTMLButtonElement>('[data-action="check"]').onclick = () => run(async () => {
    await check(); status.textContent = kind === 'dcb' ? 'Pack syntax and all overrides validated against the database.' : 'Pack syntax valid. Open a DCB for semantic validation.';
  });
  required<HTMLButtonElement>('[data-action="export"]').onclick = () => run(async () => {
    await check();
    const result = record(await bridge.call<unknown>('assets.export'));
    if (closed) return;
    download(text(result['text']), 'overrides.toml', 'application/toml');
    status.textContent = 'Validated override pack downloaded.';
  });
  required<HTMLFormElement>('.asset-model').onsubmit = (event) => {
    event.preventDefault();
    run(async () => {
      const result = record(await bridge.call<unknown>('assets.convert', { path: input('model').value }));
      if (closed) return;
      lastGlb = decodeGlb(text(result['glb']));
      try {
        if (!preview) { preview = createPreview(canvas); canvas.querySelector('.asset-empty')?.remove(); }
        modelInfo.textContent = await preview.load(lastGlb);
      } catch (error: unknown) {
        modelInfo.textContent = error instanceof Error ? error.message : String(error);
        throw error;
      }
      status.textContent = `GLB ready (${Number(result['bytes']).toLocaleString()} bytes). Source file is unchanged.`;
    });
  };
  required<HTMLButtonElement>('[data-action="glb"]').onclick = () => {
    if (lastGlb) download(lastGlb, 'asset.glb', 'model/gltf-binary');
  };
  required<HTMLFormElement>('.asset-import form').onsubmit = (event) => {
    event.preventDefault();
    run(async () => {
      await check();
      await bridge.call('assets.import', {
        path: input('model').value, destination: input('destination').value, virtualPath: input('virtual').value,
      });
      status.textContent = 'Pack created with files/ geometry and datacore/ overrides. Existing files were not overwritten.';
    });
  };
  buttons();
  return () => { closed = true; preview?.dispose(); root.remove(); };
}
