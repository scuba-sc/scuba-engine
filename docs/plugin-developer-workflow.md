# Independent plugin workflow on the published SDK

This workflow keeps discovery, compilation and runtime evidence separate.
It has been exercised locally on Windows x64 with SDK 2.3.0, the SDK checker,
native hostd and the native editor. Not run in game.

## Discover and scaffold

Connect `sco-core-mcp` to a catalogue built from a trusted clean source commit.
Enable `--extended-tools` for manifest checking and scaffolding. Use
`check_compatibility` to compare the catalogue coordinates with the SDK archive;
it is not runtime certification. Read a service's absent-capability, ownership,
thread and table-extent contracts before using it.

Call `generate_plugin_scaffold` with a non-reserved ID, `lang: "c"` or `"cpp"`,
and optionally `with_menu: true`. The tool returns files; it does not write,
build or load them. Materialize the files in a new independent folder, then
validate its manifest with `validate_manifest_and_widgets`.

## Compile and check

Download the published SDK archive and verify it against the release's
`SHA256SUMS` before extraction. Do not point this proof at a sibling checkout or
an unpublished candidate. The SDK root below contains `cmake`, `include` and
`tools` (in a source kit that root is its `sdk` directory).

```sh
cmake -S plugin -B plugin/build -A x64 -DSCO_SDK=<verified-sdk-root>
cmake --build plugin/build --config Debug --target check
```

The generated target loads the module through `sco-plugin-check` with full and
short host tables. Both must finish with zero failures. Missing optional
services must refuse without a private engine fallback. A checker pass is not
a game run.

## Inspect in native hostd and editor

The build stages `build/check/<id>/plugin.ini` and the module. Pass the parent
directory to hostd; do not install a second plugin with the same ID.

```sh
scuba-hostd --bind 127.0.0.1 --port 7878 --plugins plugin/build/check --data sandbox-data
node tools/scuba-engine-mcp/server.mjs --host ws://127.0.0.1:7878/ws
```

Through MCP, confirm `host_status` reports the plugin loaded, invoke its ping
command, read its retained panel and send a supported widget event. Inspect the
reply/readback rather than treating successful transport as successful behavior.
Use an isolated port if another sandbox already owns 7878.

```sh
scuba-engine --warp --plugins plugin/build/check --data editor-data --open models/cube.obj
```

The native editor's Plugin controls tab renders the same retained SDK panels.
Scene/Assets and Inspector surround the viewport; Console and loader diagnostics
stay below it. `View > Reset workspace` repairs an unusable saved layout without
clearing scene data. Mock services and camera poses are synthetic.

## Bounded agent automation

The sandbox MCP server's optional `--projects` configuration adds build, check
and test tools for named trusted projects. The operator fixes executable,
arguments, working directory and time limit in configuration. The agent supplies
only a project ID. Results include exit code, bounded output, diagnostics and
timeout/truncation flags; failures are not retried. See
[`tools/scuba-engine-mcp/README.md`](../tools/scuba-engine-mcp/README.md).

## What remains an owner check

Use the same exact module in a positively admitted offline sc-offline session.
Record build/module identity, observed command and widget behavior, refusal when
the service is absent, and unload behavior. Native hostd/editor evidence does
not promote a game capability or authorize publication. Remove personal paths,
account names and tokens from anything shared publicly.
