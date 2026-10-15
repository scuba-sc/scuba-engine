// Pure view of a sco.widgets panel snapshot: a small element tree (VNode) whose input handlers
// return the widgets.event params to send. dom.ts turns it into DOM; tests inspect it directly.
import type { WidgetEventParams, WidgetNode, WidgetPanel } from './types';

export interface ControlState {
  value: string;
  checked: boolean;
}

export type EventHandler = (state: ControlState) => WidgetEventParams | null;

export interface VNode {
  tag: string;
  attrs?: Record<string, string>;
  text?: string;
  children?: VNode[];
  on?: Partial<Record<'click' | 'change', EventHandler>>;
}

export interface TreeNode {
  node: WidgetNode;
  disabled: boolean;
  children: TreeNode[];
}

export function buildTree(nodes: WidgetNode[]): TreeNode[] {
  const byId = new Map<number, TreeNode>();
  const roots: TreeNode[] = [];
  for (const node of nodes) {
    const parent = node.parent ? byId.get(node.parent) : undefined;
    const item: TreeNode = { node, disabled: node.disabled || (parent?.disabled ?? false), children: [] };
    byId.set(node.id, item);
    if (parent) parent.children.push(item);
    else roots.push(item);
  }
  return roots;
}

export function rowCount(node: WidgetNode): number {
  if (node.kind === 'table') return node.columns > 0 ? Math.floor(node.items.length / node.columns) : 0;
  return node.kind === 'list' ? node.items.length : 0;
}

export interface PageWindow {
  first: number;
  end: number;
  rows: number;
  prev: number | null;
  next: number | null;
}

export function pageWindow(node: WidgetNode): PageWindow {
  const rows = rowCount(node);
  if (node.visible_rows <= 0) return { first: 0, end: rows, rows, prev: null, next: null };
  const first = Math.min(node.first_row, Math.max(0, rows - 1));
  const end = Math.min(rows, first + node.visible_rows);
  return {
    first,
    end,
    rows,
    prev: first > 0 ? Math.max(0, first - node.visible_rows) : null,
    next: end < rows ? end : null,
  };
}

function h(tag: string, attrs?: Record<string, string>, children?: VNode[], text?: string): VNode {
  const out: VNode = { tag };
  if (attrs) out.attrs = attrs;
  if (children) out.children = children;
  if (text !== undefined) out.text = text;
  return out;
}

function decorate(item: TreeNode, base: string): Record<string, string> {
  const n = item.node;
  const attrs: Record<string, string> = { class: `w-node w-${n.kind || 'unknown'}`, 'data-node-id': String(n.id) };
  if (n.severity !== 'none') {
    attrs.class += ` w-sev-${n.severity}`;
    attrs['data-severity'] = n.severity;
  }
  if (n.tooltip) attrs.title = n.tooltip;
  if (base) attrs.class += ` ${base}`;
  return attrs;
}

function controlAttrs(item: TreeNode, extra: Record<string, string> = {}): Record<string, string> {
  const attrs: Record<string, string> = { ...extra, 'aria-label': item.node.label };
  if (item.disabled) attrs.disabled = '';
  return attrs;
}

function severityBadge(item: TreeNode): VNode[] {
  const s = item.node.severity;
  return s === 'none' ? [] : [h('span', { class: `w-badge w-sev-${s}`, role: 'status' }, undefined, s)];
}

function pager(panel: string, item: TreeNode): VNode[] {
  const n = item.node;
  if (n.visible_rows <= 0) return [];
  const w = pageWindow(n);
  const page = (row: number | null, text: string, label: string): VNode => {
    const attrs: Record<string, string> = { class: 'w-page', 'aria-label': `${label} ${n.label}`.trim() };
    if (row === null || item.disabled) attrs.disabled = '';
    const v = h('button', attrs, undefined, text);
    if (row !== null) v.on = { click: () => ({ panel, node_id: n.id, kind: 'page', row }) };
    return v;
  };
  const shown = w.rows === 0 ? '0 of 0' : `${w.first + 1}-${w.end} of ${w.rows}`;
  return [
    h('div', { class: 'w-pager' }, [
      page(w.prev, '<', 'Previous rows of'),
      h('span', { class: 'w-page-info' }, undefined, shown),
      page(w.next, '>', 'Next rows of'),
    ]),
  ];
}

function renderNode(panel: string, item: TreeNode): VNode {
  const n = item.node;
  const kids = () => item.children.map((c) => renderNode(panel, c));
  const caption = n.label ? [h('span', { class: 'w-caption' }, undefined, n.label)] : [];
  switch (n.kind) {
    case 'column':
    case 'row':
      return h('div', decorate(item, ''), kids());
    case 'label':
      return h('div', decorate(item, ''), [h('span', { class: 'w-text' }, undefined, n.label), ...severityBadge(item)]);
    case 'button': {
      const button = h('button', controlAttrs(item, { type: 'button' }), undefined, n.label);
      button.on = { click: () => ({ panel, node_id: n.id, kind: 'button' }) };
      return h('div', decorate(item, ''), [button, ...severityBadge(item)]);
    }
    case 'toggle': {
      const attrs = controlAttrs(item, { type: 'checkbox' });
      if (n.value === 1) attrs.checked = '';
      const input = h('input', attrs);
      input.on = { change: (s) => ({ panel, node_id: n.id, kind: 'toggle', value: s.checked ? 1 : 0 }) };
      return h('label', decorate(item, ''), [input, ...caption, ...severityBadge(item)]);
    }
    case 'slider': {
      const input = h(
        'input',
        controlAttrs(item, { type: 'range', min: String(n.minimum), max: String(n.maximum), step: 'any', value: String(n.value) }),
      );
      input.on = {
        change: (s) => {
          const value = Number(s.value);
          if (!Number.isFinite(value) || value < n.minimum || value > n.maximum) return null;
          return { panel, node_id: n.id, kind: 'slider', value };
        },
      };
      return h('label', decorate(item, ''), [
        ...caption,
        input,
        h('output', { class: 'w-value' }, undefined, String(n.value)),
        ...severityBadge(item),
      ]);
    }
    case 'combo': {
      const options = n.items.map((text, i) => {
        const attrs: Record<string, string> = { value: String(i) };
        if (i === n.index) attrs.selected = '';
        return h('option', attrs, undefined, text);
      });
      if (n.index < 0) options.unshift(h('option', { value: '-1', selected: '' }, undefined, ''));
      const select = h('select', controlAttrs(item), options);
      select.on = {
        change: (s) => {
          const index = Number.parseInt(s.value, 10);
          return Number.isInteger(index) ? { panel, node_id: n.id, kind: 'combo', index } : null;
        },
      };
      return h('label', decorate(item, ''), [...caption, select, ...severityBadge(item)]);
    }
    case 'list': {
      const w = pageWindow(n);
      const rows: VNode[] = [];
      for (let i = w.first; i < w.end; i++) {
        const attrs: Record<string, string> = { role: 'option', 'aria-selected': String(i === n.index), 'data-index': String(i) };
        if (i === n.index) attrs.class = 'w-selected';
        const li = h('li', attrs, undefined, n.items[i]);
        if (!item.disabled) li.on = { click: () => ({ panel, node_id: n.id, kind: 'list', index: i }) };
        rows.push(li);
      }
      const listAttrs: Record<string, string> = { role: 'listbox', 'aria-label': n.label };
      if (item.disabled) listAttrs['aria-disabled'] = 'true';
      return h('div', decorate(item, ''), [...caption, ...severityBadge(item), h('ul', listAttrs, rows), ...pager(panel, item)]);
    }
    case 'text_input': {
      const input = h(
        'input',
        controlAttrs(item, { type: n.password ? 'password' : 'text', value: n.text, maxlength: '255', autocomplete: 'off' }),
      );
      input.on = { change: (s) => ({ panel, node_id: n.id, kind: 'text_input', text: s.value }) };
      return h('label', decorate(item, ''), [...caption, input, ...severityBadge(item)]);
    }
    case 'table': {
      const w = pageWindow(n);
      const body: VNode[] = [];
      for (let r = w.first; r < w.end; r++) {
        const cells = n.items.slice(r * n.columns, (r + 1) * n.columns).map((c) => h('td', undefined, undefined, c));
        body.push(h('tr', { 'data-row': String(r) }, cells));
      }
      const table = h('table', undefined, [
        ...(n.label ? [h('caption', undefined, undefined, n.label)] : []),
        h('tbody', undefined, body),
      ]);
      return h('div', decorate(item, ''), [...severityBadge(item), table, ...pager(panel, item)]);
    }
    default:
      return h('div', decorate(item, 'w-unsupported'), undefined, `unsupported widget kind ${n.kind_code}`);
  }
}

export function renderPanelView(panel: WidgetPanel): VNode {
  return h('section', { class: 'w-panel', 'data-panel-id': panel.id }, [
    h('header', { class: 'w-panel-header' }, [
      h('h3', undefined, undefined, panel.title),
      h('span', { class: 'w-panel-meta' }, undefined, `${panel.id} - ${panel.owner}${panel.extended ? ' - 1.1' : ''}`),
    ]),
    h('div', { class: 'w-panel-body' }, buildTree(panel.nodes).map((t) => renderNode(panel.id, t))),
  ]);
}

export function findNode(root: VNode, predicate: (v: VNode) => boolean): VNode | undefined {
  if (predicate(root)) return root;
  for (const child of root.children ?? []) {
    const found = findNode(child, predicate);
    if (found) return found;
  }
  return undefined;
}
