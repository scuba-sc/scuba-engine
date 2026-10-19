// SPDX-License-Identifier: GPL-3.0-only
// Real stdio MCP -> WebSocket -> native hostd integration. All fixtures synthetic.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createInterface } from 'node:readline';
import { connect } from 'node:net';

const [hostArg, pluginArg] = process.argv.slice(2);
assert(hostArg && pluginArg, 'usage: node integration.mjs HOSTD PLUGIN_ROOT');
const work = await mkdtemp(join(tmpdir(), 'studio-mcp-integration-'));
const fixture = join(work, 'triangle.obj');
await writeFile(fixture, 'v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n');
const host = spawn(resolve(hostArg), ['--port', '0', '--bind', '127.0.0.1',
  '--plugins', resolve(pluginArg), '--data', join(work, 'data'), '--test-shutdown'],
{ cwd: work, stdio: ['ignore', 'pipe', 'pipe'] });
const deadline = AbortSignal.timeout(45_000);
const hostExit = once(host, 'exit', { signal: deadline });
const hostLines = createInterface({ input: host.stdout });
let hostLog = '', serverLog = '', server, serverExit, serverLines, admin, port;
host.stderr.on('data', chunk => { hostLog += chunk; });
const ready = new Promise((accept, reject) => {
  hostLines.on('line', line => {
    hostLog += line + '\n';
    const match = /scuba-hostd: listening port=(\d+)/.exec(line);
    if (match) accept(Number(match[1]));
  });
  host.once('error', reject);
  host.once('exit', code => reject(new Error(`host exited before ready: ${code}`)));
  deadline.addEventListener('abort', () => reject(deadline.reason), { once: true });
});
const pending = new Map();
const transcript = [];
let sequence = 0;
function request(method, params = {}) {
  const id = ++sequence;
  const value = { jsonrpc: '2.0', id, method, params };
  transcript.push({ sent: value });
  return new Promise((accept, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${method} timed out`)); }, 10_000);
    pending.set(id, { accept, reject, timer });
    server.stdin.write(JSON.stringify(value) + '\n');
  });
}
async function call(name, args = {}) {
  const reply = await request('tools/call', { name, arguments: args });
  assert(!reply.error, JSON.stringify(reply));
  assert.equal(reply.result.isError, false, JSON.stringify(reply));
  return JSON.parse(reply.result.content[0].text);
}
let adminSequence = 0;
function adminCall(method, params = {}) {
  const id = ++adminSequence;
  return new Promise((accept, reject) => {
    const timer = setTimeout(() => reject(new Error('admin call timeout')), 10_000);
    function message(event) {
      const value = JSON.parse(event.data);
      if (value.id !== id) return;
      clearTimeout(timer); admin.removeEventListener('message', message);
      if (value.error) reject(new Error(JSON.stringify(value.error))); else accept(value.result);
    }
    admin.addEventListener('message', message);
    admin.send(JSON.stringify({ id, method, params }));
  });
}
try {
  port = await ready;
  const url = `ws://127.0.0.1:${port}/ws`;
  server = spawn('node', [join(dirname(fileURLToPath(import.meta.url)), 'server.mjs'), '--host', url],
    { cwd: work, stdio: ['pipe', 'pipe', 'pipe'] });
  serverExit = once(server, 'exit', { signal: deadline });
  server.stderr.on('data', chunk => { serverLog += chunk; });
  serverLines = createInterface({ input: server.stdout });
  serverLines.on('line', line => {
    const value = JSON.parse(line); // Non-JSON stdout fails the integration.
    transcript.push({ received: value });
    const entry = pending.get(value.id);
    if (entry) { pending.delete(value.id); clearTimeout(entry.timer); entry.accept(value); }
  });
  const initial = await request('initialize', { protocolVersion: '2025-11-25', capabilities: {},
    clientInfo: { name: 'studio-mcp-integration', version: '1' } });
  assert.equal(initial.result.protocolVersion, '2025-11-25');
  server.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
  assert.equal((await request('tools/list')).result.tools.length, 15);
  const commands = await call('plugin_commands');
  assert(Array.isArray(commands) && commands.some(command => command.name === 'mp_chat.status'));
  for (const command of commands) {
    assert.equal(typeof command.ready, 'boolean');
    assert(Array.isArray(command.args));
    assert(command.args.every(arg => ['integer', 'number', 'string', 'boolean'].includes(arg.type)));
  }
  const status = await call('host_status');
  assert.equal(status.name, 'scuba-hostd');
  assert(status.kernel.plugins.some(p => p.id === 'diegetic_asop' && p.state === 'loaded'));
  assert.equal((await call('invoke_command', { name: 'mp_chat.status', args: [] })).result, 'OK');
  const wrongCount = await request('tools/call', { name: 'invoke_command',
    arguments: { name: 'mp_chat.status', args: [1] } });
  assert.equal(wrongCount.result.isError, true);
  assert(wrongCount.result.content[0].text.includes('wrong command argument count'));
  const panel = await call('get_widget_panel', { panel_id: 'diegetic_asop.terminal' });
  const input = panel.nodes.find(n => n.kind === 'text_input');
  assert(input);
  assert.equal((await call('send_widget_event', { panel_id: panel.id,
    node_id: input.id, kind: 'text_input', text: '272' })).result, 'OK');
  assert((await call('get_widget_panel', { panel_id: panel.id })).nodes.some(n => n.text === '272'));
  await call('mock_move_camera', { pos: [2, -5, 1.7], rot: [0, 0, 0, 1] });
  admin = new WebSocket(url);
  await once(admin, 'open', { signal: deadline });
  assert.deepEqual((await adminCall('mock_game.state')).camera.pos, [2, -5, 1.7]);
  assert.equal((await call('mock_set_service', { service: 'game.actors', published: false })).published, false);
  assert.equal((await adminCall('mock_game.services.list')).find(s => s.name === 'game.actors').published, false);
  await call('mock_set_service', { service: 'game.actors', published: true });
  assert((await call('convert_asset', { path: fixture })).bytes > 0);
  const invalid = await request('tools/call', { name: 'mock_move_camera', arguments: { pos: [1, 2] } });
  assert.equal(invalid.result.isError, true);
  const unknown = await request('tools/call', { name: 'invoke_command', arguments: { name: 'missing.command' } });
  assert.equal(unknown.result.isError, true);
  await adminCall('system.shutdown');
  assert.equal((await hostExit)[0], 0);
  const disconnected = await request('tools/call', { name: 'host_status' });
  assert.equal(disconnected.result.isError, true);
  server.stdin.end();
  assert.equal((await serverExit)[0], 0);
  await new Promise((accept, reject) => {
    const probe = connect({ host: '127.0.0.1', port });
    probe.once('connect', () => { probe.destroy(); reject(new Error('host port remained open')); });
    probe.once('error', error => error.code === 'ECONNREFUSED' ? accept() : reject(error));
  });
  console.log(JSON.stringify({ status: 'PASS', tools: 7, transcript, cleanup: { hostExit: 0, serverExit: 0, portClosed: true } }, null, 2));
} catch (error) {
  console.error(hostLog, serverLog);
  throw error;
} finally {
  admin?.close();
  for (const entry of pending.values()) clearTimeout(entry.timer);
  if (server && server.exitCode === null) server.kill();
  if (serverExit) await serverExit;
  if (host.exitCode === null) host.kill();
  await hostExit;
  serverLines?.close(); hostLines.close();
  await rm(work, { recursive: true });
}
