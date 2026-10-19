// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
const [executable] = process.argv.slice(2);
assert(executable);
for (const args of [['--width', '0'], ['--height', '4097'], ['--width', '1280junk'],
  ['--control-port', '65536'], ['--control-port', '-1']]) {
  const child = spawn(executable, args, { stdio: ['ignore', 'ignore', 'pipe'] });
  let log = ''; child.stderr.on('data', chunk => { log += chunk; });
  const [code] = await once(child, 'exit', { signal: AbortSignal.timeout(10000) });
  assert.equal(code, 2, `${args.join(' ')}\n${log}`);
  assert(!log.includes('listening port='));
}
console.log('native arguments: PASS bounded dimensions/control port');
