// SPDX-License-Identifier: GPL-3.0-only
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, rmdirSync, symlinkSync, unlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { test } from 'node:test';
import { fileURLToPath, pathToFileURL } from 'node:url';

const server = resolve(dirname(fileURLToPath(import.meta.url)), '../tools/scuba-engine-mcp/server.mjs');
const initialize = JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'initialize', params: {
  protocolVersion: '2025-11-25', capabilities: {}, clientInfo: { name: 'test', version: '1' },
} }) + '\n';

function run(args, input = '') {
  const result = spawnSync(process.execPath, args, { input, encoding: 'utf8', timeout: 10000 });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stderr);
  return result.stdout;
}

function assertStarts(path, flags = []) {
  const output = run([...flags, path, '--host', 'ws://127.0.0.1:1/ws'], initialize);
  const reply = JSON.parse(output);
  assert.equal(reply.id, 1);
  assert.equal(reply.result.serverInfo.name, 'scuba-engine-mcp');
}

test('CLI starts through a direct path', () => assertStarts(server));

for (const kind of ['file', 'dir']) {
  test(`CLI starts through a ${kind} symlink`, t => {
    const root = mkdtempSync(join(tmpdir(), 'scuba-mcp-entry-'));
    const link = join(root, kind === 'file' ? 'server-link.mjs' : 'server-dir');
    let linked = false;
    try {
      try {
        symlinkSync(kind === 'file' ? server : dirname(server), link, kind);
        linked = true;
      } catch (error) {
        if (process.platform === 'win32' && ['EPERM', 'EACCES', 'ENOTSUP'].includes(error.code)) {
          t.skip(`symlink unavailable: ${error.code}`);
          return;
        }
        throw error;
      }
      const entry = kind === 'file' ? link : join(link, 'server.mjs');
      assertStarts(entry);
      assertStarts(entry, ['--preserve-symlinks-main']);
    } finally {
      if (linked) {
        if (process.platform === 'win32' && kind === 'dir') rmdirSync(link);
        else unlinkSync(link);
      }
      rmdirSync(root);
    }
  });
}

test('import does not start the server for missing or unrelated argv paths', () => {
  for (const argvPath of [join(tmpdir(), 'missing-scuba-entrypoint'), process.execPath]) {
    const code = `process.argv[1] = ${JSON.stringify(argvPath)}; ` +
      `await import(${JSON.stringify(pathToFileURL(server).href)}); console.log('imported');`;
    assert.equal(run(['--input-type=module', '-e', code]), 'imported\n');
  }
});
