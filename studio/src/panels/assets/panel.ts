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

function download(data: BlobPart, name: string, type: string): void {
  const url = URL.createObjectURL(new Blob([data], { type }));
  const link = document.createElement('a');
  link.href = url;
  link.download = name;
  link.click();
  URL.revokeObjectURL(url);
}

const TEMPLATE_HTML = `
    <div class="asset-heading"><div><span class="asset-eyebrow">LOCAL AUTHORING</span><h1>Asset Lab</h1></div>
      <p>Your install. Your files.<br>No game data is included.</p></div>
    <form class="asset-source"><label>DataCore or override pack
      <input name="database" placeholder="Path to your Game2.dcb or overrides.toml" spellcheck="false" required></label>
      <button type="submit" class="asset-primary">Open database</button></form>
    <p class="asset-status" role="status" aria-live="polite">Open a local database to browse records, structs and GUID references.</p>
    <div class="asset-workspace">
      <section class="asset-tree"><div class="asset-section-title"><h2>Record tree</h2><button type="button" data-action="structs">Structs</button></div>
        <form class="asset-search"><label>Filter name, type or GUID<input name="filter" placeholder="Search records" spellcheck="false"></label><button>Find</button></form>
        <div class="asset-records"><p class="asset-empty">No database open</p></div>
        <div class="asset-pagination"><button type="button" data-action="previous">Previous</button><span data-count>0 records</span><button type="button" data-action="next">Next</button></div>
      </section>
      <section class="asset-inspector"><div class="asset-section-title"><h2>Field inspector</h2><span data-selection>Nothing selected</span></div>
        <div class="asset-fields"><p class="asset-empty">Select a record to inspect its typed fields.<br>GUID references link to their target records.</p></div>
        <div class="asset-pack"><label>Overrides (TOML)<textarea name="pack" spellcheck="false" aria-describedby="asset-pack-help">format = 1\natomic = true\n</textarea></label>
          <p id="asset-pack-help">Stage a field, edit its value, then check the pack. A loaded DCB also validates field types and record targets.</p>
          <div class="asset-actions"><button type="button" data-action="check">Check pack</button><button type="button" data-action="export">Download TOML</button></div></div>
      </section>
      <section class="asset-preview-section"><div class="asset-section-title"><h2>Mesh preview</h2><span>glTF 2.0</span></div>
        <form class="asset-model"><label>Mesh or texture path<input name="model" placeholder="Path to .cgf, .cga, .dds, .glb, .gltf or .obj" spellcheck="false" required></label><button>Load preview</button></form>
        <div class="asset-canvas"><p class="asset-empty">Local geometry and textures<br>Preview without launching the game</p></div>
        <p class="asset-model-info" role="status">Static geometry | BC1-BC7 DDS | Orbit controls</p>
        <button type="button" data-action="glb" disabled>Download GLB</button>
        <details class="asset-import"><summary>Import a custom model as a pack</summary>
          <form><label>New pack folder<input name="destination" placeholder="A new local folder" required spellcheck="false"></label>
            <label>VFS path inside files/<input name="virtual" value="objects/custom/model.glb" required spellcheck="false"></label>
            <p>Imports glTF/OBJ plus the checked TOML into files/ and datacore/. GLB is an authoring asset, not a game-native CGF replacement.</p>
            <button>Create pack</button></form></details>
      </section>
    </div>`;

class AssetLabPanel {
  private readonly root: HTMLElement;
  private readonly bridge: HostdBridge;
  private readonly pack: HTMLTextAreaElement;
  private readonly status: HTMLElement;
  private readonly records: HTMLElement;
  private readonly fields: HTMLElement;
  private readonly selection: HTMLElement;
  private readonly count: HTMLElement;
  private readonly canvas: HTMLElement;
  private readonly modelInfo: HTMLElement;

  private preview: ReturnType<typeof createPreview> | undefined;
  private closed = false;
  private busy = false;
  private offset = 0;
  private total = 0;
  private kind = '';
  private selected = '';
  private lastGlb: ArrayBuffer | undefined;

  constructor(container: HTMLElement, bridge: HostdBridge) {
    this.bridge = bridge;
    this.root = document.createElement('section');
    this.root.className = 'asset-lab';
    this.root.innerHTML = TEMPLATE_HTML;

    this.pack = this.required<HTMLTextAreaElement>('textarea');
    this.status = this.required<HTMLElement>('.asset-status');
    this.records = this.required<HTMLElement>('.asset-records');
    this.fields = this.required<HTMLElement>('.asset-fields');
    this.selection = this.required<HTMLElement>('[data-selection]');
    this.count = this.required<HTMLElement>('[data-count]');
    this.canvas = this.required<HTMLElement>('.asset-canvas');
    this.modelInfo = this.required<HTMLElement>('.asset-model-info');

    container.append(this.root);
    this.bindEvents();
    this.updateButtons();
  }

  public destroy(): void {
    this.closed = true;
    this.preview?.dispose();
    this.root.remove();
  }

  private required<T extends HTMLElement>(selector: string): T {
    const element = this.root.querySelector<T>(selector);
    if (!element) throw new Error(`Missing Asset Lab element: ${selector}`);
    return element;
  }

  private input(name: string): HTMLInputElement {
    return this.required<HTMLInputElement>(`input[name="${name}"]`);
  }

  private updateButtons(): void {
    for (const button of this.root.querySelectorAll<HTMLButtonElement>('button')) {
      button.disabled = this.busy;
    }
    this.required<HTMLButtonElement>('[data-action="previous"]').disabled = this.busy || this.offset === 0;
    this.required<HTMLButtonElement>('[data-action="next"]').disabled = this.busy || this.offset + 100 >= this.total;
    this.required<HTMLButtonElement>('[data-action="glb"]').disabled = this.busy || !this.lastGlb;
  }

  private run(action: () => Promise<void>): void {
    if (this.busy || this.closed) return;
    this.busy = true;
    this.status.textContent = 'Working with local files...';
    this.status.dataset.error = 'false';
    this.updateButtons();
    void action().catch((error: unknown) => {
      if (this.closed) return;
      this.status.textContent = error instanceof Error ? error.message : String(error);
      this.status.dataset.error = 'true';
    }).finally(() => {
      this.busy = false;
      if (!this.closed) this.updateButtons();
    });
  }

  private async inspect(name: string): Promise<void> {
    const response = list(await this.bridge.call<unknown>('assets.fields', { record: name }));
    if (this.closed) return;
    this.selected = name;
    this.selection.textContent = name;
    this.fields.replaceChildren();
    if (!response.length) this.fields.append(node('p', 'This record has no readable fields.'));
    for (const raw of response) {
      this.fields.append(this.createFieldRow(raw));
    }
  }

  private createFieldRow(raw: unknown): HTMLElement {
    const field = record(raw);
    const path = text(field['path']);
    const value = text(field['value']);
    const row = document.createElement('div');
    row.className = 'asset-field';
    const label = document.createElement('div');
    label.append(node('strong', path), node('small', text(field['type'])));
    row.append(label, node('code', value));
    if (field['reference']) {
      const target = record(field['reference']);
      const jump = node('button', `Open ${text(target['name'])}`);
      jump.type = 'button';
      jump.onclick = () => this.run(async () => {
        await this.inspect(text(target['guid']));
        this.status.textContent = 'Reference target opened.';
      });
      row.append(jump);
    }
    if (!field['array'] && !['strong', 'weak'].includes(text(field['type']))) {
      const stage = node('button', 'Stage field');
      stage.type = 'button';
      stage.onclick = () => {
        const isGuid = this.selected.startsWith('guid:') || /^[0-9a-f-]{36}$/i.test(this.selected);
        const recordOrGuid = isGuid ? 'guid' : 'record';
        const formattedSel = JSON.stringify(this.selected.replace(/^guid:/, ''));
        this.pack.value += `\n[[set]]\n${recordOrGuid} = ${formattedSel}\nfield = ${JSON.stringify(path)}\nvalue = ${value}\n`;
        this.status.textContent = `Staged ${path}. Edit the TOML value and check the pack.`;
      };
      row.append(stage);
    }
    return row;
  }

  private async refresh(): Promise<void> {
    const response = record(await this.bridge.call<unknown>('assets.records', {
      offset: this.offset,
      limit: 100,
      filter: this.input('filter').value,
    }));
    const items = list(response['items']);
    if (typeof response['total'] !== 'number') throw new Error('Invalid record count');
    if (this.closed) return;
    this.total = response['total'];
    this.count.textContent = `${Math.min(this.offset + 1, this.total)}-${Math.min(this.offset + items.length, this.total)} / ${this.total}`;
    this.records.replaceChildren();
    this.renderRecordTree(items);
  }

  private renderRecordTree(items: unknown[]): void {
    const groups = new Map<string, HTMLElement>();
    for (const raw of items) {
      const item = record(raw);
      const name = text(item['name']);
      const type = text(item['struct']) || 'Pack operations';
      let group = groups.get(type);
      if (!group) {
        const branch = document.createElement('details');
        branch.open = true;
        branch.append(node('summary', type));
        group = document.createElement('div');
        branch.append(group);
        this.records.append(branch);
        groups.set(type, group);
      }
      group.append(this.createRecordButton(item, name));
    }
    if (!items.length) this.records.append(node('p', 'No matching records.'));
  }

  private createRecordButton(item: Record<string, unknown>, name: string): HTMLElement {
    const button = node('button', name || '(instance operation)');
    button.type = 'button';
    const guid = text(item['guid']);
    if (guid) button.append(node('small', guid));
    button.onclick = () => this.run(async () => {
      if (this.kind === 'toml') {
        this.fields.replaceChildren(node('p', `Pack operation at line ${String(item['line'])}: ${String(item['field'] ?? '')}. Open the base .dcb to resolve its fields.`));
        this.selection.textContent = name;
      } else {
        await this.inspect(guid || name);
      }
      this.status.textContent = this.kind === 'toml'
        ? 'TOML operations are shown in the pack editor.'
        : 'Record fields loaded. Source database is unchanged.';
    });
    return button;
  }

  private async check(): Promise<void> {
    const response = record(await this.bridge.call<unknown>('assets.pack', { text: this.pack.value }));
    if (!this.closed) this.pack.value = text(response['text']);
  }

  private bindEvents(): void {
    this.required<HTMLFormElement>('.asset-source').onsubmit = (event) => {
      event.preventDefault();
      this.run(async () => {
        const response = record(await this.bridge.call<unknown>('assets.open', { path: this.input('database').value }));
        if (this.closed) return;
        this.kind = text(response['kind']);
        this.pack.value = text(response['pack']);
        this.offset = 0;
        this.selected = '';
        this.selection.textContent = 'Nothing selected';
        this.fields.replaceChildren(node('p', 'Select a record from the tree.'));
        await this.refresh();
        this.status.textContent = `${this.total} ${this.kind === 'dcb' ? 'records' : 'pack operations'} loaded. Source files are read-only.`;
      });
    };

    this.required<HTMLFormElement>('.asset-search').onsubmit = (event) => {
      event.preventDefault();
      this.run(async () => {
        this.offset = 0;
        await this.refresh();
        this.status.textContent = `${this.total} matches.`;
      });
    };

    this.required<HTMLButtonElement>('[data-action="previous"]').onclick = () => this.run(async () => {
      this.offset = Math.max(this.offset - 100, 0);
      await this.refresh();
      this.status.textContent = 'Previous records loaded.';
    });

    this.required<HTMLButtonElement>('[data-action="next"]').onclick = () => this.run(async () => {
      this.offset += 100;
      await this.refresh();
      this.status.textContent = 'Next records loaded.';
    });

    this.required<HTMLButtonElement>('[data-action="structs"]').onclick = () => this.run(async () => {
      const structs = list(await this.bridge.call<unknown>('assets.structs'));
      if (this.closed) return;
      this.fields.replaceChildren();
      this.selection.textContent = 'Struct definitions';
      for (const raw of structs) {
        const definition = record(raw);
        const branch = document.createElement('details');
        branch.append(node('summary', text(definition['name'])));
        if (definition['properties']) {
          for (const property of list(definition['properties'])) {
            const entry = record(property);
            branch.append(node('p', `${text(entry['name'])}: ${text(entry['type'])} (conversion ${String(entry['conversion'])})`));
          }
        }
        this.fields.append(branch);
      }
      this.status.textContent = `${structs.length} struct definitions.`;
    });

    this.required<HTMLButtonElement>('[data-action="check"]').onclick = () => this.run(async () => {
      await this.check();
      this.status.textContent = this.kind === 'dcb'
        ? 'Pack syntax and all overrides validated against the database.'
        : 'Pack syntax valid. Open a DCB for semantic validation.';
    });

    this.required<HTMLButtonElement>('[data-action="export"]').onclick = () => this.run(async () => {
      await this.check();
      const result = record(await this.bridge.call<unknown>('assets.export'));
      if (this.closed) return;
      download(text(result['text']), 'overrides.toml', 'application/toml');
      this.status.textContent = 'Validated override pack downloaded.';
    });

    this.required<HTMLFormElement>('.asset-model').onsubmit = (event) => {
      event.preventDefault();
      this.run(async () => {
        const result = record(await this.bridge.call<unknown>('assets.convert', { path: this.input('model').value }));
        if (this.closed) return;
        this.lastGlb = decodeGlb(text(result['glb']));
        try {
          if (!this.preview) {
            this.preview = createPreview(this.canvas);
            this.canvas.querySelector('.asset-empty')?.remove();
          }
          this.modelInfo.textContent = await this.preview.load(this.lastGlb);
        } catch (error: unknown) {
          this.modelInfo.textContent = error instanceof Error ? error.message : String(error);
          throw error;
        }
        this.status.textContent = `GLB ready (${Number(result['bytes']).toLocaleString()} bytes). Source file is unchanged.`;
      });
    };

    this.required<HTMLButtonElement>('[data-action="glb"]').onclick = () => {
      if (this.lastGlb) download(this.lastGlb, 'asset.glb', 'model/gltf-binary');
    };

    this.required<HTMLFormElement>('.asset-import form').onsubmit = (event) => {
      event.preventDefault();
      this.run(async () => {
        await this.check();
        await this.bridge.call('assets.import', {
          path: this.input('model').value,
          destination: this.input('destination').value,
          virtualPath: this.input('virtual').value,
        });
        this.status.textContent = 'Pack created with files/ geometry and datacore/ overrides. Existing files were not overwritten.';
      });
    };
  }
}

export function mountAssetLab(container: HTMLElement, bridge: HostdBridge): () => void {
  const panel = new AssetLabPanel(container, bridge);
  return () => panel.destroy();
}
