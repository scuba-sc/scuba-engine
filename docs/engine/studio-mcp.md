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
| `plugin_commands` | none | Discover live command arguments and capability readiness | `plugins.commands` |
| `invoke_command` | `name: string, args: array` | Call any registered plugin command on game thread | `plugins.invoke` |
| `get_widget_panel` | `panel_id: string` | Read complete widget tree and current state | `widgets.get` |
| `send_widget_event` | `panel_id: string, node_id: int, kind: string, text?: string` | Dispatch UI button click, text input, or tab select | `widgets.event` |
| `mock_move_camera` | `pos: [x,y,z], rot?: [x,y,z,w]` | Set synthetic viewport camera pose | `mock_game.camera.move` |
| `mock_set_service` | `service: string, published: bool` | Dynamically publish or withdraw mock service | `mock_game.services.set` |
| `convert_asset` | `path: string` | Clean-room conversion of Cry/OBJ/DDS/glTF assets | `assets.convert` |

## 3. Protocol & Transport
- **Transport**: Standard stdio JSON-RPC (MCP protocol version `2025-11-25`).
- **Loopback Bridge**: Connects to active `scuba-hostd` on `ws://127.0.0.1:<port>/ws`. Rejects foreign origins (Rule 11).

## 4. Implemented stdio bridge

`tools/scuba-engine-mcp/server.mjs` implements these tools with Node.js 22.12+
or Bun, without dependencies. The explicit endpoint accepts numeric IPv4 loopback
only. MCP initialization and the initialized notification precede tool calls.
The server forwards only the eight named methods, validates arguments and does
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

## 5. Proposed next step: the agent control plane (E-15)

The eight tools above drive plugin panels, command discovery and synthetic game primitives. The studio's
own 26 controls (product profile, export, ASOP readback, capability admit/revoke, entity
injection, scene and inspector actions) are reachable only by OS pointer input today, which
makes native UI evidence the least durable kind we produce. The design for closing that -
one addressable action table that the ImGui UI renders from, built on the existing
`sco::Command` registry, exposed over WebSocket/MCP, `--drive` headless scripts and engine-side PNG
capture - is [agent-control-plane.md](agent-control-plane.md). It is additive (a read-only
`plugins.commands` discovery method plus five `engine.*` methods; 7 -> 13 bridge tools) and keeps
the loopback endpoint, argument-validation and no-retry-mutation rules unchanged. It adds no game
capability and requests no raw-ABI widening; the SDK asks it does make are listed in its section 8.
