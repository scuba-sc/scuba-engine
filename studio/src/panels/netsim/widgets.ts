import type { JsonObject, WidgetPanel } from './types';

export function element<K extends keyof HTMLElementTagNameMap>(tag: K, text?: string, className?: string): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (text !== undefined) node.textContent = text;
  if (className) node.className = className;
  return node;
}

export function renderWidgets(panel: WidgetPanel, send: (event: JsonObject) => void): HTMLElement {
  const root = element('section', undefined, 'ns-widget');
  root.append(element('h4', panel.title));
  const parents = new Map<number, HTMLElement>([[0, root]]);
  for (const node of panel.nodes) {
    const group = element('div', undefined, `ns-node ns-kind-${node.kind}`);
    group.title = node.tooltip;
    if (node.severity) group.dataset.severity = String(node.severity);
    const dispatch = (payload: JsonObject = {}) => send({ panel: panel.id, node: node.id, kind: node.kind, ...payload });
    const disabled = (node.flags & 1) !== 0;
    switch (node.kind) {
      case 1:
        group.append(element('p', node.label));
        break;
      case 2: {
        const button = element('button', node.label);
        button.disabled = disabled;
        button.onclick = () => dispatch();
        group.append(button);
        break;
      }
      case 3:
      case 4:
      case 7: {
        const label = element('label');
        const input = element('input');
        input.disabled = disabled;
        if (node.kind === 3) { input.type = 'checkbox'; input.checked = node.value !== 0; }
        if (node.kind === 4) {
          input.type = 'range'; input.min = String(node.minimum); input.max = String(node.maximum);
          input.step = 'any'; input.value = String(node.value);
        }
        if (node.kind === 7) {
          input.type = node.flags & 2 ? 'password' : 'text';
          input.value = node.text;
          input.maxLength = 255;
        }
        label.append(element('span', node.label), input);
        input.onchange = () => dispatch(node.kind === 7 ? { text: input.value }
          : { value: node.kind === 3 ? Number(input.checked) : Number(input.value) });
        group.append(label);
        break;
      }
      case 5:
      case 6: {
        const label = element('label');
        const select = element('select');
        select.disabled = disabled;
        if (node.kind === 6) select.size = Math.min(6, Math.max(2, node.visibleRows || node.items.length));
        const end = node.visibleRows ? node.firstRow + node.visibleRows : node.items.length;
        node.items.slice(node.firstRow, end).forEach((item, offset) => {
          const option = element('option', item);
          option.value = String(node.firstRow + offset);
          select.append(option);
        });
        select.value = String(node.index);
        select.onchange = () => dispatch({ index: Number(select.value) });
        label.append(element('span', node.label), select);
        group.append(label);
        break;
      }
      case 8: {
        const table = element('table');
        table.setAttribute('aria-label', node.label);
        const start = node.firstRow * node.columns;
        const end = node.visibleRows ? Math.min(node.items.length, start + node.visibleRows * node.columns) : node.items.length;
        for (let offset = start; offset < end; offset += node.columns) {
          const row = element('tr');
          for (const item of node.items.slice(offset, offset + node.columns)) row.append(element('td', item));
          table.append(row);
        }
        group.append(element('p', node.label), table);
        break;
      }
      case 9:
      case 10:
        parents.set(node.id, group);
        break;
      default:
        group.append(element('p', `Unsupported widget kind ${node.kind}`));
    }
    if ((node.kind === 6 || node.kind === 8) && node.visibleRows) {
      const pages = element('div', undefined, 'ns-actions');
      const rows = node.kind === 8 ? node.items.length / node.columns : node.items.length;
      for (const [title, next] of [['Previous rows', Math.max(0, node.firstRow - node.visibleRows)], ['Next rows', node.firstRow + node.visibleRows]] as const) {
        const button = element('button', title);
        button.disabled = disabled || next === node.firstRow || next >= rows;
        button.onclick = () => send({ panel: panel.id, node: node.id, kind: 256, row: next });
        pages.append(button);
      }
      group.append(pages);
    }
    (parents.get(node.parent) ?? root).append(group);
  }
  return root;
}
