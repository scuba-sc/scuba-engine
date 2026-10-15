// Wire shapes of the widgets.* bridge methods and events (docs/protocol.md, "Widgets").
import type { JsonValue } from '../../protocol';

export type WidgetKind =
  | 'label'
  | 'button'
  | 'toggle'
  | 'slider'
  | 'combo'
  | 'list'
  | 'text_input'
  | 'table'
  | 'row'
  | 'column';

export type WidgetSeverity = 'none' | 'info' | 'warning' | 'error';

export interface WidgetNode {
  id: number;
  parent: number;
  kind: WidgetKind | '';
  kind_code: number;
  disabled: boolean;
  password: boolean;
  label: string;
  text: string;
  value: number;
  minimum: number;
  maximum: number;
  index: number;
  items: string[];
  columns: number;
  extended: boolean;
  severity: WidgetSeverity;
  tooltip: string;
  first_row: number;
  visible_rows: number;
}

export interface WidgetPanel {
  id: string;
  title: string;
  owner: string;
  revision: number;
  extended: boolean;
  nodes: WidgetNode[];
}

export interface WidgetCapabilities {
  service: string;
  version: string;
  features: string[];
  limits: Record<string, number>;
  events: string[];
}

export interface WidgetsListResult {
  started: boolean;
  capabilities: WidgetCapabilities;
  panels: WidgetPanel[];
}

export interface WidgetEventResult {
  result: string;
  revision: number;
}

export type WidgetEventKind = 'button' | 'toggle' | 'slider' | 'combo' | 'list' | 'text_input' | 'page';

export interface WidgetEventParams {
  panel: string;
  node_id: number;
  kind: WidgetEventKind;
  value?: number;
  index?: number;
  text?: string;
  row?: number;
}

export function eventToJson(e: WidgetEventParams): JsonValue {
  const out: { [key: string]: JsonValue } = { panel: e.panel, node_id: e.node_id, kind: e.kind };
  if (e.value !== undefined) out.value = e.value;
  if (e.index !== undefined) out.index = e.index;
  if (e.text !== undefined) out.text = e.text;
  if (e.row !== undefined) out.row = e.row;
  return out;
}

function isObject(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

export function isWidgetPanel(v: unknown): v is WidgetPanel {
  return isObject(v) && typeof v.id === 'string' && typeof v.title === 'string' && Array.isArray(v.nodes);
}
