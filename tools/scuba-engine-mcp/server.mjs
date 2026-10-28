// SPDX-License-Identifier: GPL-3.0-only
import { pathToFileURL } from 'node:url';

export const PROTOCOL = '2025-11-25';
const MAX_FRAME = 64 * 1024;
const MAX_REPLY = 4 * 1024 * 1024;
const string = { type: 'string', minLength: 1, maxLength: 4096 };
const number = { type: 'number' };
const integer = { type: 'integer', minimum: 0, maximum: 0xffffffff };
const vector = n => ({ type: 'array', items: number, minItems: n, maxItems: n });
const object = (properties, required = Object.keys(properties)) => ({
  type: 'object', properties, required, additionalProperties: false,
});
const spec = (name, description, inputSchema, method, map, readOnlyHint = false) => ({
  name, description, inputSchema, method, map,
  annotations: { readOnlyHint, destructiveHint: !readOnlyHint, openWorldHint: false },
});
const widgetSchema = object({
  panel_id: string, node_id: { ...integer, minimum: 1 },
  kind: { type: 'string', enum: ['button', 'toggle', 'slider', 'combo', 'list', 'text_input', 'page'] },
  text: { type: 'string', maxLength: 255 }, value: number,
  index: { type: 'integer', minimum: -1, maximum: 0x7fffffff },
  row: { type: 'integer', minimum: 0, maximum: 0x7fffffff },
}, ['panel_id', 'node_id', 'kind']);

export const TOOLS = [
  spec('host_status', 'Inspect the local sandbox host; not a game-session result.', object({}),
    'system.status', () => ({}), true),
  spec('invoke_command', 'Invoke a registered trusted native plugin command in the local sandbox. May mutate state.',
    object({ name: string, args: { type: 'array', maxItems: 32,
      items: { type: ['string', 'number', 'boolean'] } } }, ['name']),
    'plugins.invoke', a => ({ name: a.name, args: a.args ?? [] })),
  spec('get_widget_panel', 'Read the retained widget tree by panel id.', object({ panel_id: string }),
    'widgets.get', a => ({ id: a.panel_id }), true),
  spec('send_widget_event', 'Submit a supported widget event; may invoke a mutating plugin callback.', widgetSchema,
    'widgets.event', ({ panel_id, ...rest }) => ({ panel: panel_id, ...rest })),
  spec('mock_move_camera', 'Move only the synthetic mock camera, not a live game camera.',
    object({ pos: vector(3), rot: vector(4) }, ['pos']), 'mock_game.camera.move', a => a),
  spec('mock_set_service', 'Publish or withdraw a synthetic game service in the sandbox.',
    object({ service: string, published: { type: 'boolean' } }),
    'mock_game.services.set', a => ({ name: a.service, published: a.published })),
  spec('convert_asset', 'Convert a local asset using hostd bounded readers; may write hostd conversion output.',
    object({ path: string }), 'assets.convert', a => a),
  spec('mock_get_state', 'Read the synthetic sandbox scene state; not a game-session result.', object({}),
    'mock_game.state', () => ({}), true),
  spec('mock_list_services', 'List synthetic game services and their published flags.', object({}),
    'mock_game.services.list', () => ({}), true),
  spec('mock_spawn_entity', 'Spawn a synthetic entity in the sandbox scene.',
    object({ class: string, zone: integer, pos: vector(3), actor: { type: 'boolean' } }, ['class', 'zone']),
    'mock_game.entity.spawn', a => a),
  spec('mock_move_entity', 'Move a synthetic entity to a position in a streamed zone.',
    object({ id: integer, zone: integer, pos: vector(3) }, ['id', 'pos']),
    'mock_game.entity.move', a => a),
  spec('mock_remove_entity', 'Remove a synthetic entity (never the player).',
    object({ id: integer }, ['id']), 'mock_game.entity.remove', a => a),
  spec('mock_move_player', 'Move the synthetic player; camera follows only when requested.',
    object({ pos: vector(3), zone: integer, rot: vector(4), camera: { type: 'boolean' } }, ['pos']),
    'mock_game.player.move', a => a),
  spec('mock_set_capability', 'Set one synthetic game capability ready flag.',
    object({ name: string, ready: { type: 'boolean' } }),
    'mock_game.caps.set', a => a),
  spec('mock_apply_caps_profile', 'Apply a synthetic capability profile: all, game_pack or none.',
    object({ name: { type: 'string', enum: ['all', 'game_pack', 'none'] } }),
    'mock_game.caps.profile', a => a),
  spec('mock_reset', 'Reset the synthetic sandbox scene to defaults.', object({}),
    'mock_game.reset', () => ({})),
];

function record(value) { return value !== null && typeof value === 'object' && !Array.isArray(value); }
export class RpcError extends Error {
  constructor(code, message) { super(message); this.code = code; }
}
function validate(value, schema, path = 'arguments') {
  const types = Array.isArray(schema.type) ? schema.type : [schema.type];
  const type = Array.isArray(value) ? 'array' : value === null ? 'null' : typeof value;
  if (!types.includes(type) && !(types.includes('integer') && Number.isSafeInteger(value)))
    throw new RpcError(-32602, `${path}: wrong type`);
  if (schema.enum && !schema.enum.includes(value)) throw new RpcError(-32602, `${path}: unsupported value`);
  if (type === 'number' && (!Number.isFinite(value) || value < (schema.minimum ?? -Infinity) ||
      value > (schema.maximum ?? Infinity))) throw new RpcError(-32602, `${path}: out of range`);
  if (type === 'string' && (value.includes('\0') || value.length < (schema.minLength ?? 0) ||
      Buffer.byteLength(value) > (schema.maxLength ?? MAX_FRAME)))
    throw new RpcError(-32602, `${path}: invalid string`);
  if (type === 'array') {
    if (value.length < (schema.minItems ?? 0) || value.length > (schema.maxItems ?? MAX_FRAME))
      throw new RpcError(-32602, `${path}: invalid length`);
    value.forEach((v, i) => validate(v, schema.items, `${path}[${i}]`));
  }
  if (type === 'object') {
    for (const key of schema.required ?? [])
      if (!Object.hasOwn(value, key)) throw new RpcError(-32602, `${path}.${key}: required`);
    for (const [key, v] of Object.entries(value)) {
      if (!Object.hasOwn(schema.properties, key)) throw new RpcError(-32602, `${path}.${key}: unknown field`);
      validate(v, schema.properties[key], `${path}.${key}`);
    }
  }
}

export function endpoint(value) {
  const url = new URL(value);
  // A numeric IPv4 loopback avoids DNS rebinding and never targets a LAN host.
  if (url.protocol !== 'ws:' || url.hostname !== '127.0.0.1' || !url.port || url.pathname !== '/ws' ||
      url.username || url.password || url.search || url.hash)
    throw new RpcError(-32602, 'Host endpoint must be ws://127.0.0.1:<port>/ws');
  return url.href;
}

export class Bridge {
  constructor(url, timeoutMs = 10000) {
    this.url = endpoint(url); this.timeoutMs = timeoutMs;
    this.socket = null; this.pending = new Map(); this.sequence = 0; this.opening = null; this.failed = false;
  }
  fail(message) {
    this.failed = true;
    for (const p of this.pending.values()) { clearTimeout(p.timer); p.reject(new RpcError(-32000, message)); }
    this.pending.clear();
    this.socket?.close();
  }
  async open() {
    if (this.failed) throw new RpcError(-32000, 'Bridge closed; restart MCP server to reconnect');
    if (this.opening) return this.opening;
    this.opening = new Promise((resolve, reject) => {
      const socket = new WebSocket(this.url); this.socket = socket;
      const timer = setTimeout(() => { this.fail('Host connection timed out'); reject(new RpcError(-32000, 'Host connection timed out')); }, this.timeoutMs);
      socket.addEventListener('open', () => { clearTimeout(timer); resolve(); }, { once: true });
      const disconnected = () => {
        clearTimeout(timer); this.fail('Host disconnected'); reject(new RpcError(-32000, 'Host disconnected'));
      };
      socket.addEventListener('error', disconnected, { once: true });
      socket.addEventListener('close', disconnected, { once: true });
      socket.addEventListener('message', event => {
        try {
          if (typeof event.data !== 'string' || Buffer.byteLength(event.data) > MAX_REPLY)
            throw new RpcError(-32000, 'Invalid host frame');
          const msg = JSON.parse(event.data);
          if (!record(msg)) throw new RpcError(-32000, 'Invalid host envelope');
          if (typeof msg.event === 'string' && !Object.hasOwn(msg, 'id')) return;
          if (!Number.isSafeInteger(msg.id) || (!Object.hasOwn(msg, 'result') && !record(msg.error)))
            throw new RpcError(-32000, 'Invalid host reply');
          const p = this.pending.get(msg.id);
          if (!p) return;
          this.pending.delete(msg.id); clearTimeout(p.timer);
          if (record(msg.error)) p.reject(new RpcError(msg.error.code, String(msg.error.message)));
          else p.resolve(msg.result);
        } catch (error) { this.fail(error instanceof Error ? error.message : 'Invalid host reply'); }
      });
    });
    return this.opening;
  }
  async call(method, params) {
    await this.open();
    const id = ++this.sequence;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => this.fail('Host call timed out; outcome unknown, not retried'), this.timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      try { this.socket.send(JSON.stringify({ id, method, params })); }
      catch (error) { this.fail(error instanceof Error ? error.message : 'Host send failed'); }
    });
  }
  close() { this.fail('MCP server stopped'); }
}

export class Server {
  constructor(bridge) { this.bridge = bridge; this.state = 'new'; }
  async handle(request) {
    let id = null;
    try {
      if (!record(request) || request.jsonrpc !== '2.0' || typeof request.method !== 'string' ||
          (Object.hasOwn(request, 'id') && typeof request.id !== 'string' && !Number.isSafeInteger(request.id)))
        throw new RpcError(-32600, 'Invalid JSON-RPC request');
      if (!Object.hasOwn(request, 'id')) {
        if (request.method === 'notifications/initialized' && this.state === 'initializing') this.state = 'ready';
        return null;
      }
      id = request.id;
      const params = request.params === undefined ? {} : request.params;
      if (!record(params)) throw new RpcError(-32602, 'params must be an object');
      let result;
      switch (request.method) {
        case 'initialize':
          if (this.state !== 'new') throw new RpcError(-32600, 'Already initialized');
          if (typeof params.protocolVersion !== 'string' || !record(params.capabilities) ||
              !record(params.clientInfo) || typeof params.clientInfo.name !== 'string' ||
              typeof params.clientInfo.version !== 'string') throw new RpcError(-32602, 'Invalid initialize params');
          this.state = 'initializing';
          result = { protocolVersion: PROTOCOL, capabilities: { tools: {} },
            serverInfo: { name: 'scuba-engine-mcp', version: '0.2.0' } };
          break;
        case 'ping': result = {}; break;
        case 'tools/list':
          if (this.state !== 'ready') throw new RpcError(-32002, 'Initialization required');
          validate(params, object({}));
          result = { tools: TOOLS.map(({ method, map, ...tool }) => tool) };
          break;
        case 'tools/call': {
          if (this.state !== 'ready') throw new RpcError(-32002, 'Initialization required');
          if (typeof params.name !== 'string' || (params.arguments !== undefined && !record(params.arguments)))
            throw new RpcError(-32602, 'Expected tool name and arguments object');
          if (Object.keys(params).some(key => !['name', 'arguments', '_meta'].includes(key)))
            throw new RpcError(-32602, 'Unknown tool-call field');
          const tool = TOOLS.find(t => t.name === params.name);
          if (!tool) throw new RpcError(-32602, 'Unknown tool');
          try {
            const args = params.arguments ?? {};
            validate(args, tool.inputSchema);
            if (tool.name === 'send_widget_event') {
              for (const [field, kinds] of Object.entries({ value: ['toggle', 'slider'],
                index: ['combo', 'list'], text: ['text_input'], row: ['page'] })) {
                if (kinds.includes(args.kind) !== Object.hasOwn(args, field))
                  throw new RpcError(-32602, `Widget ${field} does not match event kind`);
              }
            }
            const value = await this.bridge.call(tool.method, tool.map(args));
            const failed = record(value) && typeof value.result === 'string' && value.result !== 'OK';
            result = { content: [{ type: 'text', text: JSON.stringify(value) }], isError: failed };
          } catch (error) {
            result = { content: [{ type: 'text', text: error instanceof Error ? error.message : 'Tool failed' }], isError: true };
          }
          break;
        }
        default: throw new RpcError(-32601, 'Method not found');
      }
      return { jsonrpc: '2.0', id, result };
    } catch (error) {
      return { jsonrpc: '2.0', id, error: { code: error instanceof RpcError ? error.code : -32603,
        message: error instanceof Error ? error.message : 'Internal error' } };
    }
  }
}

export async function stdio(bridge, input = process.stdin, output = process.stdout) {
  const server = new Server(bridge);
  let pending = Buffer.alloc(0);
  const emit = value => {
    if (value === null) return;
    let text = JSON.stringify(value);
    if (Buffer.byteLength(text) > MAX_REPLY) text = JSON.stringify({ jsonrpc: '2.0', id: value.id,
      error: { code: -32603, message: 'Response exceeds limit' } });
    output.write(text + '\n');
  };
  try {
    for await (const chunk of input) {
      pending = Buffer.concat([pending, Buffer.from(chunk)]);
      let newline;
      while ((newline = pending.indexOf(10)) !== -1) {
        const line = pending.subarray(0, newline); pending = pending.subarray(newline + 1);
        if (line.length > MAX_FRAME) throw new RpcError(-32600, 'Input frame exceeds 64 KiB');
        let request;
        try { request = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(line)); }
        catch { emit({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'Parse error' } }); continue; }
        emit(await server.handle(request));
      }
      if (pending.length > MAX_FRAME) throw new RpcError(-32600, 'Input frame exceeds 64 KiB');
    }
    if (pending.length) emit({ jsonrpc: '2.0', id: null, error: { code: -32700, message: 'Unterminated input frame' } });
  } finally { bridge.close(); }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    if (process.argv.length !== 4 || process.argv[2] !== '--host')
      throw new RpcError(-32602, 'usage: node server.mjs --host ws://127.0.0.1:<port>/ws');
    await stdio(new Bridge(process.argv[3]));
  } catch (error) {
    process.stderr.write((error instanceof Error ? error.message : 'Server failed') + '\n');
    process.exitCode = 1;
  }
}
