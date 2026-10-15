import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, rm } from 'node:fs/promises';
import { createInterface } from 'node:readline';
import { resolve } from 'node:path';

const executable = resolve(process.argv[2]);
const fixtures = resolve(process.argv[3]);
const work = await mkdtemp(resolve(process.argv[4], 'integration-'));
const child = spawn(executable, ['--port', '0', '--test-shutdown', '--data', 'data'], {
  cwd: work, stdio: ['ignore', 'pipe', 'pipe'],
});
const exited = once(child, 'exit');
const deadline = AbortSignal.timeout(60_000);
const pending = new Map();
const events = new Map();
let socket;
let sequence = 0;
let log = '';
child.stderr.on('data', chunk => { log += chunk; });
const lines = createInterface({ input: child.stdout });
const ready = new Promise((accept, reject) => {
  lines.on('line', line => {
    log += `${line}\n`;
    const match = /scuba-hostd: listening port=(\d+)/.exec(line);
    if (match) accept(Number(match[1]));
  });
  child.once('exit', code => reject(new Error(`host exited ${code} before readiness`)));
  deadline.addEventListener('abort', () => reject(new Error('host readiness timed out')), { once: true });
});
function call(method, params) {
  const id = ++sequence;
  return new Promise((accept, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${method} timed out`)); }, 15_000);
    pending.set(id, { accept, reject, timer });
    socket.send(JSON.stringify({ id, method, params }));
  });
}
function nextEvent(name) {
  return new Promise((accept, reject) => {
    const timer = setTimeout(() => { events.delete(name); reject(new Error(`${name} timed out`)); }, 15_000);
    events.set(name, data => { clearTimeout(timer); events.delete(name); accept(data); });
  });
}
try {
  const port = await ready;
  socket = new WebSocket(`ws://127.0.0.1:${port}/ws`);
  socket.addEventListener('message', message => {
    const value = JSON.parse(message.data);
    if (value.event) { events.get(value.event)?.(value.data); return; }
    const entry = pending.get(value.id);
    if (!entry) return;
    pending.delete(value.id);
    clearTimeout(entry.timer);
    if (value.error) entry.reject(Object.assign(new Error(value.error.message), value.error));
    else entry.accept(value.result);
  });
  await once(socket, 'open', { signal: deadline });
  assert.equal((await call('system.ping')).pong, true);
  const status = await call('system.status');
  const ids = ['mp_chat', 'org_social', 'diegetic_asop', 'diegetic_shop_inv'];
  for (const id of ids) {
    assert(status.kernel.plugins.some(plugin => plugin.id === id && plugin.state === 'loaded'), `${id} not loaded`);
  }
  const panels = (await call('widgets.list')).panels;
  for (const id of ['mp_chat.main', 'org_social.roster', 'diegetic_asop.terminal', 'diegetic_shop_inv.shop'])
    assert(panels.some(panel => panel.id === id), `missing panel ${id}`);
  const invoke = async (name, args = []) => {
    const reply = await call('plugins.invoke', { name, args });
    assert.equal(reply.result, 'OK', `${name}: ${reply.reply}`);
    return reply.reply;
  };
  await invoke('mp_chat.status');
  await invoke('org_social.member_add', ['synthetic_member', 'officer']);
  await invoke('diegetic_asop.find_lift', ['272']);
  await invoke('diegetic_shop_inv.balance');
  await invoke('diegetic_shop_inv.buy', ['armor_helmet_tern']);
  await invoke('diegetic_shop_inv.equip', ['armor_helmet_tern']);
  await invoke('diegetic_shop_inv.spawn', ['prefab_crate_stack', '4', 0, 0, 0]);
  await assert.rejects(call('plugins.invoke', { name: 'diegetic_shop_inv.spawn', args: ['bad'] }),
    error => error.code === -32602);
  await assert.rejects(call('plugins.invoke', { name: 'missing.command' }),
    error => error.code === -32601);
  const changed = nextEvent('mock_game.changed');
  await call('mock_game.camera.move', { pos: [2, -5, 1.7], rot: [0, 0, 0, 1] });
  assert.equal((await changed).what, 'camera');
  assert.deepEqual((await call('mock_game.state')).camera.pos, [2, -5, 1.7]);
  const asop = (await call('widgets.get', { id: 'diegetic_asop.terminal' })).nodes;
  const input = asop.find(node => node.kind === 'text_input');
  assert(input);
  assert.equal((await call('widgets.event', {
    panel: 'diegetic_asop.terminal', node_id: input.id, kind: 'text_input', text: '272',
  })).result, 'OK');
  assert.equal((await call('assets.convert', { path: resolve(fixtures, 'triangle.obj') })).bytes > 0, true);
  assert.deepEqual((await call('netsim.status')).peers, []);
  const sandbox = await call('netsim.start', { count: 2, sample: true });
  assert.equal(sandbox.peers.length, 2);
  await call('netsim.stop');
  const exported = await call('designer.export', {
    id: 'integration_probe', language: 'c', source: 'int synthetic(void) { return 1; }\n',
  });
  assert.equal(exported.folder, 'integration_probe');
  console.log(`INTEGRATION_PASS modules=system,widgets,mock_game,assets,netsim,designer,plugins apps=${ids.join(',')}`);
  await call('system.shutdown');
  const [code] = await exited;
  assert.equal(code, 0);
} catch (error) {
  console.error(log);
  throw error;
} finally {
  socket?.close();
  for (const entry of pending.values()) clearTimeout(entry.timer);
  for (const handler of events.values()) handler(null);
  if (child.exitCode === null) child.kill('SIGTERM');
  await exited;
  lines.close();
  await rm(work, { recursive: true, force: true });
}
