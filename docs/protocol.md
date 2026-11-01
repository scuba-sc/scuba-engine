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

## Plugin commands (`plugins.invoke`)

`plugins.invoke {name:string,args?:array}` calls a registered SDK command on the game thread.
Result: `{result:string,reply:string}`, preserving the SDK result name (including refusals).
Arguments must match the command's declared SDK types: JSON strings, booleans, finite numbers,
or safe integers within +/-9007199254740991. Omitted `args` means an empty array.
Malformed arguments return -32602; an unknown command returns -32601; a missing synchronous
reply returns -32000. No shell, eval or source execution is involved.

Example: `{"id":1,"method":"plugins.invoke","params":{"name":"diegetic_asop.find_lift","args":["272"]}}`.
Widget panels for the APP plugins remain available through `widgets.list/get/event`.

## Kernel events

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

## Widgets (`widgets.*`, `src/hostd/widgets_bridge.{h,cpp}`)

`scuba-hostd` renders sco-core's renderer-neutral panels (the `sco.widgets` host
service, `include/sco/widgets.h`) through `StudioWidgetRenderer`, a `sco::widgets::Renderer`.
Every 50 ms on the game thread it calls `sco::widgets::RenderPanel` for each live panel and
publishes a panel again only when its snapshot was rebuilt (`sco::widgets::SnapshotBuilds()`
advanced). Studio events go back through the same `RenderPanel` call, so sco-core validates them
and calls the owning plugin exactly as an in-game renderer would.

### Panel snapshot

```json
{"id": "widgets.main", "title": "Widgets", "owner": "widgets", "revision": 7, "extended": false,
 "nodes": [{"id": 6, "parent": 1, "kind": "slider", "kind_code": 4, "disabled": false,
            "password": false, "label": "Amount", "text": "", "value": 0.5, "minimum": 0,
            "maximum": 1, "index": 0, "items": [], "columns": 0, "extended": false,
            "severity": "none", "tooltip": "", "first_row": 0, "visible_rows": 0}]}
```

- `nodes` is sco-core's preorder tree: `parent` 0 is the root, otherwise a preceding `row` or
  `column` node.
- `kind` is one of `label`, `button`, `toggle`, `slider`, `combo`, `list`, `text_input`,
  `table`, `row` or `column`.
- Base fields are always present; fields that don't apply to a node's kind are zero or empty.
  `items` holds combo/list options, or the row-major cells of a `table` with `columns` columns.
- The 1.1 node extension: `extended` is true when the plugin submitted a `sco_widget_node_1_1`.
  `severity` is `none`, `info`, `warning` or `error`, and `tooltip` is plain text.
- Paging (1.1, `list` and `table` only): when `visible_rows` > 0, show `visible_rows` rows
  starting at `first_row`.
- `revision` increases with every snapshot the bridge publishes, across all panels.
- A `text_input` flagged `password` is sent with `text` empty. The plaintext stays in the host.
- When the compiled host snapshot includes widget 1.3 numeric presentation, each node also
  carries `presentation`: `0` for NUMBER, `1` for INTEGER or `2` for INTEGER_PERCENT.
  Integer modes apply to sliders and require integral values. Older host snapshots omit
  this field; consumers must treat its absence as NUMBER. The Studio widget renderer uses
  whole-number steps for integer modes and a `%` suffix for INTEGER_PERCENT.

### Methods

| Method | Params | Result |
|---|---|---|
| `widgets.list` | none | `{"started": bool, "capabilities": {"service": "sco.widgets", "version": "1.2" or "1.3", "features": [...], "limits": {"max_panels", "max_nodes", "max_total_nodes", "max_items", "max_columns", "max_text", "max_events", "max_tag"}, "events": [...]}, "panels": [snapshot...]}` |
| `widgets.get` | `{"id": string}` | one snapshot |
| `widgets.event` | `{"panel": string, "node_id": int, "kind": string, ...payload}` | `{"result": "OK", "revision": number}` |

`widgets.event` takes one event `kind`, and each kind requires exactly its own payload:

| `kind` | Payload |
|---|---|
| `button` | none |
| `toggle` | `value`: 0 or 1 |
| `slider` | `value`, within `[minimum, maximum]`; integral for INTEGER/INTEGER_PERCENT presentation |
| `combo`, `list` | `index`: -1 or an item index |
| `text_input` | `text`: at most 255 bytes |
| `page` | `row`: the new first visible row of a paged list or table |

The plugin's callback runs before the response is sent. The response's `result` is sco-core's
`ResultName` for the dispatching render (`OK`, `CRASHED`, ...). A non-button event changes the
host-held value, so a `widgets.snapshot` event follows.

The bridge reports version `1.3` and feature `numeric_presentation` only when both the SDK
declaration and host snapshot field are available. Otherwise it retains version `1.2` and
omits that feature and field. The released RC2 host-source kit supports the 1.3 metadata.

Errors:

- -32602: malformed params, a kind that doesn't match the node, a value out of range, or a
  disabled node.
- -32010: no such panel or node.
- -32001: `sco.widgets` is not running.

sco-core validates the event again and silently drops what it refuses. For example, an event under
a disabled `row` is dropped, because the bridge does not check ancestors.

### Events

| Event | Data |
|---|---|
| `widgets.snapshot` | one panel snapshot (a new panel, or a changed one) |
| `widgets.removed` | `{"id": string}`: the panel is gone (removed, or its plugin unloaded) |
