import type { JsonValue } from '../../protocol';
export type JsonObject = { [key: string]: JsonValue };

export interface WidgetNode {
  id: number;
  parent: number;
  kind: number;
  flags: number;
  label: string;
  text: string;
  value: number;
  minimum: number;
  maximum: number;
  index: number;
  columns: number;
  items: string[];
  tooltip: string;
  severity: number;
  firstRow: number;
  visibleRows: number;
}
export interface WidgetPanel { id: string; title: string; owner: string; nodes: WidgetNode[] }
export interface Peer {
  slot: number;
  pid: string;
  status: 'running' | 'failed';
  error?: string;
  active?: boolean;
  members?: number;
  port?: number;
  self?: string;
  reason?: string;
  ipcPrefix?: string;
  panels?: WidgetPanel[];
  plugins?: { id: string; state: string; reason: string }[];
  traffic?: {
    datagramsIn: number; datagramsOut: number; retransmits: number;
    sendDropped: number; inboxDropped: number; quotaRefused: number;
  };
  probe?: {
    reliableSent: number; unreliableSent: number;
    reliableReceived: number; unreliableReceived: number;
  };
}
export interface Sandbox { peers: Peer[] }
