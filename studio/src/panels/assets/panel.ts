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

export function mountAssetLab(container: HTMLElement, bridge: HostdBridge): () => void {
  const root = document.createElement('section');
  root.className = 'asset-lab';

  const heading = document.createElement('div');
  heading.className = 'asset-heading';
  const titleWrap = document.createElement('div');
  const eyebrow = node('span', 'LOCAL AUTHORING');
  eyebrow.className = 'asset-eyebrow';
  titleWrap.append(eyebrow, node('h1', 'Asset Lab'));
  const headingDesc = document.createElement('p');
  headingDesc.append('Your install. Your files.', document.createElement('br'), 'No game data is included.');
  heading.append(titleWrap, headingDesc);

  const sourceForm = document.createElement('form');
  sourceForm.className = 'asset-source';
  const sourceLabel = document.createElement('label');
  sourceLabel.append('DataCore or override pack');
  const dbInput = document.createElement('input');
  dbInput.name = 'database';
  dbInput.placeholder = 'Path to your Game2.dcb or overrides.toml';
  dbInput.spellcheck = false;
  dbInput.required = true;
  sourceLabel.append(dbInput);
  const openBtn = node('button', 'Open database');
  openBtn.type = 'submit';
  openBtn.className = 'asset-primary';
  sourceForm.append(sourceLabel, openBtn);

  const statusPara = node('p', 'Open a local database to browse records, structs and GUID references.');
  statusPara.className = 'asset-status';
  statusPara.setAttribute('role', 'status');
  statusPara.setAttribute('aria-live', 'polite');

  const workspace = document.createElement('div');
  workspace.className = 'asset-workspace';

  const treeSection = document.createElement('section');
  treeSection.className = 'asset-tree';
  const treeTitle = document.createElement('div');
  treeTitle.className = 'asset-section-title';
  const structsBtn = node('button', 'Structs');
  structsBtn.type = 'button';
  structsBtn.dataset.action = 'structs';
  treeTitle.append(node('h2', 'Record tree'), structsBtn);

  const searchForm = document.createElement('form');
  searchForm.className = 'asset-search';
  const searchLabel = document.createElement('label');
  searchLabel.append('Filter name, type or GUID');
  const filterInput = document.createElement('input');
  filterInput.name = 'filter';
  filterInput.placeholder = 'Search records';
  filterInput.spellcheck = false;
  searchLabel.append(filterInput);
  searchForm.append(searchLabel, node('button', 'Find'));

  const recordsDiv = document.createElement('div');
  recordsDiv.className = 'asset-records';
  const emptyRecords = node('p', 'No database open');
  emptyRecords.className = 'asset-empty';
  recordsDiv.append(emptyRecords);

  const paginationDiv = document.createElement('div');
  paginationDiv.className = 'asset-pagination';
  const prevBtn = node('button', 'Previous');
  prevBtn.type = 'button';
  prevBtn.dataset.action = 'previous';
  const countSpan = node('span', '0 records');
  countSpan.dataset.count = '';
  const nextBtn = node('button', 'Next');
  nextBtn.type = 'button';
  nextBtn.dataset.action = 'next';
  paginationDiv.append(prevBtn, countSpan, nextBtn);
  treeSection.append(treeTitle, searchForm, recordsDiv, paginationDiv);

  const inspectorSection = document.createElement('section');
  inspectorSection.className = 'asset-inspector';
  const inspectorTitle = document.createElement('div');
  inspectorTitle.className = 'asset-section-title';
  const selectionSpan = node('span', 'Nothing selected');
  selectionSpan.dataset.selection = '';
  inspectorTitle.append(node('h2', 'Field inspector'), selectionSpan);

  const fieldsDiv = document.createElement('div');
  fieldsDiv.className = 'asset-fields';
  const emptyFields = document.createElement('p');
  emptyFields.className = 'asset-empty';
  emptyFields.append('Select a record to inspect its typed fields.', document.createElement('br'), 'GUID references link to their target records.');
  fieldsDiv.append(emptyFields);

  const packDiv = document.createElement('div');
  packDiv.className = 'asset-pack';
  const packLabel = document.createElement('label');
  packLabel.append('Overrides (TOML)');
  const textarea = document.createElement('textarea');
  textarea.name = 'pack';
  textarea.spellcheck = false;
  textarea.setAttribute('aria-describedby', 'asset-pack-help');
  textarea.value = 'format = 1\natomic = true\n';
  packLabel.append(textarea);

  const packHelp = node('p', 'Stage a field, edit its value, then check the pack. A loaded DCB also validates field types and record targets.');
  packHelp.id = 'asset-pack-help';

  const actionsDiv = document.createElement('div');
  actionsDiv.className = 'asset-actions';
  const checkBtn = node('button', 'Check pack');
  checkBtn.type = 'button';
  checkBtn.dataset.action = 'check';
  const exportBtn = node('button', 'Download TOML');
  exportBtn.type = 'button';
  exportBtn.dataset.action = 'export';
  actionsDiv.append(checkBtn, exportBtn);
  packDiv.append(packLabel, packHelp, actionsDiv);
  inspectorSection.append(inspectorTitle, fieldsDiv, packDiv);

  const previewSection = document.createElement('section');
  previewSection.className = 'asset-preview-section';
  const previewTitle = document.createElement('div');
  previewTitle.className = 'asset-section-title';
  previewTitle.append(node('h2', 'Mesh preview'), node('span', 'glTF 2.0'));

  const modelForm = document.createElement('form');
  modelForm.className = 'asset-model';
  const modelLabel = document.createElement('label');
  modelLabel.append('Mesh or texture path');
  const modelInput = document.createElement('input');
  modelInput.name = 'model';
  modelInput.placeholder = 'Path to .cgf, .cga, .dds, .glb, .gltf or .obj';
  modelInput.spellcheck = false;
  modelInput.required = true;
  modelLabel.append(modelInput);
  modelForm.append(modelLabel, node('button', 'Load preview'));

  const canvasDiv = document.createElement('div');
  canvasDiv.className = 'asset-canvas';
  const canvasEmpty = document.createElement('p');
  canvasEmpty.className = 'asset-empty';
  canvasEmpty.append('Local geometry and textures', document.createElement('br'), 'Preview without launching the game');
  canvasDiv.append(canvasEmpty);

  const modelInfoP = node('p', 'Static geometry | BC1-BC7 DDS | Orbit controls');
  modelInfoP.className = 'asset-model-info';
  modelInfoP.setAttribute('role', 'status');

  const glbBtn = node('button', 'Download GLB');
  glbBtn.type = 'button';
  glbBtn.dataset.action = 'glb';
  glbBtn.disabled = true;

  const details = document.createElement('details');
  details.className = 'asset-import';
  details.append(node('summary', 'Import a custom model as a pack'));

  const importForm = document.createElement('form');
  const destLabel = document.createElement('label');
  destLabel.append('New pack folder');
  const destInput = document.createElement('input');
  destInput.name = 'destination';
  destInput.placeholder = 'A new local folder';
  destInput.required = true;
  destInput.spellcheck = false;
  destLabel.append(destInput);

  const virtLabel = document.createElement('label');
  virtLabel.append('VFS path inside files/');
  const virtInput = document.createElement('input');
  virtInput.name = 'virtual';
  virtInput.value = 'objects/custom/model.glb';
  virtInput.required = true;
  virtInput.spellcheck = false;
  virtLabel.append(virtInput);

  const importDesc = node('p', 'Imports glTF/OBJ plus the checked TOML into files/ and datacore/. GLB is an authoring asset, not a game-native CGF replacement.');

  importForm.append(destLabel, virtLabel, importDesc, node('button', 'Create pack'));
  details.append(importForm);

  previewSection.append(previewTitle, modelForm, canvasDiv, modelInfoP, glbBtn, details);

  workspace.append(treeSection, inspectorSection, previewSection);
  root.append(heading, sourceForm, statusPara, workspace);
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
