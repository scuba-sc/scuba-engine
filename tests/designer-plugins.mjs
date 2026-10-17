import { mkdir, mkdtemp, readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { spawn } from 'node:child_process';
import { flatten, sampleDesigns } from '../tools/designer-codegen/src/model';
import * as cpp from '../tools/designer-codegen/src/codegen/cpp';
import * as c from '../tools/designer-codegen/src/codegen/c';
import * as lua from '../tools/designer-codegen/src/codegen/lua';

const [sdkRoot, outputRoot, python = 'python3', luaExecutable = 'lua5.4'] = process.argv.slice(2);
if (!sdkRoot || !outputRoot) throw new Error('Usage: bun tests/designer-plugins.mjs SDK_SOURCE OUTPUT_ROOT [PYTHON] [LUA54]');
await mkdir(outputRoot, { recursive: true });
const runRoot = await mkdtemp(join(outputRoot, 'generated-'));
async function run(command, args) {
  const child = spawn(command, args, { stdio: 'inherit', timeout: 180_000 });
  const status = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', resolve);
  });
  console.log(`${command} ${args.join(' ')} => ${status ?? 'failed'}`);
  if (status !== 0) throw new Error(`${command} failed (${status})`);
}
for (const [language, generator] of [['c', c], ['cpp', cpp]]) {
  for (const sample of sampleDesigns()) {
    const design = { ...sample, id: `${sample.id}_${language}` };
    if (sample.id === 'workbench') {
      design.title = 'Synthetic */ panel';
      const nodes = flatten(design.nodes);
      nodes[3].node.minimum = -1e30;
      nodes[3].node.maximum = 1e30;
      nodes[3].node.value = 1e25;
      nodes[6].node.text = 'a;b; "quoted"';
    }
    const folder = join(runRoot, design.id);
    await run(python, [join(sdkRoot, 'sdk/tools/sco-new-plugin.py'), '--id', design.id, '--lang', language,
      '--author', 'the maintainer', '--out', folder]);
    await writeFile(join(folder, `${design.id}.${language}`), generator.generate(design));
    const build = join(folder, 'build');
    const flags = process.platform === 'win32' ? '/W4 /WX' : '-Wall -Wextra -Wshadow -Wconversion -Werror';
    await run('cmake', ['-S', folder, '-B', build, `-DSCO_SDK=${join(sdkRoot, 'sdk')}`,
      `-DCMAKE_C_FLAGS=${flags}`, ...(language === 'cpp' ? [`-DCMAKE_CXX_FLAGS=${flags}${process.platform === 'win32' ? ' /EHsc' : ''}`] : [])]);
    await run('cmake', ['--build', build, '--config', 'Debug', '--target', 'check']);
  }
}

// CMake passes <var>-NOTFOUND when no Lua 5.4 is installed (a stock Windows box): the native
// plugins above are still checked, and the result says Lua was not.
if (luaExecutable.endsWith('-NOTFOUND')) {
  console.log('DESIGNER_PLUGINS_OK: six native plugins passed both checker modes; Lua NOT checked (no lua5.4 found)');
  process.exit(0);
}

// The published checker has no widgets/panel_builder stand-in. It still checks the
// capability guard; a second harness executes the real embedded builder below.
const luaFolder = join(runRoot, 'instrument_lua');
await mkdir(luaFolder);
await writeFile(join(luaFolder, 'plugin.ini'), 'id = instrument_lua\nname = Instrument\nversion = 0.1.0\nauthor = the maintainer\napi = 1.0\nkind = lua\nentry = main.lua\n');
const luaSource = lua.generate(sampleDesigns()[0]);
await writeFile(join(luaFolder, 'main.lua'), `if not sco.panel_builder then\n  sco.log("info", "panel_builder unavailable")\n  return\nend\n${luaSource}`);
await run(luaExecutable, [join(sdkRoot, 'sdk/tools/lua-check.lua'), luaFolder]);

const embedded = await readFile(join(sdkRoot, 'plugins/lua/sco_panel_builder.h'), 'utf8');
const builderSource = [...embedded.matchAll(/^\s*("(?:[^"\\]|\\.)*")/gm)]
  .map(match => JSON.parse(match[1])).join('');
const harness = join(runRoot, 'builder-check.lua');
await writeFile(harness, `local builder = assert(load(${JSON.stringify(builderSource)}, "panel_builder"))()
local sco = { panel_builder = builder }
local nodes, dispatch = (function()
${luaSource}
end)()
assert(#nodes == 4)
assert(nodes[2].label == "Run diagnostics")
assert(nodes[4].minimum == 0 and nodes[4].maximum == 1)
assert(type(dispatch) == "function")
print("OK: actual embedded Lua builder accepted generated source")
`);
await run(luaExecutable, [harness]);
console.log('DESIGNER_PLUGINS_OK: six native plugins passed both checker modes; Lua checked');
