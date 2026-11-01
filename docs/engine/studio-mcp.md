# scuba-engine-mcp: Studio MCP Server Architecture (K-11 / E-4), 2026-10-18 UTC

Decision and architecture specification for `scuba-engine-mcp`, establishing AI agent
tooling parity over the out-of-game sandbox and studio runtime.

## 1. Scope & Execution Home
- **Repository Home**: Lives directly in `scuba-engine` under `tools/scuba-engine-mcp/`.
- **Relationship to `sco-core-mcp`**:
  - `sco-core-mcp`: Static SDK catalogue, contract documentation, and symbol search. Read-only, offline, stdio.
  - `scuba-engine-mcp`: Interactive runtime tooling for the sandbox host (`scuba-hostd` WebSocket bridge) and asset pipeline.

## 2. Core Tool Definitions (Schema 1.0)

| Tool Name | Parameters | Behavior | Wire Backend |
|---|---|---|---|
| `host_status` | none | Inspect running hostd state, loaded plugins, kernel uptime | `system.status` |
| `invoke_command` | `name: string, args: array` | Call any registered plugin command on game thread | `plugins.invoke` |
| `get_widget_panel` | `panel_id: string` | Read complete widget tree and current state | `widgets.get` |
| `send_widget_event` | `panel_id: string, node_id: int, kind: string, text?: string` | Dispatch UI button click, text input, or tab select | `widgets.event` |
| `mock_move_camera` | `pos: [x,y,z], rot?: [x,y,z,w]` | Set synthetic viewport camera pose | `mock_game.camera.move` |
| `mock_set_service` | `service: string, published: bool` | Dynamically publish or withdraw mock service | `mock_game.services.set` |
| `mock_get_state` | none | Read synthetic scene state (entities, player, camera) | `mock_game.state` |
| `mock_list_services` | none | List mock services and published flags | `mock_game.services.list` |
| `mock_spawn_entity` | `class: string, zone: int, pos: [x,y,z], actor?: bool` | Spawn synthetic entity; returns `id` | `mock_game.entity.spawn` |
| `mock_move_entity` | `id: int, zone?: int, pos: [x,y,z]` | Move a synthetic entity | `mock_game.entity.move` |
| `mock_remove_entity` | `id: int` | Remove a synthetic entity (never the player) | `mock_game.entity.remove` |
| `mock_move_player` | `pos: [x,y,z], zone?: int, rot?: [x,y,z,w], camera?: bool` | Move the synthetic player | `mock_game.player.move` |
| `mock_set_capability` | `name: string, ready: bool` | Set one capability ready flag | `mock_game.caps.set` |
| `mock_apply_caps_profile` | `name: 'all' \| 'game_pack' \| 'none'` | Apply a capability profile | `mock_game.caps.profile` |
| `mock_reset` | none | Reset the synthetic scene to defaults | `mock_game.reset` |
| `convert_asset` | `path: string` | Clean-room conversion of Cry/OBJ/DDS/glTF assets | `assets.convert` |

## 3. Protocol & Transport
- **Transport**: Standard stdio JSON-RPC (MCP protocol version `2025-11-25`).
- **Loopback Bridge**: Connects to active `scuba-hostd` on `ws://127.0.0.1:<port>/ws`. Rejects foreign origins (Rule 11).

## 4. Implemented stdio bridge

`tools/scuba-engine-mcp/server.mjs` implements these tools with Node.js 22.12+
or Bun, without dependencies. The explicit endpoint accepts numeric IPv4 loopback
only. MCP initialization and the initialized notification precede tool calls.
The server forwards only the named tools (16), validates arguments and does
not retry mutating calls after a timeout or disconnect. EOF closes its bridge;
hostd remains independently owned.

`send_widget_event` also accepts value, index and row for the host's existing
toggle/slider, combo/list and page events. No tab event primitive is invented.
Hostd receives `panel` for events and `id` for panel reads. Runtime control is
opt-in: plugin commands, widget callbacks and conversion can mutate local state
or create output files. This is sandbox access, not game certification.

Run the tool directory's Bun tests and `integration.mjs HOSTD PLUGIN_ROOT` for
real stdio-to-native verification. See its README for client launch details and
failure semantics.
