// SPDX-License-Identifier: GPL-3.0-only
// Profile-selected first-party plugins through stdio MCP and the native WebSocket bridge.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { cp, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';

const [hostPath, root, profile, serverPath, expectationsPath] = process.argv.slice(2).map(value => resolve(value));
const expected = JSON.parse(await readFile(expectationsPath, 'utf8'));
const work = await mkdtemp(join(tmpdir(), 'scuba-profile-mcp-'));

async function runSession(label) {
  const sessionDir = join(work, label);
  const dataDir = join(sessionDir, 'data');
  const host = spawn(hostPath, ['--port', '0', '--plugins', root, '--profile', profile,
    '--data', dataDir, '--test-shutdown'], { stdio: ['ignore', 'pipe', 'pipe'] });
  const deadline = AbortSignal.timeout(45000);
  const hostExit = once(host, 'exit', { signal: deadline });
  let server, serverExit, admin;
  let log = '';
  host.stderr.on('data', chunk => { log += chunk; });
  const lines = createInterface({ input: host.stdout });
  const ready = new Promise((accept, reject) => {
    lines.on('line', line => {
      log += line + '\n';
      const match = /listening port=(\d+)/.exec(line);
      if (match) accept(Number(match[1]));
    });
    host.once('error', reject);
    host.once('exit', code => reject(new Error(`host exited before listening: ${code}\n${log}`)));
    deadline.addEventListener('abort', () => reject(deadline.reason), { once: true });
  });
  let pending = new Map();
  let id = 0;
  function request(method, params) {
    const next = ++id;
    return new Promise((accept, reject) => {
      const timer = setTimeout(() => reject(new Error(`${method} timed out`)), 10000);
      pending.set(next, value => { clearTimeout(timer); accept(value); });
      server.stdin.write(JSON.stringify({ jsonrpc: '2.0', id: next, method, params }) + '\n');
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
      const value = JSON.parse(line);
      const complete = pending.get(value.id);
      if (complete) { pending.delete(value.id); complete(value); }
    });

    const init = await request('initialize', {
      protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'profile-test', version: '1' },
    });
    assert.equal(init.result.protocolVersion, '2025-11-25');
    server.stdin.write(JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) + '\n');

    const plugins = (await call('host_status')).kernel.plugins;
    assert.deepEqual(plugins.filter(plugin => plugin.id !== 'scuba_mock_game')
      .map(plugin => [plugin.id, plugin.state]).sort((a, b) => a[0].localeCompare(b[0])),
    expected.plugins.map(plugin => [plugin, 'loaded']).sort((a, b) => a[0].localeCompare(b[0])));

    for (const name of expected.commands) {
      const result = await call('invoke_command', { name, args: [] });
      assert.equal(result.result, 'OK', `${name}: ${JSON.stringify(result)}`);
    }

    const services = await call('mock_list_services');
    for (const name of expected.unpublished_services) {
      const service = services.find(item => item.name === name);
      assert(service, `missing mock service ${name}`);
      assert.equal(service.published, false, `${name} must remain unpublished in this sandbox profile`);
    }

    for (const panel of expected.panels) {
      const snapshot = await call('get_widget_panel', { panel_id: panel.id });
      assert.equal(snapshot.id, panel.id);
      assert.equal(snapshot.owner, panel.owner, `${panel.id} ownership mismatch`);
      for (const tag of panel.required_tags ?? [])
        assert(snapshot.nodes.some(node => node.tag === tag), `${panel.id} missing stable tag ${tag}`);
    }

    admin = new WebSocket(url);
    await once(admin, 'open', { signal: deadline });
    const shutdownReply = once(admin, 'message', { signal: deadline });
    admin.send(JSON.stringify({ id: 1, method: 'system.shutdown', params: {} }));
    await shutdownReply;
    const [hostCode] = await hostExit;
    assert.equal(hostCode, 0, `host teardown failed\n${log}`);
    server.stdin.end();
    const [serverCode] = await serverExit;
    assert.equal(serverCode, 0, `MCP bridge did not exit cleanly\n${log}`);
    console.log(`PROFILE_MCP_${label.toUpperCase()}_PASS`);
  } catch (error) {
    console.error(log);
    throw error;
  } finally {
    admin?.close();
    if (server && server.exitCode === null) server.kill();
    if (serverExit && server.exitCode === null) await serverExit;
    if (host.exitCode === null) host.kill();
    if (host.exitCode === null) await hostExit;
    lines.close();
  }
}

async function expectDisabledPluginRefusal() {
  const badRoot = join(work, 'refusal-plugins');
  await cp(root, badRoot, { recursive: true });
  await writeFile(join(badRoot, expected.refuse_plugin, 'disabled'), '');
  const child = spawn(hostPath, ['--port', '0', '--plugins', badRoot, '--profile', profile,
    '--data', join(work, 'refusal-data')], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '';
  child.stdout.on('data', chunk => { output += chunk; });
  child.stderr.on('data', chunk => { output += chunk; });
  const exit = once(child, 'exit');
  let refusalTimer;
  const timeout = new Promise(resolveTimeout => { refusalTimer = setTimeout(() => resolveTimeout(null), 10000); });
  const result = await Promise.race([exit, timeout]);
  clearTimeout(refusalTimer);
  if (!result) {
    child.kill();
    await exit;
    assert.fail(`disabled selected plugin did not refuse startup\n${output}`);
  }
  assert.notEqual(result[0], 0, `disabled selected plugin unexpectedly loaded\n${output}`);
  assert(!/listening port=\d+/.test(output), `host listened after selected plugin refusal\n${output}`);
  assert(output.includes(expected.refuse_plugin), `refusal did not identify ${expected.refuse_plugin}\n${output}`);
  console.log('PROFILE_MCP_REFUSAL_PASS');
}

try {
  await runSession('first');
  await runSession('restart');
  await expectDisabledPluginRefusal();
  console.log('PROFILE_MCP_PASS');
} finally {
  await rm(work, { recursive: true, force: true });
}
