import type { WidgetEventParams, WidgetKind, WidgetPanel, WidgetSeverity } from '../widgets/types';
import { flatten, type Design, type Kind } from './model';

const wireKinds = {
  Label: ['label', 1], Button: ['button', 2], Toggle: ['toggle', 3], Slider: ['slider', 4],
  Combo: ['combo', 5], List: ['list', 6], TextInput: ['text_input', 7], Table: ['table', 8],
  Row: ['row', 9], Column: ['column', 10],
} as const satisfies Record<Kind, readonly [WidgetKind, number]>;
const severities: readonly WidgetSeverity[] = ['none', 'info', 'warning', 'error'];

export function designSnapshot(design: Design): WidgetPanel {
  return {
    id: `${design.id}.main`, title: design.title, owner: 'designer', revision: 0, extended: true,
    nodes: flatten(design.nodes).map(({ node, id, parent }) => ({
      id, parent, kind: wireKinds[node.kind][0], kind_code: wireKinds[node.kind][1],
      disabled: node.disabled, password: node.password, label: node.label, text: node.text,
      value: node.value, minimum: node.minimum, maximum: node.maximum, index: node.index,
      items: [...node.items], columns: node.columns, extended: true,
      severity: severities[node.severity] ?? 'none', tooltip: node.tooltip,
      first_row: node.firstRow, visible_rows: node.visibleRows,
    })),
  };
}

export function applyPreviewEvent(design: Design, event: WidgetEventParams): void {
  const node = flatten(design.nodes).find(row => row.id === event.node_id)?.node;
  if (!node) return;
  switch (event.kind) {
    case 'toggle':
    case 'slider': node.value = event.value ?? node.value; break;
    case 'combo':
    case 'list': node.index = event.index ?? node.index; break;
    case 'text_input': node.text = event.text ?? node.text; break;
    case 'page': node.firstRow = event.row ?? node.firstRow; break;
    case 'button': break;
  }
}
