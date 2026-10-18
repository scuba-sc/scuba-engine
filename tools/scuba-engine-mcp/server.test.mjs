// SPDX-License-Identifier: GPL-3.0-only
import { test, expect } from 'bun:test';
import { Readable, Writable } from 'node:stream';
import { Bridge, Server, TOOLS, endpoint, stdio, PROTOCOL } from './server.mjs';

const request = (method, params = {}, id = 1) => ({ jsonrpc: '2.0', id, method, params });
async function initialized(bridge) {
  const server = new Server(bridge);
  const reply = await server.handle(request('initialize', {
    protocolVersion: PROTOCOL, capabilities: {}, clientInfo: { name: 'test', version: '1' },
  }));
  expect(reply.result.protocolVersion).toBe(PROTOCOL);
  await server.handle({ jsonrpc: '2.0', method: 'notifications/initialized' });
  return server;
}

test('requires initialized notification before exposing tools', async () => {
  const server = new Server({ call() { throw new Error('unexpected forward'); } });
  expect((await server.handle(request('tools/list'))).error.code).toBe(-32002);
  expect((await server.handle(request('initialize', {}))).error.code).toBe(-32602);
  expect((await server.handle(request('anything'))).error.code).toBe(-32601);
  expect((await server.handle({ id: 1, method: 'ping' })).error.code).toBe(-32600);
  expect((await server.handle(request('ping', {}, true))).error.code).toBe(-32600);
  expect((await server.handle(request('ping', null))).error.code).toBe(-32602);
});

test('advertises explicit tools with object schemas and mutation annotations', async () => {
  const server = await initialized({});
  const tools = (await server.handle(request('tools/list'))).result.tools;
  expect(tools).toHaveLength(8);
  expect(tools.every(t => t.inputSchema.additionalProperties === false && !('map' in t) && !('method' in t))).toBe(true);
  expect(tools.find(t => t.name === 'host_status').annotations.readOnlyHint).toBe(true);
  expect(tools.find(t => t.name === 'plugin_commands').annotations.readOnlyHint).toBe(true);
  expect(tools.find(t => t.name === 'invoke_command').annotations.destructiveHint).toBe(true);
});

test('maps every approved tool to the actual hostd field names', async () => {
  const calls = [];
  const server = await initialized({ async call(method, params) { calls.push({ method, params }); return { result: 'OK' }; } });
  const samples = [
    ['host_status', {}, 'system.status', {}],
    ['plugin_commands', {}, 'plugins.commands', {}],
    ['invoke_command', { name: 'mp_chat.status' }, 'plugins.invoke', { name: 'mp_chat.status', args: [] }],
    ['get_widget_panel', { panel_id: 'mp_chat.main' }, 'widgets.get', { id: 'mp_chat.main' }],
    ['send_widget_event', { panel_id: 'mp_chat.main', node_id: 3, kind: 'button' }, 'widgets.event',
      { panel: 'mp_chat.main', node_id: 3, kind: 'button' }],
    ['mock_move_camera', { pos: [1, 2, 3], rot: [0, 0, 0, 1] }, 'mock_game.camera.move', { pos: [1, 2, 3], rot: [0, 0, 0, 1] }],
    ['mock_set_service', { service: 'game.actors', published: false }, 'mock_game.services.set', { name: 'game.actors', published: false }],
    ['convert_asset', { path: 'fixture.obj' }, 'assets.convert', { path: 'fixture.obj' }],
  ];
  for (const [name, args, method, params] of samples) {
    const reply = await server.handle(request('tools/call', { name, arguments: args }));
    expect(reply.result.isError).toBe(false);
    expect(calls.at(-1)).toEqual({ method, params });
  }
});

test('rejects invalid input without making any bridge call', async () => {
  let forwards = 0;
  const server = await initialized({ async call() { forwards++; return {}; } });
  const invalid = [
    ['host_status', { ignored: 1 }], ['plugin_commands', { ignored: 1 }], ['invoke_command', { name: 'x', args: [null] }],
    ['invoke_command', { name: 'bad\0name' }], ['invoke_command', { name: 'x', args: [{}] }],
    ['get_widget_panel', {}], ['mock_set_service', { service: 'game.actors', published: 'false' }],
    ['mock_move_camera', { pos: [1, 2] }], ['mock_move_camera', { pos: [1, NaN, 3] }],
    ['send_widget_event', { panel_id: 'p', node_id: 0, kind: 'button' }],
    ['send_widget_event', { panel_id: 'p', node_id: 1.5, kind: 'button' }],
    ['send_widget_event', { panel_id: 'p', node_id: 1, kind: 'toggle' }],
    ['send_widget_event', { panel_id: 'p', node_id: 1, kind: 'button', text: 'x' }],
    ['send_widget_event', { panel_id: 'p', node_id: 1, kind: 'tab' }],
  ];
  for (const [name, args] of invalid)
    expect((await server.handle(request('tools/call', { name, arguments: args }))).result.isError).toBe(true);
  expect(forwards).toBe(0);
  expect((await server.handle(request('tools/call', { name: 'unknown' }))).error.code).toBe(-32602);
  expect((await server.handle(request('tools/call', { name: 'host_status', extra: true }))).error.code).toBe(-32602);
});

test('surfaces native refusal and bridge errors as tool errors', async () => {
  const denied = await initialized({ async call() { return { result: 'UNAVAILABLE', reply: '' }; } });
  expect((await denied.handle(request('tools/call', { name: 'host_status' }))).result.isError).toBe(true);
  const broken = await initialized({ async call() { throw new Error('bridge unavailable'); } });
  expect((await broken.handle(request('tools/call', { name: 'host_status' }))).result.isError).toBe(true);
});

test('only accepts numeric IPv4 loopback without credentials or alternate paths', () => {
  expect(endpoint('ws://127.0.0.1:7878/ws')).toBe('ws://127.0.0.1:7878/ws');
  for (const url of ['ws://localhost:1/ws', 'ws://192.168.1.1:1/ws', 'wss://127.0.0.1:1/ws',
    'ws://user:pass@127.0.0.1:1/ws', 'ws://127.0.0.1:1/other', 'ws://127.0.0.1:1/ws?x=1'])
    expect(() => endpoint(url)).toThrow();
});

test('WebSocket bridge correlates responses and ignores host event frames', async () => {
  const host = Bun.serve({ port: 0, hostname: '127.0.0.1',
    fetch(req, server) { if (server.upgrade(req)) return; return new Response('', { status: 400 }); },
    websocket: { message(ws, text) { const req = JSON.parse(String(text));
      ws.send(JSON.stringify({ event: 'tick', data: {} }));
      ws.send(JSON.stringify({ id: req.id, result: req.params })); } },
  });
  const bridge = new Bridge(`ws://127.0.0.1:${host.port}/ws`);
  try { expect(await Promise.all([bridge.call('a.b', { value: 1 }), bridge.call('a.c', { value: 2 })]))
    .toEqual([{ value: 1 }, { value: 2 }]); }
  finally { bridge.close(); host.stop(true); }
});

test('disconnect rejects an in-flight call without reconnecting or replaying it', async () => {
  let received = 0;
  const host = Bun.serve({ port: 0, hostname: '127.0.0.1',
    fetch(req, server) { if (server.upgrade(req)) return; return new Response('', { status: 400 }); },
    websocket: { message(ws) { received++; ws.close(); } },
  });
  const bridge = new Bridge(`ws://127.0.0.1:${host.port}/ws`);
  try {
    await expect(bridge.call('a.b', {})).rejects.toThrow();
    await expect(bridge.call('a.b', {})).rejects.toThrow();
    expect(received).toBe(1);
  } finally { bridge.close(); host.stop(true); }
});

test('stdio recovers from invalid JSON and preserves request ids with JSON-only output', async () => {
  let output = '', closed = false;
  const sink = new Writable({ write(chunk, encoding, done) { output += chunk; done(); } });
  await stdio({ close() { closed = true; } }, Readable.from([
    'not-json\n', JSON.stringify(request('ping', {}, 'exact-id')) + '\n',
  ]), sink);
  const replies = output.trim().split('\n').map(line => JSON.parse(line));
  expect(replies[0].error.code).toBe(-32700);
  expect(replies[1]).toEqual({ jsonrpc: '2.0', id: 'exact-id', result: {} });
  expect(closed).toBe(true);
});
