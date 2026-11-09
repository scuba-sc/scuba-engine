// Shared by the generated-plugin integration test and its policy controls.
export async function verifyGeneratedLua(executable, required, run) {
  if (!executable || executable.endsWith('-NOTFOUND')) {
    if (required) throw new Error('Generated Lua coverage requires a Lua 5.4 executable');
    return;
  }
  await run(executable, ['-e', 'assert(_VERSION == "Lua 5.4")']);
}
