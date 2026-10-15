// Typed client for the scuba-hostd WebSocket bridge, with automatic reconnect.
//
//   const bridge = new HostdBridge();
//   bridge.connect();
//   const status = await bridge.call<SystemStatus>('system.status');
//   const off = bridge.on('kernel.log', (data) => console.log(data));
//
// Calls made while disconnected are queued and sent once the socket opens; each call rejects
// with BridgeError after requestTimeoutMs, or when the connection drops before it is answered.
import type { BridgeEvent, BridgeResponse, JsonValue, RequestId } from './protocol';

export type ConnectionState = 'idle' | 'connecting' | 'open' | 'closed';

export interface WebSocketLike {
  readyState: number;
  onopen: ((ev: unknown) => void) | null;
  onclose: ((ev: unknown) => void) | null;
  onerror: ((ev: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  send(data: string): void;
  close(code?: number, reason?: string): void;
}

export type WebSocketFactory = (url: string) => WebSocketLike;

export interface BridgeOptions {
  url?: string;
  createSocket?: WebSocketFactory;
  requestTimeoutMs?: number;
  reconnectInitialMs?: number;
  reconnectMaxMs?: number;
}

export class BridgeError extends Error {
  readonly code: number;
  readonly data?: JsonValue;

  constructor(code: number, message: string, data?: JsonValue) {
    super(message);
    this.name = 'BridgeError';
    this.code = code;
    this.data = data;
  }
}

export const ClientErrorCode = {
  Timeout: -1,
  Disconnected: -2,
  Closed: -3,
} as const;

const OPEN = 1;

interface Pending {
  resolve: (value: JsonValue) => void;
  reject: (err: BridgeError) => void;
  timer: ReturnType<typeof setTimeout>;
  text: string;
  sent: boolean;
}

export function defaultBridgeUrl(): string {
  const loc = (globalThis as { location?: { protocol: string; host: string } }).location;
  if (!loc) return 'ws://127.0.0.1:7878/ws';
  return `${loc.protocol === 'https:' ? 'wss' : 'ws'}://${loc.host}/ws`;
}

export class HostdBridge {
  private readonly url: string;
  private readonly createSocket: WebSocketFactory;
  private readonly requestTimeoutMs: number;
  private readonly reconnectInitialMs: number;
  private readonly reconnectMaxMs: number;

  private socket: WebSocketLike | null = null;
  private stateValue: ConnectionState = 'idle';
  private nextId = 1;
  private readonly pending = new Map<RequestId, Pending>();
  private readonly eventHandlers = new Map<string, Set<(data: JsonValue) => void>>();
  private readonly stateHandlers = new Set<(state: ConnectionState) => void>();
  private reconnectDelay: number;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private stopped = true;

  constructor(options: BridgeOptions = {}) {
    this.url = options.url ?? defaultBridgeUrl();
    this.createSocket = options.createSocket ?? ((url) => new WebSocket(url) as unknown as WebSocketLike);
    this.requestTimeoutMs = options.requestTimeoutMs ?? 10_000;
    this.reconnectInitialMs = options.reconnectInitialMs ?? 250;
    this.reconnectMaxMs = options.reconnectMaxMs ?? 5_000;
    this.reconnectDelay = this.reconnectInitialMs;
  }

  get state(): ConnectionState {
    return this.stateValue;
  }

  connect(): void {
    if (!this.stopped) return;
    this.stopped = false;
    this.open();
  }

  close(): void {
    this.stopped = true;
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    const socket = this.socket;
    this.socket = null;
    if (socket) {
      socket.onopen = socket.onclose = socket.onerror = null;
      socket.onmessage = null;
      socket.close(1000, 'client closed');
    }
    this.failAll(ClientErrorCode.Closed, 'bridge closed');
    this.setState('closed');
  }

  call<T = JsonValue>(method: string, params?: JsonValue): Promise<T> {
    const id = this.nextId++;
    const request: { id: number; method: string; params?: JsonValue } = { id, method };
    if (params !== undefined) request.params = params;
    const text = JSON.stringify(request);
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new BridgeError(ClientErrorCode.Timeout, `${method}: no answer after ${this.requestTimeoutMs} ms`));
      }, this.requestTimeoutMs);
      const entry: Pending = { resolve: resolve as (v: JsonValue) => void, reject, timer, text, sent: false };
      this.pending.set(id, entry);
      if (this.socket && this.socket.readyState === OPEN) this.send(entry);
    });
  }

  on(event: string, handler: (data: JsonValue) => void): () => void {
    let set = this.eventHandlers.get(event);
    if (!set) {
      set = new Set();
      this.eventHandlers.set(event, set);
    }
    set.add(handler);
    return () => {
      set.delete(handler);
    };
  }

  onStateChange(handler: (state: ConnectionState) => void): () => void {
    this.stateHandlers.add(handler);
    handler(this.stateValue);
    return () => {
      this.stateHandlers.delete(handler);
    };
  }

  private setState(next: ConnectionState): void {
    if (next === this.stateValue) return;
    this.stateValue = next;
    for (const h of this.stateHandlers) h(next);
  }

  private open(): void {
    this.setState('connecting');
    let socket: WebSocketLike;
    try {
      socket = this.createSocket(this.url);
    } catch {
      this.scheduleReconnect();
      return;
    }
    this.socket = socket;
    socket.onopen = () => {
      this.reconnectDelay = this.reconnectInitialMs;
      this.setState('open');
      for (const entry of this.pending.values()) if (!entry.sent) this.send(entry);
    };
    socket.onmessage = (ev) => this.handleMessage(ev.data);
    socket.onerror = () => {
      /* onclose follows and drives the reconnect */
    };
    socket.onclose = () => {
      if (this.socket !== socket) return;
      this.socket = null;
      for (const [id, entry] of this.pending) {
        if (!entry.sent) continue;
        clearTimeout(entry.timer);
        this.pending.delete(id);
        entry.reject(new BridgeError(ClientErrorCode.Disconnected, 'connection lost before the answer'));
      }
      this.setState('closed');
      this.scheduleReconnect();
    };
  }

  private scheduleReconnect(): void {
    if (this.stopped || this.reconnectTimer !== null) return;
    const delay = this.reconnectDelay;
    this.reconnectDelay = Math.min(this.reconnectDelay * 2, this.reconnectMaxMs);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      if (!this.stopped) this.open();
    }, delay);
  }

  private send(entry: Pending): void {
    entry.sent = true;
    this.socket?.send(entry.text);
  }

  private handleMessage(raw: unknown): void {
    if (typeof raw !== 'string') return;
    let msg: unknown;
    try {
      msg = JSON.parse(raw);
    } catch {
      return;
    }
    if (typeof msg !== 'object' || msg === null) return;
    if ('event' in msg && typeof (msg as BridgeEvent).event === 'string') {
      const ev = msg as BridgeEvent;
      const handlers = this.eventHandlers.get(ev.event);
      if (handlers) for (const h of [...handlers]) h(ev.data ?? null);
      return;
    }
    const response = msg as BridgeResponse;
    if (response.id === null || response.id === undefined) return;
    const entry = this.pending.get(response.id);
    if (!entry) return;
    clearTimeout(entry.timer);
    this.pending.delete(response.id);
    if (response.error) entry.reject(new BridgeError(response.error.code, response.error.message, response.error.data));
    else entry.resolve(response.result ?? null);
  }

  private failAll(code: number, message: string): void {
    for (const entry of this.pending.values()) {
      clearTimeout(entry.timer);
      entry.reject(new BridgeError(code, message));
    }
    this.pending.clear();
  }
}
