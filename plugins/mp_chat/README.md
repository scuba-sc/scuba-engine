# mp_chat

Room-aware multiplayer chat as a native sco plugin (C11), built against the sco-core
`host-source-sc` 2.4.0 kit (release candidate 2) with the kit's `sdk/cmake/sco-plugin.cmake`.

- **sco.net 1.0**: one reliable channel `mp_chat.msg`. Subscribes to `net.peer` (joined/left
  notices) and `net.state` (session started/ended).
- **sco.storage 1.0**: chat lines are kept in the plugin's own SQLite table `mp_chat_history`, at
  most 256 per room. The current room is stored under the key `current_room`.
- **sco.widgets**: a panel `mp_chat.main` with a status label, a room combo, a "Clear history"
  button, a message list, a `TEXT_INPUT` and a "Send" button. On a 1.1 host, chosen by checking
  the table size and then `capabilities()`, the list is paged at 8 rows and handles
  `SCO_WIDGET_EVENT_PAGE`. The status label and the list carry severity badges, and status/list
  refreshes go through `update_nodes`. A 1.0 host gets the same tree with 1.0-sized nodes and no
  paging.

Every service is optional. Without sco.net, the panel and history still work and sending answers
`SCO_UNAVAILABLE` with a warning notice. On a sco_api 1.0 host only the commands load. The plugin
never opens, joins or leaves a session: that is the product's decision.

## Commands

| Command | Args | Reply |
| --- | --- | --- |
| `mp_chat.send` | `text` (1-200 bytes UTF-8, no control characters) | `sent to #<room>`; `SCO_UNAVAILABLE` with no session |
| `mp_chat.join` | `room` (1-24 of `a-z 0-9 _ -`) | `now in #<room>`; creates the room locally (at most 8) |
| `mp_chat.status` | none | `room=<r> rooms=<n> lines=<n> session=on\|off peers=<n> net=yes\|no storage=yes\|no widgets=none\|1.0\|1.1` |
| `mp_chat.last` | none | the newest chat line of the current room, `<sender>: <text>`, or empty |
| `mp_chat.clear` | none | `cleared #<room>`; clears the room locally and in storage |

## Wire format (channel `mp_chat.msg`, reliable, max 230 bytes)

`'M' 'C' version=1 room_len(u8) text_len(u16 LE) room text`.

Incoming messages are validated before use: the room must match the room rule and the text must
be well-formed UTF-8 with no controls. Anything else is dropped and counted. The sender's name
comes from sco.net (`get_peer_name`), never from the payload. sco.net messages are authenticated
but not encrypted, so do not send anything private.

## Build and test

```
cmake -S plugins -B build-plugins -DSCUBA_SDK_ARCHIVE_DIR=<dir with the sdk-v2.4.0-rc.2 assets>
cmake --build build-plugins --target mp_chat mp_chat_model_test mp_chat_host_test
ctest --test-dir build-plugins -R '^mp_chat_' --output-on-failure
```

Without `SCUBA_SDK_ARCHIVE_DIR` the kit is downloaded from the sco-core `sdk-v2.4.0-rc.2` pre-release, and
its SHA-256 is checked either way (`cmake/ScubaSdk.cmake`). The built plugin folder is
`build-plugins/mp_chat/root/mp_chat/` (`plugin.ini` + `mp_chat.dll`). Copy it into a host's
`data/plugins/`.

The tests:

- `mp_chat_model`: the wire codec, UTF-8 validation, rooms, history ring and paging.
- `mp_chat_check`, `mp_chat_check_short_tables` and `mp_chat_check_invoke_join`: the kit's
  `sco-plugin-check` stand-in host, run as is, with `--short-tables` (a 1.0 host), and with an
  invoke.
- `mp_chat_host_sim`: the kit's `sco-host-sim`.
- `mp_chat_host`: the real host kit, headless, driving the panel through
  `sco::widgets::RenderPanel`, plus a storage restart.
- `mp_chat_loopback`: two processes chatting over sco.net on 127.0.0.1, then reading the history
  back after a restart.

None of this has run inside the game.
