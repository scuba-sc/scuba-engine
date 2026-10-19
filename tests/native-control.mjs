// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { Bridge, Server, PROTOCOL } from '../tools/scuba-engine-mcp/server.mjs';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { inflateSync } from 'node:zlib';
const [executable] = process.argv.slice(2);
assert(executable);
const work = await mkdtemp(join(tmpdir(), 'native-control-'));
const child = spawn(executable, ['--warp', '--hidden', '--no-plugins', '--control-port', '0', '--width', '1280', '--height', '800', '--data', work, '--open', 'models/cube.obj', '--open', 'models/pyramid.obj'],
  { stdio: ['ignore', 'pipe', 'pipe'] });
const deadline = AbortSignal.timeout(30000);
const exited = once(child, 'exit', { signal: deadline });
const lines = createInterface({ input: child.stderr });
let log = '', socket;
const ready = new Promise((resolve, reject) => {
  lines.on('line', line => {
    log += line + '\n';
    const match = /control: listening port=(\d+)/.exec(line);
    if (match) resolve(Number(match[1]));
  });
  child.once('error', reject);
  child.once('exit', code => reject(new Error(`native editor exited before ready: ${code}\n${log}`)));
  deadline.addEventListener('abort', () => reject(deadline.reason), { once: true });
});
const pending = new Map();
let sequence = 0;
function call(method, params = {}) {
  const id = ++sequence;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`timeout: ${method}`)); }, 5000);
    pending.set(id, value => { clearTimeout(timer); resolve(value); });
    socket.send(JSON.stringify({ id, method, params }));
  });
}
try {
  const port = await ready;
  socket = new WebSocket(`ws://127.0.0.1:${port}/ws`);
  socket.addEventListener('message', event => {
    const value = JSON.parse(event.data);
    const resolve = pending.get(value.id);
    if (resolve) { pending.delete(value.id); resolve(value); }
  });
  await once(socket, 'open', { signal: deadline });
  const commands = (await call('plugins.commands')).result;
  const methods = (await call('system.methods')).result;
  for (const method of ['engine.actions.list', 'engine.actions.get', 'engine.actions.invoke', 'engine.state.get', 'engine.vfs.get'])
    assert(methods.includes(method));
  for (const name of ['sco.mock.admit', 'sco.mock.revoke', 'sco.mock.fire_respawn_loadout', 'sco.mock.fire_mission_changed'])
    assert(commands.some(command => command.name === name && command.args.length === 0));
  assert.deepEqual((await call('engine.actions.list')).result, commands);
  assert.deepEqual((await call('engine.actions.get', { id: 'sco.mock.admit' })).result,
    commands.find(command => command.name === 'sco.mock.admit'));
  assert.equal((await call('engine.actions.get', { id: 'missing.command' })).error.code, -32601);
  assert.equal((await call('engine.actions.invoke', { id: 'scene.camera.set', args: [1.2, 0.7, 12] })).result.result, 'OK');
  const camera = (await call('engine.state.get')).result.camera;
  assert(Math.abs(camera.yaw - 1.2) < 0.00001 && Math.abs(camera.pitch - 0.7) < 0.00001 && camera.distance === 12);
  assert.equal((await call('engine.actions.invoke', { id: 'scene.camera.set', args: [1.2, 0.7, -1] })).result.result, 'BAD_ARG');
  assert.deepEqual((await call('engine.state.get')).result.camera, camera);
  const mcp = new Server({ async call(method, params) {
    const response = await call(method, params);
    if (response.error) throw new Error(response.error.message);
    return response.result;
  } });
  await mcp.handle({ jsonrpc: '2.0', id: 1, method: 'initialize', params: {
    protocolVersion: PROTOCOL, capabilities: {}, clientInfo: { name: 'native-fixture', version: '1' } } });
  await mcp.handle({ jsonrpc: '2.0', method: 'notifications/initialized' });
  const described = await mcp.handle({ jsonrpc: '2.0', id: 2, method: 'tools/call',
    params: { name: 'engine_describe', arguments: { id: 'sco.mock.admit' } } });
  assert.equal(described.result.isError, false);
  assert.equal(JSON.parse(described.result.content[0].text).name, 'sco.mock.admit');
  const invoked = await mcp.handle({ jsonrpc: '2.0', id: 3, method: 'tools/call',
    params: { name: 'engine_invoke', arguments: { id: 'sco.mock.admit', request_id: 'mcp-native' } } });
  assert.equal(invoked.result.isError, false);
  assert.equal(JSON.parse(invoked.result.content[0].text).request_id, 'mcp-native');
  const named = await call('engine.actions.invoke', { id: 'sco.mock.admit', request_id: 'native-fixture' });
  const direct = await call('plugins.invoke', { name: 'sco.mock.admit', args: [] });
  assert.equal(named.result.reply, direct.result.reply);
  assert.equal(named.result.request_id, 'native-fixture');
  assert.equal((await call('engine.actions.invoke', { id: 'sco.mock.revoke', request_id: 1 })).error.code, -32602);
  for (const params of [{ id: 'sco.mock.revoke', request_id: '' },
    { id: 'sco.mock.revoke', request_id: 'bad\0id' },
    { id: 'sco.mock.revoke', request_id: 'fixture', unexpected: true }])
    assert.equal((await call('engine.actions.invoke', params)).error.code, -32602);
  assert((await call('engine.state.get')).result.capabilities.every(capability => capability.ready));
  assert.equal((await call('engine.actions.invoke', { id: 'missing.command' })).error.code, -32601);
  assert.equal((await call('plugins.invoke', { name: 'sco.mock.revoke', args: [] })).result.result, 'OK');
  const revoked = (await call('engine.state.get')).result;
  assert.equal(revoked.width, 1280); assert.equal(revoked.height, 800);
  assert(revoked.capabilities.length > 0 && revoked.capabilities.every(capability => !capability.ready));
  assert.equal((await call('plugins.invoke', { name: 'sco.mock.fire_respawn_loadout', args: [] })).result.result, 'UNAVAILABLE');
  assert.equal((await call('plugins.invoke', { name: 'sco.mock.admit', args: [] })).result.result, 'OK');
  assert((await call('engine.state.get')).result.capabilities.every(capability => capability.ready));
  assert.equal((await call('plugins.invoke', { name: 'sco.mock.fire_respawn_loadout', args: [] })).result.result, 'OK');
  assert.equal((await call('plugins.invoke', { name: 'sco.mock.admit', args: [1] })).error.code, -32602);
  const frameReady = new Promise((resolve, reject) => {
    const timer = setTimeout(() => { socket.removeEventListener('message', listener); reject(new Error('frame event timeout')); }, 5000);
    const listener = event => {
      const message = JSON.parse(event.data);
      if (message.event !== 'engine.frame.ready') return;
      clearTimeout(timer); socket.removeEventListener('message', listener); resolve(message.data);
    };
    socket.addEventListener('message', listener);
  });
  const requestedFrame = await call('engine.frame.request');
  const completedFrame = await frameReady;
  assert.equal(completedFrame.ticket, requestedFrame.result.ticket);
  assert(completedFrame.frame > 0);
  const frameBridge = new Bridge(`ws://127.0.0.1:${port}/ws`);
  try {
    assert.equal((await call('engine.frame.request', { unexpected: true })).error.code, -32602);
    const receipt = await frameBridge.frame();
    assert(receipt.ticket > completedFrame.ticket && receipt.frame >= completedFrame.frame);
  } finally { frameBridge.close(); }
  const capture = await call('engine.screenshot', { path: 'native.png' });
  assert.equal(capture.result.width, 1280); assert.equal(capture.result.height, 800);
  const png = await readFile(join(work, 'captures', 'native.png'));
  assert.deepEqual([...png.subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
  assert.equal(png.readUInt32BE(16), 1280); assert.equal(png.readUInt32BE(20), 800);
  let offset = 8; const imageData = [];
  while (offset < png.length) {
    const length = png.readUInt32BE(offset), type = png.toString('ascii', offset + 4, offset + 8);
    if (type === 'IDAT') imageData.push(png.subarray(offset + 8, offset + 8 + length));
    offset += length + 12;
  }
  const rgba = inflateSync(Buffer.concat(imageData));
  assert.equal(rgba.length, (1280 * 4 + 1) * 800);
  assert(rgba.some(value => value !== 0));
  assert.equal((await call('engine.screenshot', { path: 'native.png' })).error.code, -32000);
  assert.equal((await call('engine.screenshot', { path: '../outside.png' })).error.code, -32602);
  for (const params of [{ path: 'extra.png', unexpected: true }, { path: 'bad\0name.png' },
    { path: 'absolute/escape.png' }, { path: 'wrong.jpg' }, {}])
    assert.equal((await call('engine.screenshot', params)).error.code, -32602);
  assert.deepEqual(await readFile(join(work, 'captures', 'native.png')), png);
  const mcpCapture = await mcp.handle({ jsonrpc: '2.0', id: 4, method: 'tools/call',
    params: { name: 'engine_screenshot', arguments: { path: 'mcp-native.png' } } });
  assert.equal(mcpCapture.result.isError, false);
  assert.equal(JSON.parse(mcpCapture.result.content[0].text).width, 1280);
  const sceneBefore = (await call('engine.state.get')).result;
  assert.equal(sceneBefore.scene.length, 2);
  assert.equal(sceneBefore.selected, sceneBefore.scene[1].id);
  for (const id of [-1, 4294967296, 999999])
    assert.equal((await call('engine.actions.invoke', { id: 'scene.select', args: [id] })).result.result, 'BAD_ARG');
  assert.equal((await call('engine.state.get')).result.selected, sceneBefore.selected);
  assert.equal((await call('engine.actions.invoke', { id: 'scene.select', args: [0] })).result.result, 'OK');
  assert.equal((await call('engine.state.get')).result.selected, 0);
  assert.equal((await call('engine.actions.invoke', { id: 'scene.select', args: [sceneBefore.selected] })).result.result, 'OK');
  assert.equal((await call('engine.vfs.get', { unexpected: true })).error.code, -32602);
  const mountsBefore = (await call('engine.vfs.get')).result.mounts;
  assert(mountsBefore.length >= 1);
  assert.equal((await call('engine.actions.invoke', { id: 'vfs.mount', args: [] })).error.code, -32602);
  assert.equal((await call('engine.actions.invoke', { id: 'vfs.mount', args: ['C:/definitely-missing-zz', '', 0] })).result.result, 'FAILED');
  assert.equal((await call('engine.actions.invoke', { id: 'vfs.mount', args: [work, '..', 0] })).result.result, 'BAD_ARG');
  assert.equal((await call('engine.actions.invoke', { id: 'vfs.mount', args: [work, '', 9999] })).result.result, 'BAD_ARG');
  const mounted = await call('engine.actions.invoke', { id: 'vfs.mount', args: [work, '', 5], request_id: 'mount-cycle' });
  assert.equal(mounted.result.result, 'OK');
  const newMounts = (await call('engine.vfs.get')).result.mounts;
  const tempMount = newMounts.find(mount => mount.kind === 'dir' && mount.label === work.split(/[\\/]/).pop());
  assert(tempMount);
  assert.equal(mountsBefore.some(mount => mount.id === tempMount.id), false);
  assert.equal((await call('engine.actions.invoke', { id: 'vfs.unmount', args: [tempMount.id], request_id: 'unmount-cycle' })).result.result, 'OK');
  assert.equal((await call('engine.vfs.get')).result.mounts.some(mount => mount.id === tempMount.id), false);
  assert.equal((await call('engine.actions.invoke', { id: 'vfs.unmount', args: [tempMount.id] })).result.result, 'NOT_FOUND');
  assert.equal((await call('engine.actions.invoke', { id: 'vfs.unmount', args: [0] })).result.result, 'BAD_ARG');
  assert.equal((await call('engine.actions.invoke', { id: 'vfs.unmount', args: ['x'] })).error.code, -32602);
  assert.equal((await call('engine.actions.invoke', { id: 'scene.selection.transform', args: [3, 4, 5, 0.5, 2] })).result.result, 'OK');
  const transformed = (await call('engine.state.get')).result.scene.find(item => item.id === sceneBefore.selected).transform;
  assert.deepEqual(transformed, { x: 3, y: 4, z: 5, yaw: 0.5, scale: 2 });
  assert.equal((await call('engine.actions.invoke', { id: 'scene.selection.material', args: [1, 0, 0.5, 0.5, 0.5] })).result.result, 'OK');
  const material = (await call('engine.state.get')).result.scene.find(item => item.id === sceneBefore.selected).material;
  assert.deepEqual(material, { red: 1, green: 0, blue: 0.5, metallic: 0.5, roughness: 0.5 });
  for (const args of [[-1, 0, 0, 0, 1], [0, 0, 0, 2, 1], [0, 0, 0, 0, 0]]) {
    assert.equal((await call('engine.actions.invoke', { id: 'scene.selection.material', args })).result.result, 'BAD_ARG');
    assert.deepEqual((await call('engine.state.get')).result.scene.find(item => item.id === sceneBefore.selected).material, material);
  }
  for (const args of [[9, 8, 7, 0, -1], [1000001, 0, 0, 0, 1], [0, 0, 0, 4, 1]]) {
    assert.equal((await call('engine.actions.invoke', { id: 'scene.selection.transform', args })).result.result, 'BAD_ARG');
    assert.deepEqual((await call('engine.state.get')).result.scene.find(item => item.id === sceneBefore.selected).transform, transformed);
  }
  const snapshot = (await call('engine.state.get')).result.scene.map(({ name, transform, material }) => ({ name, transform, material }));
  assert.equal((await call('engine.actions.invoke', { id: 'scene.save', args: ['base.json'], request_id: 'save-one' })).result.result, 'OK');
  assert.equal((await call('engine.actions.invoke', { id: 'scene.save', args: ['base.json'] })).result.result, 'FAILED');
  for (const name of ['../evil.json', '', 'bad name.json'])
    assert.equal((await call('engine.actions.invoke', { id: 'scene.save', args: [name] })).result.result, 'BAD_ARG');
  assert.equal((await call('engine.actions.invoke', { id: 'scene.load', args: ['missing.json'] })).result.result, 'NOT_FOUND');
  assert.equal((await call('engine.actions.invoke', { id: 'scene.clear', args: [] })).result.result, 'OK');
  assert.equal((await call('engine.actions.invoke', { id: 'scene.load', args: ['base.json'], request_id: 'load-one' })).result.result, 'OK');
  const reloaded = (await call('engine.state.get')).result.scene.map(({ name, transform, material }) => ({ name, transform, material }));
  assert.deepEqual(reloaded, snapshot);
  const reloadedScene = (await call('engine.state.get')).result.scene;
  await writeFile(join(work, 'scenes', 'malformed.json'), '{"instances": [{"path": "models/cube.obj", "x": 1e9}]}', 'utf8');
  const kept = (await call('engine.state.get')).result.scene.length;
  assert.equal((await call('engine.actions.invoke', { id: 'scene.load', args: ['malformed.json'] })).result.result, 'BAD_ARG');
  assert.equal((await call('engine.state.get')).result.scene.length, kept);
  assert.equal((await call('engine.actions.invoke', { id: 'scene.clear', args: [1] })).error.code, -32602);
  assert.deepEqual((await call('engine.state.get')).result.scene.map(({ id, name }) => ({ id, name })),
    reloadedScene.map(({ id, name }) => ({ id, name })));
  assert.equal((await call('engine.actions.invoke', { id: 'scene.selection.remove' })).result.result, 'OK');
  const removed = (await call('engine.state.get')).result;
  assert.equal(removed.scene.length, 1); assert.equal(removed.selected, 0);
  assert.equal(removed.scene[0].id, reloadedScene[0].id);
  assert.equal((await call('engine.actions.invoke', { id: 'scene.clear', request_id: 'clear-fixture' })).result.result, 'OK');
  const sceneAfter = (await call('engine.state.get')).result;
  assert.deepEqual(sceneAfter.scene, []); assert.equal(sceneAfter.selected, 0);
  for (const id of ['scene.selection.frame', 'scene.selection.remove']) {
    assert(commands.some(command => command.name === id && command.args.length === 0));
    assert.equal((await call('engine.actions.invoke', { id })).result.result, 'BAD_ARG');
  }
  assert.deepEqual((await call('engine.state.get')).result.camera, sceneAfter.camera);
  assert.equal((await call('engine.actions.invoke', { id: 'scene.selection.material', args: [1, 0, 0, 0, 1] })).result.result, 'BAD_ARG');
  const mcpState = await mcp.handle({ jsonrpc: '2.0', id: 5, method: 'tools/call',
    params: { name: 'engine_state', arguments: {} } });
  assert.equal(mcpState.result.isError, false);
  const stateReadback = JSON.parse(mcpState.result.content[0].text);
  assert.deepEqual(stateReadback.scene, []); assert.equal(stateReadback.selected, 0);
  assert.deepEqual(stateReadback.camera, sceneAfter.camera);
  for (const path of ['../models/cube.obj', 'models/missing.obj', 'models/cube.txt', ''])
    assert.equal((await call('engine.actions.invoke', { id: 'scene.place', args: [path] })).result.result, 'BAD_ARG');
  assert.deepEqual((await call('engine.state.get')).result.scene, []);
  assert.equal((await call('engine.actions.invoke', { id: 'scene.place', args: ['models/cube.obj'] })).result.result, 'OK');
  const placed = (await call('engine.state.get')).result;
  assert.equal(placed.scene.length, 1); assert.equal(placed.scene[0].name, 'cube.obj');
  assert.equal(placed.selected, placed.scene[0].id);
  assert.equal(typeof placed.scene[0].resident, 'boolean');
  assert.equal(typeof placed.scene[0].load_error, 'string');
  assert(['queued', 'reading', 'decoding', 'ready', 'failed', 'cancelled'].includes(placed.scene[0].load_state.toLowerCase()));
  const instanceCompleted = new Promise((resolve, reject) => {
    const listener = event => {
      const message = JSON.parse(event.data);
      if (message.event !== 'engine.instance.completed') return;
      clearTimeout(timer); socket.removeEventListener('message', listener); resolve(message.data);
    };
    const timer = setTimeout(() => { socket.removeEventListener('message', listener); reject(new Error('instance completion timeout')); }, 5000);
    socket.addEventListener('message', listener);
  });
  const watched = await call('engine.instance.watch', { id: placed.selected });
  const instanceReceipt = await instanceCompleted;
  assert.equal(instanceReceipt.ticket, watched.result.ticket);
  assert.equal(instanceReceipt.id, placed.selected); assert.equal(instanceReceipt.outcome, 'ready');
  const readyInstance = (await call('engine.state.get')).result.scene[0];
  assert.equal(readyInstance.loaded, true); assert.equal(readyInstance.resident, true);
  const instanceBridge = new Bridge(`ws://127.0.0.1:${port}/ws`);
  try {
    const receipt = await instanceBridge.instance(placed.selected);
    assert.equal(receipt.outcome, 'ready'); assert.equal(receipt.id, placed.selected);
  } finally { instanceBridge.close(); }
  for (const id of [-1, 1.5, 4294967296, 999999])
    assert.equal((await call('engine.instance.watch', { id })).error.code, -32602);
  assert.equal((await call('system.shutdown')).result.stopping, true);
  assert.equal((await exited)[0], 0, log);
  console.log('native control: PASS discovery/admission/event refusal/clean shutdown');
} finally {
  socket?.close(); lines.close();
  if (child.exitCode === null) { child.kill(); await exited.catch(() => {}); }
  await rm(work, { recursive: true, force: true });
}
