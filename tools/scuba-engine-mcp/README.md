# Local sandbox MCP bridge

`scuba-engine-mcp` exposes eleven tools over newline-delimited stdio JSON-RPC,
using MCP protocol `2025-11-25`. Requires Node.js 22.12+ or Bun; no dependencies.
Start an existing `scuba-hostd` on loopback with the plugins you trust, then run:

```sh
node tools/scuba-engine-mcp/server.mjs --host ws://127.0.0.1:7878/ws
```

Initialize, send `notifications/initialized`, then use `tools/list` and
`tools/call`. The tools are `host_status`, `invoke_command`, `get_widget_panel`,
`send_widget_event`, `mock_move_camera`, `mock_set_service`, and `convert_asset`.
Their JSON Schemas are returned by `tools/list`. `panel_id` maps to hostd's `id`
for reads and `panel` for events; `service` maps to `name`.

Candidate SDK foundation hosts also support `mock_get_actor_stats`,
`mock_set_actor_stat`, `mock_get_container` and `mock_set_container`. These map to
the explicit synthetic-scene inspection/edit endpoints; they never read or write
the live game. Older hosts refuse missing methods explicitly. Container edits
validate the entire replacement before mutation; unknown-volume items require
zero unit_volume. Actor IDs/stat IDs and quantities use bounded integer schemas.

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
all tools, and verifies shutdown, port closure and temporary-directory cleanup.
It requires the four built APP examples under the supplied plugin root.
