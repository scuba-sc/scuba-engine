import { strict as assert } from 'node:assert';
import { test } from 'node:test';
import { verifyGeneratedLua } from './generated-lua-policy.mjs';

test('required coverage rejects missing Lua instead of native-only success', async () => {
  for (const executable of ['', undefined, 'SCUBA_LUA_EXECUTABLE-NOTFOUND']) {
    await assert.rejects(verifyGeneratedLua(executable, true, () => assert.fail('must not execute')),
      /requires a Lua 5.4 executable/);
  }
});
test('historical optional coverage still permits Lua-NOTFOUND', async () => {
  await verifyGeneratedLua('SCUBA_LUA_EXECUTABLE-NOTFOUND', false, () => assert.fail('must not execute'));
});
test('an available interpreter must actually execute the Lua 5.4 assertion', async () => {
  const calls = [];
  await verifyGeneratedLua('C:/test tools/lua54.exe', true, async (...args) => calls.push(args));
  assert.deepEqual(calls, [['C:/test tools/lua54.exe', ['-e', 'assert(_VERSION == "Lua 5.4")']]]);
});
test('missing executable and wrong-version process failures propagate', async () => {
  for (const failure of [new Error('ENOENT'), new Error('assertion failed')]) {
    await assert.rejects(verifyGeneratedLua('lua54', true, async () => { throw failure; }),
      error => error === failure);
  }
});
