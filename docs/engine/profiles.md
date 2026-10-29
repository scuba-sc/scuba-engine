# Hostd profiles

See [the first-party plugin map](plugin-integration.md) for the tested engine app suite, external
sc-mp profile, and provider-readiness boundaries.

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
