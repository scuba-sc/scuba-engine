# Local sandbox MCP bridge

`scuba-engine-mcp` exposes fifteen tools over newline-delimited stdio JSON-RPC,
using MCP protocol `2025-11-25`. Requires Node.js 22.12+ or Bun; no dependencies.
Start an existing `scuba-hostd` on loopback with the plugins you trust, then run:

```sh
node tools/scuba-engine-mcp/server.mjs --host ws://127.0.0.1:7878/ws
```

Initialize, send `notifications/initialized`, then use `tools/list` and
`tools/call`. The tools are `host_status`, `plugin_commands`, `invoke_command`, `get_widget_panel`,
`send_widget_event`, `mock_move_camera`, `mock_set_service`, and `convert_asset`.
Their JSON Schemas are returned by `tools/list`. `panel_id` maps to hostd's `id`
for reads and `panel` for events; `service` maps to `name`.
`plugin_commands` reads the live command registry, including typed argument
definitions and current capability readiness. Discovery does not invoke commands.
The native editor's opt-in `--control-port` endpoint additionally supports
`engine_actions`, `engine_describe`, `engine_invoke`, `engine_state` and
`engine_screenshot`, `engine_frame` and `engine_instance`. Action tools use the existing
command registry and require a bounded correlation `request_id` for invocation.
An ordinary hostd without native action methods refuses them; point the bridge at
the editor's reported loopback port when driving studio controls.
`engine_state` copies scene instance IDs, names, loaded flags, loader states,
loader errors and GPU residency flags, the selected ID,
editor camera and synthetic capability readiness. It is not an action-completion
or GPU-residency receipt. Generation-await is not yet implemented.
`engine_screenshot` requires `--data` on the editor and a new ASCII `.png`
basename in `path`. It captures completed native GPU client pixels, including
the editor UI, under `<data>/captures/`; it never captures the desktop.
Existing output files, traversal and unknown request fields are refused.
Capture reads the last completed frame, not necessarily a frame containing the
preceding action. Several requests can run in one main-thread poll before the
next scene submission. An action reply confirms CPU-side mutation only; do not
use an immediate capture as a render-completion receipt.
After an action, call `engine_frame` and await its ticket-correlated completion
before capturing. It subscribes before requesting the next scene frame and
returns only after that frame has been submitted and the GPU flushed. It uses
events, not polling, and does not retry on timeout or disconnect. This explicit
barrier can stall for GPU completion; it does not await asset loading or prove
an individual model is resident. A minimized editor cannot submit the requested
frame until restored. General state-generation await remains outstanding.
Native WebSocket clients can separately call `engine.instance.watch` with an
existing integer instance `id`. Subscribe to `engine.instance.completed` before
requesting the watch, then match the returned ticket and instance ID. Outcomes
are `ready`, `failed`, `cancelled` or `removed`. Ready requires a decoded model,
renderer residency and a GPU flush; it does not promise that a minimized window
presented the model. At most 64 watches can remain pending. The MCP
`engine_instance` tool takes `{ "id": <instance-id> }`, subscribes before
requesting the watch and awaits the exact ticket and instance ID. Non-ready
terminal outcomes are tool errors with the receipt retained. Disconnect or
timeout is not retried. To capture a newly placed model, await `engine_instance`,
then `engine_frame`, then request `engine_screenshot`.
Scene actions include `scene.select` (zero deselects), `scene.clear`,
`scene.selection.frame`, `scene.selection.remove`, `scene.camera.set`,
`scene.selection.transform`, `scene.selection.material` and `scene.place`.
Placement accepts an existing normalized mounted-model VFS path, adds and
selects an instance, and requests asynchronous loading. Its successful reply
does not imply decoding or GPU upload has completed. Scene capacity is 4096
instances. Assets double-click uses this same placement command.
Scene documents roundtrip through `scene.save` (exclusive-create
`<data>/scenes/<name>.json`, schema 1, bounded ASCII name, no overwrite) and
`scene.load` (validate every mounted path and numeric bound first, then clear
and rebuild all-or-nothing; assets load asynchronously afterwards). VFS
inspection uses `engine.vfs.get`; `vfs.mount` and `vfs.unmount` manage studio
archive mounts (bounded path, priority -1000..1000; unmount reports `BAD_ARG`
for malformed ids and `NOT_FOUND` for unknown ones).
Transform arguments are position XYZ in metres, yaw in radians and uniform
scale. Material arguments are RGB, metallic and roughness. Both validate every
component before changing the selected instance. State readback includes these
values. Selection, frame, removal, inspector edits and viewport movement use
the same command bus.
These actions edit studio scene instances, not game entities. Reusable gameplay
contracts, native mappings, lifecycle rules and capability evidence belong in
sco-core; sc-offline composes gameplay plugins and studio mock providers consume
the same SDK contracts. Game-neutral infrastructure stays in `sco.*`, with
Star Citizen semantics in the optional `game.*` pack.

This is an opt-in control channel for the out-of-game sandbox. Plugin commands,
widget callbacks and asset conversion may mutate state or write local files;
only run against a host and native plugins you trust. It does not connect to or
certify a game session. Only numeric IPv4 loopback endpoints with `/ws` are
accepted. Remote/LAN endpoints, URL credentials and extra query parameters are
refused. There is no arbitrary hostd-method tool or shutdown tool.

Calls are not replayed or retried. Timeout/disconnect leaves an unknown outcome
for a mutating call; inspect host state and restart the MCP process deliberately
instead of assuming that retry is safe. EOF closes the WebSocket; the MCP server
does not own or terminate hostd. Stdout contains only MCP responses.

```sh
bun test tools/scuba-engine-mcp
node tools/scuba-engine-mcp/integration.mjs /path/to/scuba-hostd /path/to/plugin-root
```

The integration starts real native hostd and the actual stdio server, exercises
hostd-backed tools and verifies that native-only methods are refused. It checks
shutdown, port closure and temporary-directory cleanup.
It requires the four built APP examples under the supplied plugin root.
The CTest `native_control_integration` separately starts the hidden WARP editor
and verifies native action discovery, selection and removal, camera readback,
MCP state reads, PNG capture and clean shutdown. This is synthetic local proof,
not in-game acceptance.

## Opt-in local developer tools

Add `--projects /absolute/path/projects.json` to expose `project_build`,
`project_check` and `project_test` alongside the fifteen default tools. Each takes
only `{ "project": "id" }`. The operator-owned configuration has version 1 and
at most 16 projects, each with `id`, absolute `cwd`, `timeout_ms` (100-1800000),
and `build`, `check`, `test` arrays containing an absolute executable followed
by fixed arguments. The configuration is trusted code execution authority;
keep it private and choose projects whose build scripts you trust.

The agent cannot supply a command, working directory or extra arguments. Runs
use no shell, capture at most 64 KiB, report exit code, timeout, truncation and
diagnostic lines, and terminate the process tree on timeout. No retry occurs.
Build failures become MCP tool errors with the structured result retained.
The default invocation exposes exactly fifteen tools. A passing local
build or checker does not certify game behavior.
