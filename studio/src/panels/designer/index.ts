import './style.css';
import type { PanelModule } from '../registry';
import { flatten, isContainer, kinds, sampleDesigns, validate, widget, type Design, type Kind, type Language, type Widget } from './model';
import { button, element, field, select } from './primitives';
import * as cpp from '../../codegen/cpp';
import * as csharp from '../../codegen/csharp';
import * as lua from '../../codegen/lua';
import * as c from '../../codegen/c';
import { toDom } from '../widgets/dom';
import { renderPanelView } from '../widgets/view';
import { applyPreviewEvent, designSnapshot } from './preview';
import '../widgets/widgets.css';

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
        renderOutline(); const previewed = renderStage(); renderInspector();
        undoButton.disabled = !undo.length; redoButton.disabled = !redo.length;
        if (previewed) message('Source parsed / canvas updated');
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
    function renderStage(): boolean {
      try { validate(design); } catch (error) {
        message(`Preview not updated: ${error instanceof Error ? error.message : String(error)}`, true);
        return false;
      }
      stage.querySelector('.designer-stage')?.remove();
      const body = element('div', '', 'designer-stage');
      body.append(element('p', `${flatten(design.nodes).length} nodes / synthetic preview`, 'designer-badge'));
      const preview = toDom(renderPanelView(designSnapshot(design)), document, event => {
        update(() => applyPreviewEvent(design, event));
        if (event.kind === 'button') message('Preview button activated');
      });
      preview.classList.add('designer-preview');
      for (const row of flatten(design.nodes)) {
        const item = preview.querySelector<HTMLElement>(`[data-node-id="${row.id}"]`);
        if (!item) continue;
        item.classList.add('designer-node');
        item.dataset.key = row.node.key;
        item.dataset.kind = row.node.kind;
        item.dataset.selected = String(row.node.key === selected);
        item.addEventListener('click', event => {
          event.stopPropagation(); selected = row.node.key; renderOutline(); renderInspector();
          for (const visible of stage.querySelectorAll<HTMLElement>('.designer-node'))
            visible.dataset.selected = String(visible.dataset.key === selected);
        });
      }
      body.append(preview); stage.append(body);
      return true;
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
