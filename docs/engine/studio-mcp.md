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
| `convert_asset` | `path: string` | Clean-room conversion of Cry/OBJ/DDS/glTF assets | `assets.convert` |

## 3. Protocol & Transport
- **Transport**: Standard stdio JSON-RPC (MCP protocol version `2025-11-25`).
- **Loopback Bridge**: Connects to active `scuba-hostd` on `ws://127.0.0.1:<port>/ws`. Rejects foreign origins (Rule 11).
