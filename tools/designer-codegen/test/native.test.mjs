// SPDX-License-Identifier: GPL-3.0-only
import { test, expect } from 'bun:test';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { createInterface } from 'node:readline';
import { sampleDesigns } from '../src/model';
import { parse } from '../src/codegen/c';

test('native designer child generates roundtrippable source and rejects invalid design', async () => {
  const child = spawn(process.execPath, [new URL('../native.mjs', import.meta.url).pathname.replace(/^\/(.:)/, '$1')],
    { stdio: ['pipe', 'pipe', 'inherit'] });
  const closed = once(child, 'close');
  const lines = createInterface({ input: child.stdout });
  const iterator = lines[Symbol.asyncIterator]();
  try {
    const response = iterator.next();
    child.stdin.write(JSON.stringify({ design: sampleDesigns()[0] }) + '\n');
    const result = JSON.parse((await response).value);
    expect(parse(result.source).id).toBe('instrument');
    const invalid = iterator.next();
    child.stdin.write(JSON.stringify({ design: { ...sampleDesigns()[0], id: '../escape' } }) + '\n');
    expect(JSON.parse((await invalid).value).error).toContain('Invalid');
    child.stdin.end();
    expect((await closed)[0]).toBe(0);
  } finally { child.kill(); lines.close(); }
}, 15000);
