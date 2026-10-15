import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { BridgeError, ClientErrorCode, HostdBridge, type WebSocketLike } from './bridge';

class FakeSocket implements WebSocketLike {
  readyState = 0;
  onopen: ((ev: unknown) => void) | null = null;
  onclose: ((ev: unknown) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  sent: string[] = [];
  closed = false;

  constructor(readonly url: string) {}

  send(data: string): void {
    if (this.readyState !== 1) throw new Error('send before open');
    this.sent.push(data);
  }
  close(): void {
    this.closed = true;
    this.readyState = 3;
  }
  serverOpen(): void {
    this.readyState = 1;
    this.onopen?.({});
  }
  serverMessage(msg: unknown): void {
    this.onmessage?.({ data: JSON.stringify(msg) });
  }
  serverDrop(): void {
    this.readyState = 3;
    this.onclose?.({});
  }
  lastRequest(): { id: number; method: string; params?: unknown } {
    return JSON.parse(this.sent[this.sent.length - 1]!);
  }
}

function setup(options: { requestTimeoutMs?: number } = {}) {
  const sockets: FakeSocket[] = [];
  const bridge = new HostdBridge({
    url: 'ws://test/ws',
    createSocket: (url) => {
      const s = new FakeSocket(url);
      sockets.push(s);
      return s;
    },
    reconnectInitialMs: 100,
    reconnectMaxMs: 400,
    ...options,
  });
  return { bridge, sockets };
}

describe('HostdBridge', () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  it('sends a request envelope and resolves with the result', async () => {
    const { bridge, sockets } = setup();
    bridge.connect();
    sockets[0]!.serverOpen();
    const reply = bridge.call<{ pong: boolean }>('system.ping', { echo: 'hi' });
    const req = sockets[0]!.lastRequest();
    expect(req).toEqual({ id: 1, method: 'system.ping', params: { echo: 'hi' } });
    sockets[0]!.serverMessage({ id: req.id, result: { pong: true } });
    await expect(reply).resolves.toEqual({ pong: true });
  });

  it('rejects with BridgeError carrying the server error', async () => {
    const { bridge, sockets } = setup();
    bridge.connect();
    sockets[0]!.serverOpen();
    const reply = bridge.call('nope.nothing');
    sockets[0]!.serverMessage({ id: 1, error: { code: -32601, message: 'no such method: nope.nothing' } });
    await expect(reply).rejects.toMatchObject({ name: 'BridgeError', code: -32601 });
  });

  it('queues calls made before the socket opens', async () => {
    const { bridge, sockets } = setup();
    bridge.connect();
    const reply = bridge.call('system.status');
    expect(sockets[0]!.sent).toHaveLength(0);
    sockets[0]!.serverOpen();
    expect(sockets[0]!.sent).toHaveLength(1);
    sockets[0]!.serverMessage({ id: 1, result: 'ok' });
    await expect(reply).resolves.toBe('ok');
  });

  it('times out unanswered calls', async () => {
    const { bridge, sockets } = setup({ requestTimeoutMs: 1000 });
    bridge.connect();
    sockets[0]!.serverOpen();
    const reply = bridge.call('system.ping');
    const check = expect(reply).rejects.toMatchObject({ code: ClientErrorCode.Timeout });
    vi.advanceTimersByTime(1000);
    await check;
  });

  it('dispatches server events to subscribers until unsubscribed', () => {
    const { bridge, sockets } = setup();
    bridge.connect();
    sockets[0]!.serverOpen();
    const seen: unknown[] = [];
    const off = bridge.on('kernel.log', (data) => seen.push(data));
    sockets[0]!.serverMessage({ event: 'kernel.log', data: { line: 'a' } });
    sockets[0]!.serverMessage({ event: 'other', data: 1 });
    off();
    sockets[0]!.serverMessage({ event: 'kernel.log', data: { line: 'b' } });
    expect(seen).toEqual([{ line: 'a' }]);
  });

  it('reconnects with backoff and fails in-flight calls on drop', async () => {
    const { bridge, sockets } = setup();
    const states: string[] = [];
    bridge.onStateChange((s) => states.push(s));
    bridge.connect();
    sockets[0]!.serverOpen();
    const inflight = bridge.call('system.ping');
    const check = expect(inflight).rejects.toBeInstanceOf(BridgeError);
    sockets[0]!.serverDrop();
    await check;
    expect(bridge.state).toBe('closed');

    vi.advanceTimersByTime(99);
    expect(sockets).toHaveLength(1);
    vi.advanceTimersByTime(1);
    expect(sockets).toHaveLength(2);
    sockets[1]!.serverDrop();
    vi.advanceTimersByTime(199);
    expect(sockets).toHaveLength(2);
    vi.advanceTimersByTime(1);
    expect(sockets).toHaveLength(3);
    sockets[2]!.serverOpen();
    expect(bridge.state).toBe('open');
    expect(states).toEqual(['idle', 'connecting', 'open', 'closed', 'connecting', 'closed', 'connecting', 'open']);

    const reply = bridge.call('system.ping');
    sockets[2]!.serverMessage({ id: sockets[2]!.lastRequest().id, result: { pong: true } });
    await expect(reply).resolves.toEqual({ pong: true });
  });

  it('stops reconnecting after close()', () => {
    const { bridge, sockets } = setup();
    bridge.connect();
    sockets[0]!.serverOpen();
    bridge.close();
    expect(sockets[0]!.closed).toBe(true);
    vi.advanceTimersByTime(10_000);
    expect(sockets).toHaveLength(1);
    expect(bridge.state).toBe('closed');
  });
});
