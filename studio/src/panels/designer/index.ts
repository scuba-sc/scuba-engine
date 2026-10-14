import './style.css';
import type { PanelModule } from '../registry';
import { flatten, isContainer, kinds, sampleDesigns, validate, widget, type Design, type Kind, type Language, type Widget } from './model';
import { button, element, field, select } from './primitives';
import * as cpp from '../../codegen/cpp';
import * as csharp from '../../codegen/csharp';
import * as lua from '../../codegen/lua';
import * as c from '../../codegen/c';

export const generators = { cpp, csharp, lua, c };
const languages = [
  { value: 'cpp', label: 'C++20' }, { value: 'csharp', label: 'C#' },
  { value: 'lua', label: 'Lua' }, { value: 'c', label: 'C11' },
];

export const designerPanel: PanelModule = {
  id: 'designer', title: 'Designer', order: 20,
  mount(container, { bridge }) {
    let design = sampleDesigns()[2];
    let selected = '2';
    let language: Language = 'cpp';
    let serial = 256;
    let disposed = false;
    let validSource = true;
    const undo: string[] = [], redo: string[] = [];
    const root = element('section', '', 'designer');
    const top = element('div', '', 'designer-top');
    const heading = element('div');
    heading.append(element('p', 'SCO.WIDGETS / 1.1', 'designer-kicker'), element('h1', 'Panel designer'),
      element('p', 'Shape a panel. Keep the source.', 'designer-muted'));
    const actions = element('div', '', 'designer-actions');
    const exportButton = button('Export plugin folder', () => void exportPlugin(), 'designer-primary');
    actions.append(select('Sample', sampleDesigns().map((d, i) => ({ value: String(i), label: d.title })), '2', value => {
      remember(); design = sampleDesigns()[Number(value)]; selected = design.nodes[0].key; render();
    }), exportButton);
    top.append(heading, actions);
    const grid = element('div', '', 'designer-grid');
    const box = (title: string) => {
      const section = element('section', '', 'designer-box');
      section.append(element('h2', title, 'designer-box-title'));
      grid.append(section);
      return section;
    };
    const outline = box('01 / Structure');
    const stage = box('02 / Live preview');
    const inspector = box('03 / Properties');
    const sourceBox = box('04 / Source');
    sourceBox.classList.add('designer-source');
    const sourceHead = element('div', '', 'designer-body designer-source-head');
    const languageField = select('Target language', languages, language, value => { language = value as Language; renderSource(); });
    const history = element('div', '', 'designer-actions');
    const undoButton = button('Undo', () => restore(undo, redo));
    const redoButton = button('Redo', () => restore(redo, undo));
    history.append(undoButton, redoButton, button('Regenerate', () => renderSource()));
    sourceHead.append(languageField, history, element('span', 'Edit generated syntax to update the canvas', 'designer-badge'));
    const source = element('textarea', '', 'designer-code');
    source.setAttribute('aria-label', 'Generated panel source');
    source.spellcheck = false;
    const status = element('p', '', 'designer-status');
    status.setAttribute('role', 'status');
    status.setAttribute('aria-live', 'polite');
    sourceBox.append(sourceHead, source, status);
    root.append(top, grid);
    container.append(root);
    function message(text: string, error = false) {
      status.textContent = text; status.dataset.error = String(error);
    }
    function remember() {
      undo.push(JSON.stringify(design));
      if (undo.length > 50) undo.shift();
      redo.length = 0;
    }
    function restore(from: string[], to: string[]) {
      const previous = from.pop();
      if (!previous) return;
      to.push(JSON.stringify(design)); design = JSON.parse(previous) as Design;
      selected = design.nodes[0].key; render();
    }
    function current() { return flatten(design.nodes).find(row => row.node.key === selected)?.node; }
    function update(change: () => void, redrawInspector = false) {
      remember(); change();
      renderOutline(); renderStage(); renderSource();
      if (redrawInspector) renderInspector();
    }
    function renderSource() {
      undoButton.disabled = !undo.length; redoButton.disabled = !redo.length;
      try {
        source.value = generators[language].generate(design); source.readOnly = false;
        validSource = true; exportButton.disabled = !['c', 'cpp'].includes(language);
        message(['csharp', 'lua'].includes(language)
          ? 'Source synchronized. This SDK builder supports Row, Column, Button, Toggle and Slider only. Native folder export: C++ or C.'
          : 'Source synchronized / native plugin ready to export');
      } catch (error) {
        source.value = ''; source.readOnly = true; validSource = false; exportButton.disabled = true;
        message(error instanceof Error ? error.message : String(error), true);
      }
    }
    source.addEventListener('input', () => {
      try {
        const parsed = generators[language].parse(source.value);
        remember(); design = parsed;
        if (!current()) selected = design.nodes[0].key;
        validSource = true; exportButton.disabled = !['c', 'cpp'].includes(language);
        renderOutline(); renderStage(); renderInspector();
        undoButton.disabled = !undo.length; redoButton.disabled = !redo.length;
        message('Source parsed / canvas updated');
      } catch (error) {
        validSource = false; exportButton.disabled = true;
        message(`Source not applied: ${error instanceof Error ? error.message : String(error)}`, true);
      }
    });
    function renderOutline() {
      outline.querySelector('.designer-body')?.remove();
      const body = element('div', '', 'designer-body');
      const tree = (nodes: Widget[]): HTMLUListElement => {
        const list = element('ul', '', 'designer-tree');
        for (const n of nodes) {
          const li = element('li');
          const b = button(`${n.kind}${n.label ? ` / ${n.label}` : ''}`, () => {
            selected = n.key; renderOutline(); renderStage(); renderInspector();
          });
          b.setAttribute('aria-pressed', String(n.key === selected));
          li.append(b);
          if (n.children.length) li.append(tree(n.children));
          list.append(li);
        }
        return list;
      };
      let insertKind: Kind = 'Button';
      const palette = select('Add widget', kinds.map(kind => ({ value: kind, label: kind })), insertKind, value => { insertKind = value as Kind; });
      body.append(tree(design.nodes), palette, button('Insert widget', () => {
        if (flatten(design.nodes).length >= 256) { message('A panel can contain at most 256 nodes', true); return; }
        const n = widget(insertKind, String(++serial));
        update(() => {
          const owner = current();
          (owner && isContainer(owner) ? owner.children : design.nodes).push(n);
          selected = n.key;
        }, true);
      }));
      const move = (offset: number) => {
        const row = flatten(design.nodes).find(entry => entry.node.key === selected);
        if (!row) return;
        const siblings = row.parent ? flatten(design.nodes)[row.parent - 1].node.children : design.nodes;
        const at = siblings.indexOf(row.node), to = at + offset;
        if (to < 0 || to >= siblings.length) return;
        update(() => { siblings.splice(at, 1); siblings.splice(to, 0, row.node); });
      };
      const rowActions = element('div', '', 'designer-actions');
      rowActions.append(button('Move up', () => move(-1)), button('Move down', () => move(1)));
      body.append(rowActions, button('Delete selected', () => {
        const row = flatten(design.nodes).find(entry => entry.node.key === selected);
        if (!row || (design.nodes.length === 1 && design.nodes[0] === row.node)) {
          message('Keep at least one root widget', true); return;
        }
        const siblings = row.parent ? flatten(design.nodes)[row.parent - 1].node.children : design.nodes;
        update(() => { siblings.splice(siblings.indexOf(row.node), 1); selected = design.nodes[0].key; }, true);
      }));
      outline.append(body);
    }
    function renderStage() {
      try { validate(design); } catch { return; }
      stage.querySelector('.designer-stage')?.remove();
      const body = element('div', '', 'designer-stage');
      body.append(element('p', `${flatten(design.nodes).length} nodes / synthetic preview`, 'designer-badge'));
      const preview = element('div', '', 'designer-preview');
      preview.append(element('div', design.title, 'designer-preview-title'));
      const renderNode = (n: Widget, ancestorDisabled: boolean): HTMLElement => {
        const item = element('div', '', 'designer-node');
        item.dataset.key = n.key;
        item.dataset.kind = n.kind;
        item.dataset.selected = String(n.key === selected);
        item.dataset.severity = String(n.severity);
        if (n.tooltip) item.title = n.tooltip;
        item.addEventListener('click', event => {
          event.stopPropagation(); selected = n.key; renderOutline(); renderInspector();
          for (const visible of stage.querySelectorAll<HTMLElement>('.designer-node'))
            visible.dataset.selected = String(visible.dataset.key === selected);
        });
        const disabled = ancestorDisabled || n.disabled;
        if (isContainer(n)) {
          if (!n.children.length) item.append(element('span', `Empty ${n.kind} / insert a widget`, 'designer-muted'));
          n.children.forEach(child => item.append(renderNode(child, disabled)));
        } else if (n.kind === 'Label') item.append(element('span', n.label));
        else if (n.kind === 'Button') {
          const b = button(n.label, () => message(`Preview click / ${n.label}`)); b.disabled = disabled; item.append(b);
        } else if (n.kind === 'Toggle') {
          const label = element('label', '', 'designer-field');
          const input = element('input'); input.type = 'checkbox'; input.checked = n.value === 1; input.disabled = disabled;
          input.addEventListener('change', () => update(() => { n.value = input.checked ? 1 : 0; }));
          label.append(element('span', n.label), input); item.append(label);
        } else if (n.kind === 'Slider') {
          const label = element('label', '', 'designer-field');
          const input = element('input'); input.type = 'range'; input.min = String(n.minimum);
          input.max = String(n.maximum); input.step = 'any'; input.value = String(n.value); input.disabled = disabled;
          input.addEventListener('change', () => update(() => { n.value = Number(input.value); }));
          label.append(element('span', `${n.label} / ${n.value}`), input); item.append(label);
        } else if (n.kind === 'TextInput') {
          const input = field(n.label, n.text, value => { remember(); n.text = value; renderSource(); }, n.password ? 'password' : 'text');
          input.querySelector('input')!.disabled = disabled; item.append(input);
        } else if (n.kind === 'Combo') {
          const input = select(n.label, [{ value: '-1', label: '(none)' }, ...n.items.map((label, i) => ({ value: String(i), label }))],
            String(n.index), value => update(() => { n.index = Number(value); }));
          input.querySelector('select')!.disabled = disabled; item.append(input);
        } else {
          item.append(element('span', n.label, 'designer-muted'));
          const count = n.kind === 'Table' ? n.items.length / n.columns : n.items.length;
          const start = n.visibleRows ? n.firstRow : 0;
          const end = n.visibleRows ? Math.min(count, start + n.visibleRows) : count;
          if (n.kind === 'Table') {
            const table = element('table'); table.setAttribute('aria-label', n.label || 'Table');
            const tbody = element('tbody');
            for (let i = start; i < end; i++) {
              const tr = element('tr');
              for (let j = 0; j < n.columns; j++) tr.append(element('td', n.items[i * n.columns + j]));
              tbody.append(tr);
            }
            table.append(tbody); item.append(table);
          } else for (let i = start; i < end; i++) {
            const b = button(n.items[i], () => update(() => { n.index = i; }), 'designer-list-item');
            b.setAttribute('aria-pressed', String(i === n.index)); b.disabled = disabled; item.append(b);
          }
          if (n.visibleRows) {
            const paging = element('div', '', 'designer-actions');
            const previous = button('Previous', () => update(() => { n.firstRow = Math.max(0, start - n.visibleRows); }));
            const next = button('Next', () => update(() => { n.firstRow = start + n.visibleRows; }));
            previous.disabled = disabled || start === 0; next.disabled = disabled || end >= count;
            paging.append(previous, next, element('span', `${start + 1}-${end} / ${count}`, 'designer-badge'));
            item.append(paging);
          }
        }
        return item;
      };
      design.nodes.forEach(n => preview.append(renderNode(n, false)));
      body.append(preview); stage.append(body);
    }
    function renderInspector() {
      inspector.querySelector('.designer-body')?.remove();
      const body = element('div', '', 'designer-body');
      body.append(field('Plugin id', design.id, value => update(() => { design.id = value; })),
        field('Panel title', design.title, value => update(() => { design.title = value; })));
      const n = current();
      if (n) {
        body.append(element('p', `${n.kind} / node ${flatten(design.nodes).find(row => row.node === n)!.id}`, 'designer-badge'));
        if (!isContainer(n)) body.append(field('Label', n.label, value => update(() => { n.label = value; })));
        const check = (label: string, value: boolean, change: (value: boolean) => void) => {
          const wrapper = element('label', '', 'designer-field');
          const input = element('input'); input.type = 'checkbox'; input.checked = value;
          input.addEventListener('change', () => update(() => change(input.checked)));
          wrapper.append(element('span', label), input); return wrapper;
        };
        body.append(check('Disabled', n.disabled, value => { n.disabled = value; }),
          field('Tooltip', n.tooltip, value => update(() => { n.tooltip = value; })),
          select('Severity', ['None', 'Info', 'Warning', 'Error'].map((label, i) => ({ label, value: String(i) })),
            String(n.severity), value => update(() => { n.severity = Number(value); })));
        const numeric = (label: string, key: 'value' | 'minimum' | 'maximum' | 'index' | 'columns' | 'firstRow' | 'visibleRows') =>
          field(label, String(n[key]), value => update(() => { n[key] = Number(value); }), 'number');
        if (n.kind === 'Toggle') body.append(check('Value', n.value === 1, value => { n.value = value ? 1 : 0; }));
        if (n.kind === 'Slider') body.append(numeric('Value', 'value'), numeric('Minimum', 'minimum'), numeric('Maximum', 'maximum'));
        if (n.kind === 'TextInput') body.append(field('Text', n.text, value => update(() => { n.text = value; })),
          check('Password masking', n.password, value => { n.password = value; }));
        if (['Combo', 'List', 'Table'].includes(n.kind)) {
          const label = element('label', '', 'designer-field'), input = element('textarea');
          input.value = n.items.join('\n'); input.rows = 4;
          input.addEventListener('input', () => update(() => { n.items = input.value ? input.value.split('\n') : []; }));
          label.append(element('span', n.kind === 'Table' ? 'Cells / one per line, row-major' : 'Items / one per line'), input);
          body.append(label);
        }
        if (['Combo', 'List'].includes(n.kind)) body.append(numeric('Selection index', 'index'));
        if (n.kind === 'Table') body.append(numeric('Columns', 'columns'));
        if (['List', 'Table'].includes(n.kind)) body.append(numeric('First row', 'firstRow'), numeric('Visible rows (0 = all)', 'visibleRows'));
        const descendants = new Set(flatten([n]).map(row => row.node.key));
        const row = flatten(design.nodes).find(entry => entry.node === n)!;
        const parentKey = row.parent ? flatten(design.nodes)[row.parent - 1].node.key : '';
        body.append(select('Parent', [{ value: '', label: 'Panel root' }, ...flatten(design.nodes)
          .filter(entry => isContainer(entry.node) && !descendants.has(entry.node.key))
          .map(entry => ({ value: entry.node.key, label: `${entry.node.kind} ${entry.id}` }))], parentKey, value => {
          update(() => {
            const siblings = row.parent ? flatten(design.nodes)[row.parent - 1].node.children : design.nodes;
            const target = value ? flatten(design.nodes).find(entry => entry.node.key === value)!.node.children : design.nodes;
            siblings.splice(siblings.indexOf(n), 1); target.push(n);
          }, true);
        }));
      }
      inspector.append(body);
    }
    async function exportPlugin() {
      if (!validSource || (language !== 'c' && language !== 'cpp')) return;
      exportButton.disabled = true;
      try {
        validate(design);
        const result = await bridge.call<{ folder: string; files: string[] }>('designer.export', {
          id: design.id, language, source: generators[language].generate(design),
        });
        if (!disposed) message(`Exported exports/${result.folder} / ${result.files.join(', ')}`);
      } catch (error) {
        if (!disposed) message(`Export failed: ${error instanceof Error ? error.message : String(error)}`, true);
      } finally { if (!disposed) exportButton.disabled = !validSource || !['c', 'cpp'].includes(language); }
    }
    function render() { renderOutline(); renderStage(); renderInspector(); renderSource(); }
    render();
    return () => { disposed = true; root.remove(); };
  },
};
