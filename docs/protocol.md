# scuba-hostd bridge protocol

The studio (or any client) talks to `scuba-hostd` over one WebSocket at `ws://<host>:<port>/ws`
(default `ws://127.0.0.1:7878/ws`). Every message is one UTF-8 **text** frame holding one JSON
object. Binary frames close the connection with code 1003.

## Envelope

| Direction | Shape | Notes |
|---|---|---|
| client -> hostd: request | `{"id": <string or number>, "method": "<domain>.<method>", "params": <object or array>}` | `id` is required and echoed back. `params` is optional (defaults to `{}`). |
| hostd -> client: response | `{"id": <same id>, "result": <any JSON>}` | Exactly one response per request. |
| hostd -> client: error response | `{"id": <same id or null>, "error": {"code": <int>, "message": "<text>", "data": <optional>}}` | `id` is `null` only when the request could not be parsed or had no usable id. |
| hostd -> client: server event | `{"event": "<domain>.<name>", "data": <any JSON>}` | Pushed to every connected client; never has an `id`. |

A client tells responses from events by the presence of `event`. Requests are handled one at a
time, in arrival order per connection, on the host's main thread (the sco-core game thread);
responses may interleave with events.

Method and event names are dot-separated lower-case parts, each `[a-z][a-z0-9_]*`, at least two
parts (`system.ping`, `widgets.panel.render`). The first part is the owning module's domain.

## Error codes

| Code | Meaning |
|---|---|
| -32700 | parse error (the frame was not JSON) |
| -32600 | invalid request (not an object, missing `id` or `method`) |
| -32601 | method not found |
| -32602 | invalid params |
| -32603 | internal error (a handler threw something other than `HandlerError`) |
| -32000 | handler failed (generic module error) |
| -32001 | unavailable (the subsystem behind the method is not running) |

Modules may define further codes in -32099..-32002 and document them with their methods.

The studio client adds local-only codes that never appear on the wire: -1 timeout, -2
disconnected before the answer, -3 client closed (`studio/src/bridge.ts`, `ClientErrorCode`).

## Methods (foundation)

| Method | Params | Result |
|---|---|---|
| `system.ping` | `{"echo"?: any}` | `{"pong": true, "uptime_ms": number, "echo"?: any}` |
| `system.status` | none | `{"name", "version", "sdk_version", "uptime_ms", "clients", "kernel": {"started": bool, "plugin_root": string, "plugins": [{"folder", "id", "name", "version", "state", "reason"}]}}` |
| `system.methods` | none | array of every registered method name |
| `system.shutdown` | none | `{"stopping": true}`; only registered when hostd runs with `--test-shutdown` |

`kernel.plugins[].state` is sco-core's `StateName` (`off`, `disabled`, `refused`, `ready`,
`loaded`, `crashed`, `unloaded`, `faulted`).

## Events (foundation)

| Event | Data |
|---|---|
| `kernel.log` | `{"line": string}`: one sco-core log line (the lines sc-offline writes to mod.log) |

## Access control

`scuba-hostd` binds `127.0.0.1` by default and has no authentication, so it refuses requests a
browser could be tricked into sending:

- every HTTP request (including the upgrade) must carry `Host: 127.0.0.1:<port>`,
  `localhost:<port>`, `[::1]:<port>` or `<bind address>:<port>`; anything else gets 403 (DNS
  rebinding);
- a WebSocket upgrade that carries an `Origin` header must name `http://<one of those hosts>` or
  an origin passed with `--allow-origin`; anything else gets 403. Non-browser clients that send
  no `Origin` are accepted.

Binding a non-loopback address (`--bind`) exposes the unauthenticated bridge to that network.

## Limits

- One message (after reassembly of fragments) is at most 16 MiB; larger closes with 1009.
- Control frames follow RFC 6455 (ping is answered with pong; close is echoed).
- No extensions (no permessage-deflate) and no subprotocols are negotiated.
