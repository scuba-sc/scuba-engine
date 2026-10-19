// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
const [executable, script, mode] = process.argv.slice(2);
assert(executable && script && ['success', 'refused', 'invalid'].includes(mode));
const child = spawn(executable, ['--warp', '--hidden', '--no-plugins', '--drive', script],
  { stdio: ['ignore', 'ignore', 'pipe'] });
let log = '';
child.stderr.on('data', chunk => { log += chunk; });
const [code] = await once(child, 'exit', { signal: AbortSignal.timeout(25000) });
assert.equal(code, mode === 'success' ? 0 : 1, log);
const replies = log.split(/\r?\n/).filter(line => line.startsWith('[info] drive: '))
  .map(line => JSON.parse(line.slice('[info] drive: '.length)));
if (mode === 'success') {
  assert.equal(replies.length, 4, log);
  assert(replies[1].result.capabilities.every(capability => !capability.ready));
  assert(replies[3].result.capabilities.every(capability => capability.ready));
} else if (mode === 'refused') {
  assert.equal(replies.length, 1, log);
  assert.equal(replies[0].error.code, -32601);
} else {
  assert.equal(replies.length, 0, log);
  assert(log.includes('Expected bounded actions array in script'));
}
console.log(`native drive ${mode}: PASS`);
