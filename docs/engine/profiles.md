# Hostd profiles

`scuba-hostd --profile profile.json` selects plugin folders and synthetic mock services for a
bounded out-of-game run. For example:

```json
{"plugins":["mp_chat"],"mocks":["game.events"],"services":["game.actors","game.entities","game.world","spawn.entities","teleport.spatial"]}
```

All three arrays are required. Plugin entries are unique SDK plugin IDs (`[a-z0-9_]`, at most
31 characters), matching folders under the default build-tree plugin root or the root passed to
`--plugins DIR`. Only listed plugins are loaded; include plugin dependencies explicitly. The
SDK resolves dependencies and loads data, Lua, and native plugins in its normal order. A missing,
disabled, invalid, refused, or failed selected plugin makes startup fail. `--no-lua` still refuses
Lua plugins. The mock initializer is a built-in and is always loaded. A selected plugin may
already have run before another selected plugin is refused: hostd then stops the kit and unloads
loaded plugins, but cannot undo their external side effects. Plugin code is trusted local code;
this is not an OS security sandbox.

`mocks` currently accepts `game.events`. `services` accepts names listed by
`mock_game.services.list`. Selection publishes only those mock tables and enables their normally
on mock capabilities; default-off capabilities stay off. These providers are synthetic and do
not establish native game behavior. `--profile` conflicts with `--no-mock-game`.

The release host kit discovers an entire root rather than filtering plugin IDs. Hostd therefore
copies selected folders into a temporary snapshot before invoking the kit loader. The snapshot
rejects symlinks and special files, and is limited to 4096 entries, 32 directory levels and
256 MiB across the selection. The source tree must not be modified concurrently during snapshot
creation: the portable filesystem checks are not a defense against a local process replacing a
checked path. Plugins see the snapshot as their plugin folder, so edits to the
source folder are picked up on the next startup, not during this run. Hostd removes the snapshot
after clean teardown; if the SDK retains a faulted or pinned module it leaves the snapshot in
the system temporary directory. A process crash can also leave a snapshot behind.

A profile and `--scenario FILE` may be combined: plugins start first, then the bounded synthetic
scenario runs and hostd unloads them before exiting. With no scenario, the normal bridge starts.
The profile JSON is limited to 65536 bytes and each array to 128 entries; unknown fields and
duplicate entries are rejected.

## Product profile (`--product DIR`)

`--product DIR` mounts a whole product distribution instead of a selection: `<DIR>/data/plugins`
is the roster and `<DIR>/data` is the `sco.storage` root. Every folder in the roster loads, the
whole synthetic game service set is published, and the mode is fail-closed — a missing
`data/plugins`, an empty roster, a refused plugin or a plugin that fails to load stops startup with
exit code 1 and names the folder. It conflicts with `--profile`, `--plugins`, `--data` and
`--no-mock-game`; the root supplies all of them.

`scuba-engine --product DIR` is the same mount in the native editor, with `<DIR>/data` also
mounted as content and the roster printed to stderr. The editor derives `--content`, `--data` and
`--plugins` from the root and refuses to combine them with it.

Both forms run the product's real plugin code against synthetic game services. That is still not
native game behavior, and the snapshot, trusted-code and no-sandbox rules above apply unchanged.

## Scenarios and the headless smoke run

`scuba-engine --scenario FILE` runs a bounded scenario against the mock game on the game thread
before the first frame, prints one `SCENARIO` line per step, and exits 1 if any step did not match
its `expect`. The file format is the one `scuba-hostd --scenario` accepts (`{"steps": [...]}`, at
most 128 steps, 65536 bytes, controls `spawn`, `teleport`, `despawn`, `pose`). The **View >
Scenario** dock runs the same path on demand: it takes a path, runs it, and lists each step with
its outcome and refusal reason. Both go through `scuba::hostd::RunScenario`, so the dock and the
CLI cannot disagree about what a scenario did.

`scuba-engine --frames N --hidden` renders into off-screen targets instead of a swap chain and
presents nothing, so the smoke run works where there is no desktop (a hidden window cannot host a
swap chain: `CreateSwapChainForHwnd` fails with `DXGI_ERROR_INVALID_CALL`). CTest registers that
run as `engine_offscreen_smoke`, plus `engine_scenario_valid` and `engine_scenario_missing` for
the scenario CLI. Neither is native game behavior: the mock game is synthetic throughout.
