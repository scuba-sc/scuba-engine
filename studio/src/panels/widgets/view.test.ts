import { describe, expect, it } from 'vitest';
import { eventToJson, isWidgetPanel, type WidgetNode, type WidgetPanel } from './types';
import { buildTree, findNode, pageWindow, renderPanelView, type VNode } from './view';

let nextId = 1;
function node(kind: WidgetNode['kind'], fields: Partial<WidgetNode> = {}): WidgetNode {
  return {
    id: nextId++,
    parent: 0,
    kind,
    kind_code: 0,
    disabled: false,
    password: false,
    label: '',
    text: '',
    value: 0,
    minimum: 0,
    maximum: 0,
    index: 0,
    items: [],
    columns: 0,
    extended: false,
    severity: 'none',
    tooltip: '',
    first_row: 0,
    visible_rows: 0,
    ...fields,
  };
}

function panel(nodes: WidgetNode[]): WidgetPanel {
  return { id: 'demo.main', title: 'Demo', owner: 'demo', revision: 3, extended: nodes.some((n) => n.extended), nodes };
}

const byNode = (id: number) => (v: VNode) => v.attrs?.['data-node-id'] === String(id);
const byTag = (tag: string) => (v: VNode) => v.tag === tag;
const texts = (v: VNode | undefined, tag: string): string[] => {
  const out: string[] = [];
  const walk = (x: VNode) => {
    if (x.tag === tag && x.text !== undefined) out.push(x.text);
    x.children?.forEach(walk);
  };
  if (v) walk(v);
  return out;
};
const state = (value = '', checked = false) => ({ value, checked });

describe('widgets view (sco.widgets 1.0)', () => {
  nextId = 1;
  const column = node('column', { id: 1 });
  const row = node('row', { id: 2, parent: 1 });
  const button = node('button', { id: 3, parent: 2, label: 'Run' });
  const toggle = node('toggle', { id: 4, parent: 2, label: 'Enabled', value: 1 });
  const slider = node('slider', { id: 5, parent: 1, label: 'Amount', value: 0.5, minimum: 0, maximum: 1 });
  const combo = node('combo', { id: 6, parent: 1, label: 'Mode', items: ['Alpha', 'Beta'], index: 1 });
  const list = node('list', { id: 7, parent: 1, label: 'Items', items: ['One', 'Two'], index: -1 });
  const text = node('text_input', { id: 8, parent: 1, label: 'Note', text: 'hello' });
  const secret = node('text_input', { id: 9, parent: 1, label: 'Secret', password: true });
  const table = node('table', { id: 10, parent: 1, label: 'Values', columns: 2, items: ['Name', 'Value', 'Alpha', '42'] });
  const label = node('label', { id: 11, parent: 1, label: 'Plain text' });
  const view = renderPanelView(panel([column, row, button, toggle, slider, combo, list, text, secret, table, label]));

  it('builds the preorder tree from parent ids', () => {
    const roots = buildTree([column, row, button, toggle, slider]);
    expect(roots.map((r) => r.node.id)).toEqual([1]);
    expect(roots[0].children.map((c) => c.node.id)).toEqual([2, 5]);
    expect(roots[0].children[0].children.map((c) => c.node.id)).toEqual([3, 4]);
  });

  it('renders the panel title and every node kind', () => {
    expect(texts(view, 'h3')).toEqual(['Demo']);
    for (const n of [column, row, button, toggle, slider, combo, list, text, table, label])
      expect(findNode(view, byNode(n.id))?.attrs?.class).toContain(`w-${n.kind}`);
    expect(texts(findNode(view, byNode(10)), 'td')).toEqual(['Name', 'Value', 'Alpha', '42']);
    expect(texts(findNode(view, byNode(11)), 'span')).toContain('Plain text');
  });

  it('maps BUTTON and TOGGLE input to widgets.event params', () => {
    const b = findNode(findNode(view, byNode(3))!, byTag('button'))!;
    expect(b.text).toBe('Run');
    expect(b.on?.click?.(state())).toEqual({ panel: 'demo.main', node_id: 3, kind: 'button' });
    const t = findNode(findNode(view, byNode(4))!, byTag('input'))!;
    expect(t.attrs).toMatchObject({ type: 'checkbox', checked: '' });
    expect(t.on?.change?.(state('on', false))).toEqual({ panel: 'demo.main', node_id: 4, kind: 'toggle', value: 0 });
  });

  it('sends SLIDER values in range and drops out-of-range ones', () => {
    const s = findNode(findNode(view, byNode(5))!, byTag('input'))!;
    expect(s.attrs).toMatchObject({ type: 'range', min: '0', max: '1', value: '0.5' });
    expect(s.on?.change?.(state('0.25'))).toEqual({ panel: 'demo.main', node_id: 5, kind: 'slider', value: 0.25 });
    expect(s.on?.change?.(state('2'))).toBeNull();
    expect(s.on?.change?.(state('nope'))).toBeNull();
  });

  it('maps COMBO, LIST and TEXT_INPUT selections', () => {
    const select = findNode(findNode(view, byNode(6))!, byTag('select'))!;
    expect(select.children?.map((o) => o.attrs?.selected !== undefined)).toEqual([false, true]);
    expect(select.on?.change?.(state('0'))).toEqual({ panel: 'demo.main', node_id: 6, kind: 'combo', index: 0 });
    const items = findNode(findNode(view, byNode(7))!, byTag('ul'))!.children!;
    expect(items.map((li) => li.text)).toEqual(['One', 'Two']);
    expect(items[1].on?.click?.(state())).toEqual({ panel: 'demo.main', node_id: 7, kind: 'list', index: 1 });
    const input = findNode(findNode(view, byNode(8))!, byTag('input'))!;
    expect(input.attrs).toMatchObject({ type: 'text', value: 'hello', maxlength: '255' });
    expect(input.on?.change?.(state('typed'))).toEqual({ panel: 'demo.main', node_id: 8, kind: 'text_input', text: 'typed' });
  });

  it('masks password inputs', () => {
    const input = findNode(findNode(view, byNode(9))!, byTag('input'))!;
    expect(input.attrs).toMatchObject({ type: 'password', value: '' });
  });
});

describe('widgets view (sco.widgets 1.1 tooltip, severity, paging)', () => {
  it('shows tooltip and severity', () => {
    const warn = node('button', { id: 1, label: 'Launch', severity: 'warning', tooltip: 'Uses fuel', extended: true });
    const v = findNode(renderPanelView(panel([warn])), byNode(1))!;
    expect(v.attrs?.title).toBe('Uses fuel');
    expect(v.attrs?.['data-severity']).toBe('warning');
    expect(v.attrs?.class).toContain('w-sev-warning');
    expect(texts(v, 'span')).toContain('warning');
  });

  it('pages a list and emits PAGE events', () => {
    const items = ['a', 'b', 'c', 'd', 'e'];
    const paged = node('list', { id: 1, label: 'Log', items, index: 3, first_row: 2, visible_rows: 2, extended: true });
    expect(pageWindow(paged)).toEqual({ first: 2, end: 4, rows: 5, prev: 0, next: 4 });
    const v = findNode(renderPanelView(panel([paged])), byNode(1))!;
    const rows = findNode(v, byTag('ul'))!.children!;
    expect(rows.map((r) => r.text)).toEqual(['c', 'd']);
    expect(rows[1].attrs?.['aria-selected']).toBe('true');
    expect(rows[0].on?.click?.(state())).toMatchObject({ kind: 'list', index: 2 });
    expect(texts(v, 'span')).toContain('3-4 of 5');
    const [prev, next] = findNode(v, (x) => x.attrs?.class === 'w-pager')!.children!.filter((c) => c.tag === 'button');
    expect(prev.on?.click?.(state())).toEqual({ panel: 'demo.main', node_id: 1, kind: 'page', row: 0 });
    expect(next.on?.click?.(state())).toEqual({ panel: 'demo.main', node_id: 1, kind: 'page', row: 4 });
  });

  it('pages a table by rows and disables the edge buttons', () => {
    const cells = ['r0a', 'r0b', 'r1a', 'r1b', 'r2a', 'r2b'];
    const paged = node('table', { id: 1, columns: 2, items: cells, first_row: 0, visible_rows: 2, extended: true });
    const v = findNode(renderPanelView(panel([paged])), byNode(1))!;
    expect(texts(v, 'td')).toEqual(['r0a', 'r0b', 'r1a', 'r1b']);
    const [prev, next] = findNode(v, (x) => x.attrs?.class === 'w-pager')!.children!.filter((c) => c.tag === 'button');
    expect(prev.attrs?.disabled).toBe('');
    expect(prev.on).toBeUndefined();
    expect(next.on?.click?.(state())).toEqual({ panel: 'demo.main', node_id: 1, kind: 'page', row: 2 });
  });

  it('inherits disabled from a ROW/COLUMN ancestor', () => {
    const row = node('row', { id: 1, disabled: true });
    const b = node('button', { id: 2, parent: 1, label: 'Go' });
    const l = node('list', { id: 3, parent: 1, label: 'L', items: ['x'], index: -1 });
    const v = renderPanelView(panel([row, b, l]));
    expect(findNode(findNode(v, byNode(2))!, byTag('button'))!.attrs?.disabled).toBe('');
    expect(findNode(findNode(v, byNode(3))!, byTag('li'))!.on).toBeUndefined();
  });
});

describe('widgets wire helpers', () => {
  it('omits absent payload fields', () => {
    expect(eventToJson({ panel: 'p.x', node_id: 2, kind: 'button' })).toEqual({ panel: 'p.x', node_id: 2, kind: 'button' });
    expect(eventToJson({ panel: 'p.x', node_id: 2, kind: 'page', row: 4 })).toEqual({ panel: 'p.x', node_id: 2, kind: 'page', row: 4 });
  });

  it('recognises panel snapshots', () => {
    expect(isWidgetPanel({ id: 'a.b', title: 'T', nodes: [] })).toBe(true);
    expect(isWidgetPanel({ id: 'a.b' })).toBe(false);
    expect(isWidgetPanel(null)).toBe(false);
  });
});
