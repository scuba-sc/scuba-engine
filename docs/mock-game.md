# Synthetic game services

`scuba-hostd` publishes an editable, entirely synthetic game scene by default.
There is no game image, signature scan, proprietary fixture, engine call, or
network session behind these tables. Nothing here has been run in Star Citizen.
Use `--no-mock-game` to omit the synthetic provider.

The pinned host-source kit is SDK 2.4.0 release candidate 2
(`sdk-v2.4.0-rc.2`). The inventory below describes registered synthetic tables,
not real game adapters. Publication can fail or be switched off individually;
query `mock_game.services.list` for the actual publication state. Actor-stat
tables are compiled and registered only when the kit supplies their headers.
`sc_game_events.h` defines bus payloads, **not** a `game.events` service table.
`game.asop` and `game.contracts` (new in 2.3.0) are read-only tables with fixed
synthetic values. Neither value comes from the game.
The wire/IPC headers `sc_net.h`, `sc_ipc.h`, `sc_titanlink.h`, and
`sc_voxel_bridge.h` do not declare additional game service tables.

## Build and exercise

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/scuba-hostd --plugins ./plugins
```

The foundation's CMake downloads the released host-source archive with TLS and
checks its pinned SHA-256. For local builds, the optional
`SCUBA_SDK_ARCHIVE_DIR` directory supplies the same archive; hashing is still
mandatory. Kit sources stay in the ignored build tree.

`mock_game_plugins` loads the compiled native `mock_game_probe.dll` through
the real kit loader and exercises the original mock service tables. Separate
rc2 tests exercise newer tables and tails. Its commands run a seed phase, two
explicit host ticks (streaming and watch delivery), and an
assertion phase. Coverage includes result codes, text-size handshakes, wrong
threads, missing classes/entities, ownership, retained removals, keyed replay,
capability changes visible through the **plugin's** `api->has()`, game events,
reload cleanup, spatial pages/epoch expiry/unknown tails, and quotas. No sleeps
or timing-luck waits are used. The existing hostd WebSocket e2e test also moves
the player, toggles a capability, and fires death/respawn through `/ws`.

## Implementation and lifecycle

`src/hostd/mock_game/mock_game.h` exposes `Configure(Platform&)`, `Publish`,
`Unpublish`, `ResetAll`, capability/profile helpers, and service publication
helpers. `Kernel::Start` calls `Configure` before `sco::app::Start`.

`Configure` installs the early `Platform::setCapabilities` hook to publish the
tables before discovery. A built-in initializer named `scuba_mock_game` runs
before external plugins: it restores the synthetic private-session capability
after the kit's real private-session startup disables it. The real broker is
disabled. The built-in has no plugin-owned service; tables use the kit's
never-released `game` owner.

One release hook removes a plugin's scene ownership, watches, receipts, and
leases. Normal unload/reload restores creative toggles, cvars, and the exact
captured camera state. Entities/NPCs/prefabs owned by the plugin disappear on
the next tick; kept entities persist without SDK removal authority.
`spawn_as` entities persist but lose their move/seat/power authority. A release
handle never becomes valid again. The kit unloads plugins before withdrawing
the tables. `Unpublish` clears synthetic capabilities.

All scene access and table methods run on the kit's game thread (the hostd main
thread). Calls elsewhere return `SCO_WRONG_THREAD`, or zero/error text for the
legacy boolean/string tables. `last_error` is callable on any thread and uses a
thread-local refusal message off the game thread. Unknown/released mutation
handles return `SCO_BAD_ARG`; caller buffers and size-prefixed records follow
the corresponding header's zeroing/unchanged-output rules.

`common.h` contains `GetScene()` and the scene structures for other C++ lanes.
Coordinates use metres and double precision; quaternions are xyzw and normalized.
Zone frames are **translation-only**, with conversion through a common ancestor.
This is not an engine transform implementation.

## Default scene

All IDs below are synthetic, session-volatile numbers, not persisted keys.

| Kind | ID | Description |
|---|---:|---|
| Zone | 1 | `MockSystem`, outermost observed root |
| Zone | 2 | `MockSystem_Planet`, parent 1, offset `[2e10,0,0]` |
| Zone | 3 | `MockSystem_Station`, parent 1, offset `[-3e8,5e7,0]` |
| Zone | 4 | `MockSystem_Station_Hangar_01`, parent 3, offset `[120,-40,8]` |
| Zone | 5 | `MockSystemFar`, separate root; cross-root relocation refused |
| Player entity / actor | 256 / 37120 | Spawned at `[0,0,0]` in zone 4; health 100 |
| Player vehicle | 257 | `MOCK_Ship_Starter`; initially not aboard |
| Seats | 258–260 | Usable pilot/copilot and unusable remote turret |
| Landing area / lift | 272 / 273 | Paired ship lift |
| ATC | 274 | Has a pending request initially |
| Unpaired landing area | 275 | Lift lookup returns `SCO_NOT_FOUND` |
| Guard / crate | 288 / 289 | Synthetic actor and prop |

Spawnable catalog: `MOCK_Ship_Starter`, `MOCK_Ship_Fighter`, `MockNPC_Guard`,
`MockNPC_Civilian`, `MockProp_Crate`, `MockProp_Barrel`.
The host scene-spawn handler adds its class to the catalog. Unknown plugin
spawn classes return `SCO_NOT_FOUND`; real-game class discovery is not simulated.
Fresh spawns stream in after one explicit tick; removals initiate now and finish
on the next tick. `Scene::streamTicks` can be changed by C++ test/host consumers.

Loadout items: `MockHelmet_Basic`, `MockArmor_Torso_Light`,
`MockArmor_Arms_Light`, `MockArmor_Legs_Light`, `MockUndersuit_Basic`,
`MockBackpack_Small`, `MockVisor_Tinted`, and for the 1.1 weapon slots
`MockRifle_Basic`, `MockPistol_Basic`, `MockOptic_Holo`, `MockMultitool_Basic`,
`MockTool_Module_Mining`.
ASOP: claim timeout 30 s, shard persisted. Mission stream radius 5000.
`Asop()` and `Contracts()` in `services.h` let C++ tests edit them or mark the
configuration not loaded (`SCO_NOT_FOUND`).
Prefab catalog: `ObjectContainers/mock/crate_stack.socpak` and
`ObjectContainers/mock/outpost_small.socpak` (names only, no assets/files).
Cvars: `mock_hud_enabled=1`, `mock_debug_draw=0`, `mock_volumetric_fog=1`.
Initial local economy balance is 25000.

## Table inventory and synthetic behavior

Each row names its implementation file under `src/hostd/mock_game/`.
Versions are query-service arguments; the table's first field is its full size.

| Table | Version | File | Members/behavior |
|---|---|---|---|
| `game.actor_stats` | 1.1, conditional | `actor_stats.cpp` | Synthetic actor stat reads when actor-stat headers are available |
| `game.actor_stats_write` | 1.0, conditional | `actor_stats.cpp` | Bounded local-player writes subject to read/write capabilities, mode and offline admission |
| `game.actors` | 1.1 | `actors.cpp` | Player IDs; owned NPC spawn/despawn; health, actor state, errors |
| `game.asop` | 1.0 | `asop.cpp` | Synthetic claim timeout and shard-persisted flag, read only, errors |
| `game.asop.selection` | `SC_ASOP_SELECTION_VERSION` | `asop_selection.cpp` | In-memory selection document, catalogue pages, editor lease, tick-driven receipts; no real file |
| `game.audio` | 1.1 | `audio.cpp` | Synthetic trigger resolution and owner-scoped playback handles; no audio output |
| `game.build` | `SC_GAME_BUILD_SERVICE_VERSION` | `build.cpp` | Synthetic build observation |
| `game.contracts` | 1.0 | `contracts.cpp` | Synthetic mission stream radius, read only, errors |
| `game.creative` | 1.0 | `creative.cpp` | Four reversible flags, bounded fly speed, flag query, errors |
| `game.cvars` | 1.0 | `cvars.cpp` | Exact-name bool observations; unknown name `NOT_FOUND` |
| `game.cvars_write` | 1.0 | `cvars_write.cpp` | Exclusive per-name claim, canonical bool writes, first-value restore |
| `game.datacore_live` | `SC_DATACORE_LIVE_SERVICE_VERSION` | `datacore_live.cpp` | Small synthetic read-only pools and record; writes refused |
| `game.economy` | 1.0 | `economy.cpp` | Balance, bounded once-only apply/receipt, synchronous transaction event |
| `game.entities` | 1.2 | `entities.cpp` | Alive, class, poses, owned spawn/despawn, keep, watches, radius queries, class catalog and component records; instance-name lookup refused |
| `game.spatial` | 1.0 | `game_spatial.cpp` | Normalized full-pose conversion, zone records/find/pages; observed ancestry only |
| `game.gamerules` | 1.1 | `gamerules.cpp` | Synthetic state and declared parameter pages |
| `game.hangar` | 1.0 | `hangar.cpp` | Landing-area lift lookup, open/close intent, ATC cancellation, errors |
| `game.input` | `SC_INPUT_SERVICE_VERSION` | `input.cpp` | Static action declaration registry |
| `game.inventory` | 1.2 | `inventory.cpp` | Synthetic player snapshot; layout snapshot refused |
| `game.inventory.contents` | 1.0 | `inventory_contents.cpp` | Synthetic container snapshots, subject to availability |
| `game.loadout` | 1.2 | `loadout.cpp` | Typed batch equip with per-slot applied/missing/unsupported results; weapon and multitool slots 8-21 also need `game.loadout.weapons` (else that slot stays `UNAVAILABLE`), 22+ `UNSUPPORTED`; tick-driven submit/status/cancel jobs |
| `game.mining` | 1.0 | `mining.cpp` | Owner request/state, tick-driven synthetic cell/promotion counters |
| `game.missions` | 1.0 | `missions.cpp` | Offers, owner-scoped acceptance/status/progress/abandon, bounded script ingestion |
| `game.owned` | 1.0 | `owned.cpp` | Retain/remove build, owned NPC/seat-NPC removal, receipt/forget |
| `game.prefabs` | 1.0 | `prefabs.cpp` | Canonical object-container path lookup and entities-owned spawn |
| `game.private_session` | 1.0 | `private_session.cpp` | Consent-bound host/join/leave/state; no sockets, synthetic peers |
| `game.quantum` | 1.0 | `quantum.cpp` | Enable/hold/release/state; 10 charging ticks, five boosting ticks |
| `game.relocation` | 1.0 | `relocation.cpp` | Pending request/cancel/status, tick-driven movement then pose confirmation |
| `game.reputation` | 1.0 | `reputation.cpp` | Four synthetic intact check sites and services-present observation, errors |
| `game.vehicles` | 1.0 | `vehicles.cpp` | Player ship, seats/occupant, allowed actor seating/ejection, Flight Ready, errors |
| `game.world` | 1.1 | `world.cpp` | Sphere/ground-plane raycast, camera query, free-camera flag, errors |
| `game.world.control` | 1.1 | `world_control.cpp` | Owned-entity filtered ray; exclusive, bounded, reversible camera lease; multi-hit ray requires a capability not in the mock capability registry |
| `spawn.entities` | 1.2 | `spawn.cpp` | Near-player and owner-tagged spawn, class/ID/alive queries, authorized moves |
| `teleport.spatial` | 1.0 | `teleport_spatial.cpp` | Player pose, entity zone, local/world/zone positions, copied zone name |

Some registered members deliberately refuse rather than invent data: valid
`game.entities` instance-name lookup and `game.inventory` layout snapshot
return `SCO_UNAVAILABLE`, as does `game.datacore_live` pool write (off-thread
calls return `SCO_WRONG_THREAD`). `game.world.control` multi-hit ray checks
`game.world.raycast_filtered_many`, which is not in `kCaps` and therefore cannot
be enabled through the mock capability controls. Registration alone does not
make every operation available. A disabled capability can cause associated
calls to refuse; not every service checks its advertised capability (notably
`game.asop.selection`). Readable control
state and cleanup calls (unwatch, restore, lease release) remain available where
needed to explain or undo state. Economy notification reentry is refused.
`game.owned` removals also propagate disabled prerequisite actor/entity/seat
capabilities. A missing published table yields `SCO_NOT_FOUND` at query time;
an unsupported version yields `SCO_UNAVAILABLE`.

Deliberately reserved fields remain faithful to this kit: actor maximum health
is 0 (unknown); ray hit entity/normal/flags remain zero; zone `system_id` is 0
with `SYSTEM_UNKNOWN`; removal receipts stay `REQUESTED` even after synthetic
disappearance. Relocation uses the explicitly synthetic scene roots as its
certified system model, not these observation records as engine evidence.
The mission format accepted is exactly:

```text
sco-mission 1
phases 2
objectives 1
```

A final LF is required. Phase/objective counts are 1..64, paths are
`missions/<lowercase ASCII id>.scom`, and session registration is immutable.
Repeated identical script ingestion is idempotent; edited replay fails.
Script content is declarative and never executed.

## Bridge methods

Use the existing `/ws` envelope `{id,method,params}`; responses are `{id,result}`
or `{id,error:{code,message}}`. Invalid parameters use -32602; a mock disabled
with `--no-mock-game` uses -32001. Scene changes broadcast
`{event:"mock_game.changed",data:{what}}`; it is an invalidation notice, not a
full snapshot. Read `mock_game.state` again.

| Method | Params | Result |
|---|---|---|
| `mock_game.state` | none | `{player,camera,zones,entities,economy_balance,tick}` |
| `mock_game.caps.list` | none | `[{name,service,ready,game_pack_default}]` |
| `mock_game.caps.set` | `{name,ready:bool}` | Current capability list |
| `mock_game.caps.profile` | `{name:"all"\|"game_pack"\|"none"}` | Current capability list |
| `mock_game.services.list` | none | `[{name,version,published}]` |
| `mock_game.services.set` | `{name,published:bool}` | `{name,published}` |
| `mock_game.player.move` | `{pos:[x,y,z],zone?,rot:[x,y,z,w]?,camera?:bool}` | `{fired}` (zone-change event) |
| `mock_game.camera.move` | `{pos:[x,y,z],rot:[x,y,z,w]?}` | `{pos}` (world frame) |
| `mock_game.event.player_spawned` | `{zone?,pos:[x,y,z]?}` | `{fired}` |
| `mock_game.event.player_died` | none | `{fired}` |
| `mock_game.event.actor_died` | `{entity}` | `{fired}` |
| `mock_game.event.vehicle_seated` | `{boarded?:bool,ship?:257,seat?:0}` | `{fired}` |
| `mock_game.entity.spawn` | `{class,zone,pos:[x,y,z],actor?:bool}` | `{id}` (host-created, unowned entity) |
| `mock_game.entity.remove` | `{id}` | `{removing:true}` |
| `mock_game.entity.move` | `{id,zone?,pos:[x,y,z]}` | Entity snapshot |
| `mock_game.session.add_peer` | none | `{added:true}` |
| `mock_game.economy.set_balance` | `{balance}` | `{balance}` |
| `mock_game.reset` | none | Fresh state snapshot |

IDs fit the exact JSON integer range (0..2^53-1). Player move positions use the
specified zone; `camera:true` also moves the camera to player eye height in the
world frame. Camera movement is otherwise independent of the player.

The default `all` profile enables registered capabilities with `defaultOn` set;
when actor stats are compiled in, `game.actor_stats.write` is explicitly off
by default even under `all`. `game_pack` uses the `gamePack` flags in
`mock_game.cpp` as a **header-derived testing profile**, not a claim about live
engine readiness. `none` disables all registered capabilities. These profiles
only change entries in `kCaps`; they do not guarantee every member is usable.
Boolean/string
legacy services retain their own ABI return conventions.

Event commands send the SDK payload structs on the kernel bus, gated separately
by `game.events.player_spawned`, `player_died`, `actor_died`, `zone_changed`,
and `vehicle_seat`. Local player death sends both `game.actors.died` and
`game.player.died` (each independently gated); killer IDs stay 0. Seating sends
`game.vehicle.boarded`; unseating sends `game.vehicle.exited` using
`sc_game_vehicle_seat`. There is no invented SDK `game.vehicle.seated` event.
`fired:false` means event capability suppression (or no zone transition); it
does not undo the corresponding synthetic scene edit.

Reset is destructive to the mock: it drops service-owned scene state and
registrations while plugins remain loaded. Session/epoch generations expire
and cached entity/mission IDs must be discarded; use reload if the plugin
does not handle a host scene reset.

## Simulation limits

No game assets, persistence, DataCore-backed catalogs, rendering, collisions
beyond synthetic spheres/ground, entitlement state, account balances, real
mission broker, or authenticated multiplayer are provided. Creative flags do
not implement player physics/ammunition consumption. Private-session state
does not authenticate peers. Mining counters and quantum progression are
tick-count driven. These are deterministic test doubles, not game adapters.

Linux GCC and Windows MSVC have not been run locally; their first compilation
is the repository's future CI. The new implementation uses standard C++20 and
the portable kit/bridge API, with no new platform-specific system calls.
