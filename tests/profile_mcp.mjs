// SPDX-License-Identifier: GPL-3.0-only
// Profile-selected real plugin through stdio MCP and the native WebSocket bridge.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';

const [hostPath, root, profile, serverPath] = process.argv.slice(2).map(value => resolve(value));
const work = await mkdtemp(join(tmpdir(), 'scuba-profile-mcp-'));
const host = spawn(hostPath, ['--port', '0', '--plugins', root, '--profile', profile,
  '--data', join(work, 'data'), '--test-shutdown'], { stdio: ['ignore', 'pipe', 'pipe'] });
const deadline = AbortSignal.timeout(30000);
const hostExit = once(host, 'exit', { signal: deadline });
let server, serverExit, admin;
let log = '';
host.stderr.on('data', chunk => { log += chunk; });
const lines = createInterface({ input: host.stdout });
const ready = new Promise((accept, reject) => {
  lines.on('line', line => { log += line + '\n'; const m = /listening port=(\d+)/.exec(line); if (m) accept(Number(m[1])); });
  host.once('error', reject);
  host.once('exit', code => reject(new Error(`host exited: ${code}`)));
  deadline.addEventListener('abort', () => reject(deadline.reason), { once: true });
});
const pending = new Map();
let id = 0;
function request(method, params) {
  const next = ++id;
  return new Promise((accept, reject) => {
    const timer = setTimeout(() => reject(new Error(`${method} timed out`)), 10000);
    pending.set(next, value => { clearTimeout(timer); accept(value); });
    server.stdin.write(JSON.stringify({jsonrpc: '2.0', id: next, method, params}) + '\n');
  });
}
async function call(name, args = {}) {
  const reply = await request('tools/call', { name, arguments: args });
  assert(!reply.error && !reply.result.isError, JSON.stringify(reply));
  return JSON.parse(reply.result.content[0].text);
}
try {
  const port = await ready;
  const url = `ws://127.0.0.1:${port}/ws`;
  server = spawn(process.execPath, [serverPath, '--host', url], { stdio: ['pipe', 'pipe', 'pipe'] });
  serverExit = once(server, 'exit', { signal: deadline });
  server.stderr.on('data', chunk => { log += chunk; });
  createInterface({ input: server.stdout }).on('line', line => {
    const value = JSON.parse(line); const complete = pending.get(value.id);
    if (complete) { pending.delete(value.id); complete(value); }
  });
  const init = await request('initialize', { protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'profile-test', version: '1' } });
  assert.equal(init.result.protocolVersion, '2025-11-25');
  server.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');
  const plugins = (await call('host_status')).kernel.plugins;
  assert.deepEqual(plugins.filter(p => p.id !== 'scuba_mock_game').map(p => [p.id, p.state]), [['mp_chat', 'loaded']]);
  assert.equal((await call('invoke_command', { name: 'mp_chat.status', args: [] })).result, 'OK');
  assert((await call('mock_list_services')).some(s => s.name === 'game.actors' && s.published));
  admin = new WebSocket(url);
  await once(admin, 'open', { signal: deadline });
  const reply = once(admin, 'message', { signal: deadline });
  admin.send(JSON.stringify({id: 1, method: 'system.shutdown', params: {}}));
  await reply;
  assert.equal((await hostExit)[0], 0);
  server.stdin.end();
  assert.equal((await serverExit)[0], 0);
  console.log('PROFILE_MCP_PASS');
} catch (error) {
  console.error(log);
  throw error;
} finally {
  admin?.close();
  if (server && server.exitCode === null) server.kill();
  if (serverExit) await serverExit;
  if (host.exitCode === null) host.kill();
  await hostExit;
  lines.close();
  await rm(work, { recursive: true, force: true });
}
