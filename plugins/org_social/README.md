# org_social

A native sco plugin with an org roster, party presence and shared waypoints, built on
`sco.net` 1.0, `sco.storage` 1.0, `sco.widgets` 1.0/1.1 and, when it is available,
`game.spatial` 1.0 `pose_to_zone`. Clean-room C11. GPL-3.0-only.

Nothing here has run in Star Citizen. Every test is headless, against the sco-core host kit,
`sco-plugin-check` or `sco-host-sim`, and uses synthetic data.

## Build

```sh
cmake -S plugins/org_social -B build -DSCUBA_SDK_ARCHIVE_DIR=<dir with the sdk-v2.4.0-rc.2 assets>
cmake --build build
ctest --test-dir build --output-on-failure
```

Leave out `SCUBA_SDK_ARCHIVE_DIR` and the build downloads `sco-sdk-host-source-sc-2.4.0.zip`
from the sco-core `sdk-v2.4.0-rc.2` pre-release (`cmake/ScubaSdk.cmake`). The archive's SHA-256 is checked either way.
The plugin is built with `sco_add_plugin` from the kit's `sdk/cmake/sco-plugin.cmake` and staged
as `build/stage/org_social/{org_social.dll,plugin.ini}`. The file is named `.dll` on every OS, as
the kit does it. The command above configures and tests only `org_social`. The repository root
also includes this plugin through `plugins/CMakeLists.txt`.

## Services (all optional)

| Service | Without it |
|---|---|
| `sco.storage` | The roster, waypoints and the share setting live in memory only; replies say "not saved" |
| `sco.net` | No party presence, no sharing; every member shows offline |
| `sco.widgets` | No panel; commands and raw handlers still work |
| `game.spatial` | Billboards resolve only positions already in the requested frame |
| sco_api 1.1 (`register_raw`) | No `*_json` raw handlers; the `org_social.billboards` command remains |

`plugin.ini` declares `api = 1.0` and no `requires`. Every 1.1 member is size-checked, and
`sco-plugin-check --short-tables` passes.

## Commands

| Command | Args | Effect |
|---|---|---|
| `org_social.member_add` | handle, rank | Adds a handle or sets its rank (`recruit`, `member`, `officer`, `director`, `founder`) |
| `org_social.member_remove` | handle | Takes it off the roster; a peer still in the session stays as a guest |
| `org_social.roster` | | `org N, online N, party N, waypoints N[ (no session)]` |
| `org_social.waypoint_add` | name, zone (int >= 0), x, y, z (float, metres) | Own waypoint, sent to the party when sharing is on |
| `org_social.waypoint_remove` | id | Removes an own waypoint |
| `org_social.self_pose` | zone, x, y, z | Supplied pose, sent on `org_social.pose` every 5 ticks while it changes. The product or another plugin feeds this; there is no automatic player observation here |
| `org_social.status` | `available` / `busy` / `away` | Announced in HELLO |
| `org_social.share` | bool | Share own waypoints (persisted as `cfg/share`) |
| `org_social.billboards` | | The billboards document for frame 0 with no camera, when it fits in the 256-byte reply; otherwise `{"schema":"org_social.billboards/1","truncated":true,"bytes":N,"raw":"org_social.billboards_json"}` |

Handles are 1-63 bytes and waypoint names 1-47 bytes. Neither may contain control bytes or
begin or end with a space.

## Raw handlers (sco_api 1.1, size handshake)

The output is UTF-8 JSON with no NUL. `SCO_TOO_MANY` sets `*out_size` to the bytes needed.

- `org_social.roster_json`: the input is ignored and the reply is `org_social.roster/1`.
- `org_social.billboards_json`: the input is ASCII `"<frame>"` or `"<frame> <cx> <cy> <cz>"`
  (decimal, space-separated). Empty input means frame 0 with no camera. Anything else is
  `SCO_BAD_ARG`. The reply is `org_social.billboards/1`.

### `org_social.roster/1`

```json
{"schema":"org_social.roster/1",
 "session":{"active":false,"self_peer":0,"status":"available","pose":null},
 "share_waypoints":true,
 "counts":{"org":1,"online":0,"party":0},
 "members":[{"handle":"Alpha","rank":"officer","in_org":true,"online":false,"peer_id":0,
             "state":"offline","declared_rank":null,"pose":null}],
 "waypoints":[{"owner_peer":0,"id":1,"name":"Rally","zone":0,"pos":[10,20,30]}]}
```

`rank` is the local roster's rank, or `guest` for a session peer who is not on the roster.
`declared_rank` is the rank the peer claims in HELLO. It is informational and never trusted.
`state` is `offline`, or the peer's declared status when it is online. `pose` is
`{"zone":Z,"pos":[x,y,z]}` or `null`.
Party and online counts describe remote peers, excluding self. Matching a session name to a
roster handle is display-only, not authenticated organization membership or an authorization
decision. Peer IDs are session-scoped; numeric IDs above JavaScript's safe-integer range need
an integer-preserving JSON parser.

### `org_social.billboards/1` (camera-facing labels for the studio viewport)

```json
{"schema":"org_social.billboards/1","frame":0,"spatial":"game.spatial","camera":[0,0,-10],
 "items":[{"kind":"waypoint","id":"wp:0:2","label":"Outpost","color":"#4fc3f7","zone":2,
           "resolved":true,"method":"pose_to_zone","pos":[2005,0,0],
           "distance_m":2005.02,"to_camera":[-0.99,0,-0.005]}]}
```

- `kind` is `waypoint` (own `#4fc3f7`, shared `#ffb74d`), `self` (`#81c784`) or `member` (org
  `#ba68c8`, guest `#90a4ae`; online peers with a shared pose only).
- `method` is `same_frame` (zone == frame), `pose_to_zone` (converted by game.spatial with an
  identity rotation) or `unresolved`. When it is `unresolved`, `pos`, `distance_m` and
  `to_camera` are `null`.
- `to_camera` is the unit vector from the item to the camera, given only when `camera` is set
  and the distance is above 1e-9. The viewport orients the sprite itself; no up axis is assumed.
- This is data for a renderer. The plugin draws nothing in-game.

## Message schema v1 (sco.net)

| Channel | Flags | max_len | Types |
|---|---|---|---|
| `org_social.roster` | `SCO_NET_RELIABLE` | 128 | HELLO, WAYPOINT_SET, WAYPOINT_DEL |
| `org_social.pose` | unreliable | 36 | POSE |

Every message starts with a 4-byte header: `u8 version = 1`, `u8 type`, `u16 body_length` (LE).
After the header comes the body. Integers are little-endian. `f64` is IEEE-754 binary64 sent as
its little-endian bit pattern. `str8` is a `u8` byte count followed by that many UTF-8 bytes with
no NUL.

| Type | Body |
|---|---|
| 1 HELLO | `u8 status` (0 available, 1 busy, 2 away), `u8 rank` (0 recruit .. 4 founder, 5 guest), `str8 handle` (1-63) |
| 2 WAYPOINT_SET | `u32 id` (!= 0), `u64 zone`, `f64 x`, `f64 y`, `f64 z` (finite), `str8 name` (1-47) |
| 3 WAYPOINT_DEL | `u32 id` (!= 0) |
| 4 POSE | `u64 zone`, `f64 x`, `f64 y`, `f64 z` (finite) |

The receiver drops a message if:
- the version or type is unknown,
- `body_length` does not match the bytes received,
- it has trailing bytes,
- a string is out of range or contains control bytes,
- a float is non-finite.

Waypoints are keyed by (sender peer, id). When a peer leaves, its waypoints and pose are dropped.
HELLO goes out when the session or peer list changes, and the sender's shared waypoints follow
it. sco.net messages are plaintext (`sco_net.h`): handles, ranks and positions are the only data
sent. Use synthetic/pseudonymous handles and do not enter secrets or personal details.
Messages on the wrong channel, from unknown peers, or HELLO handles inconsistent with the
session peer name are ignored. Declared ranks never modify local roster ranks.

The tests exercise the real kit loader, storage and widget renderer API; a synthetic net
service delivers deterministic session callbacks without opening sockets. They do not prove
multi-machine transport, live game coordinates, or viewport rendering.

## Storage keys (sco.storage key-value)

| Key | Value |
|---|---|
| `m/<handle>` | rank name |
| `w/<id>` | `"<zone> <x> <y> <z> <name>"` (`%.17g` floats) |
| `cfg/share` | `0` or `1` |

## Panel `org_social.roster`

The panel holds:
- a summary label,
- a roster table (Handle, Rank, Online, Status; paged at 10 rows on sco.widgets 1.1),
- a waypoint list,
- a handle input, a rank combo and add/remove buttons,
- a share toggle and a remove-waypoint button.

The buttons call the commands above through `invoke`, so the widget path and the command path
validate input the same way.
