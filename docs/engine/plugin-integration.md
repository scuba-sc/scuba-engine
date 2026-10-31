# First-party plugin integration map

This inventory separates source modules, shipped defaults, SDK contracts, and live provider
availability. It was checked against scuba-engine `56f5085`, sc-offline `96686b9`, sc-mp
`6308ed3`, and sco-core `0c977d3` on 2026-10-29. Those are source-tree revisions. Product
consumers still pin the published SDK 2.4.0 RC2 archive (`sco-sdk-2.4.0.zip`, SHA-256
`51a7ce266d087bcbeac39c92f26a6f905b4473883dd7ed6f0f5ca823fa88c88e`, host-source commit
`844e136ccb280ad8cb43f73bc43c9447e8f6799d`); the current core main source is not a replacement
published artifact.

## Inventory

Native modules use the SDK entrypoints `sco_plugin_query`, `sco_plugin_load`, and
`sco_plugin_unload`. The listed services describe runtime calls or optional integrations, not
loader `requires` declarations. A service name in a header or static catalogue does not establish
that a product publishes it or that its capability is ready.

### scuba-engine

All four first-party modules build as CMake targets in `plugins/`, stage into
`plugins/runtime/<id>/`, and are eligible for the headless host profile. Windows x64 is the
product DLL platform; the checked Linux build is a synthetic host/module run.

| ID / target / entry | Runtime services | Headless scope |
| --- | --- | --- |
| `mp_chat` / `mp_chat` / `mp_chat.dll` | `sco.net`, `sco.storage`, `sco.widgets`; all optional | Status, retained panel, local chat model; no network session is started |
| `org_social` / `org_social` / `org_social.dll` | `sco.net`, `sco.storage`, `sco.widgets`, optional `game.spatial` | Read-only roster command and panel; no peer session or pose sharing |
| `diegetic_asop` / `diegetic_asop` / `diegetic_asop.dll` | optional `game.hangar`, `game.asop`, `sco.widgets`; subscribes to `game.ready` | Status and terminal panel with game providers unpublished |
| `diegetic_shop_inv` / `diegetic_shop_inv` / `diegetic_shop_inv.dll` | optional `game.economy`, `game.loadout`, `game.prefabs`, `sco.datacore`, `sco.widgets`; subscribes to `game.ready` | Status and sample catalogue panel; no purchase, equip, or spawn action |

The engine ships an SDK `actor_stats_probe` example when the pinned kit has it. It is a test
fixture, not a first-party product plugin.

### Build the plugins with the published plugin SDK only

To compile the four first-party modules without the host-source SDK kit, extract the published
`sco-sdk-2.4.0.zip` from the
[`sdk-v2.4.0-rc.2` sco-core release](https://github.com/scuba-sc/sco-core/releases/tag/sdk-v2.4.0-rc.2)
and point `SCUBA_PLUGIN_SDK_ROOT` at the extracted `sco-sdk-2.4.0` directory:

```sh
cmake -S plugins -B build-plugin-sdk \
  -DSCUBA_PLUGIN_SDK_ROOT=/path/to/sco-sdk-2.4.0 \
  -DSCUBA_BUILD_TESTS=OFF
cmake --build build-plugin-sdk --target scuba_plugins sco-plugin-check
for plugin in mp_chat org_social diegetic_asop diegetic_shop_inv; do
  build-plugin-sdk/sco-plugin-check "build-plugin-sdk/runtime/$plugin"
  build-plugin-sdk/sco-plugin-check "build-plugin-sdk/runtime/$plugin" --short-tables
done
```

For the RC2 archive, verify SHA-256
`51a7ce266d087bcbeac39c92f26a6f905b4473883dd7ed6f0f5ca823fa88c88e` before extraction. CMake
checks the supplied directory's SDK version and required file layout; it does not verify archive
provenance, and `VERSION` alone cannot distinguish release candidates. The published package
contains plugin headers, CMake helpers, and `sco-plugin-check`, but not `sco_app` or
`sco-host-sim`. This mode therefore supports plugin builds and checker runs only; configure fails
clearly if `SCUBA_BUILD_TESTS` is enabled. Omit `SCUBA_PLUGIN_SDK_ROOT` to retain the default
host-source kit build and its host tests.

The positive consumer check is an opt-in root CTest so the normal suite stays unchanged. Configure
with `-DSCUBA_TEST_PLUGIN_SDK_CONSUMER=ON`; set `SCUBA_PLUGIN_SDK_ARCHIVE_PATH` to a local zip for
offline runs. If no local path is set, CTest downloads the RC2 asset and checks the same SHA-256
before extracting it. The test configures the standalone plugin project with tests off, builds the
four modules and checker, then checks each module with full and short tables. Run it with
`ctest --test-dir build -R '^plugin_sdk_public_archive_consumer$' --output-on-failure`.

### sc-offline

The offline owner confirmed the 11 native folders in the release staging roster:
`build`, `contracts`, `creative`, `discord`, `crew`, `spawn`, `mining`, `quantum`, `teleport`,
`inspector`, and `npc`. Their product entry names are `<id>.dll`; native CMake targets are
`sc-offline-<id>` (NPC and Inspector are wired by the top-level plugins CMake). The release is
Windows x64/MSVC. POSIX module/workflow builds are synthetic development evidence, not a game
DLL or gameplay result.

| Release ID | Runtime contract surface / dependency | Release state and safe conformance boundary |
| --- | --- | --- |
| `build` | `sco.content`; `game.spatial`, `game.world.raycast`, `game.world_control`, `game.entities`, and actor/entity ownership for placement/removal | Enabled folder; provider-gated, mutating placement is native-game-only |
| `contracts` | `game.missions`, `game.economy`, `sco.content` | Enabled folder; status/metadata can be checked without a mission provider; acceptance/progress/payout require live providers |
| `creative` | `game.creative` write surface, settings, optional widgets | Ships disabled; do not treat the SDK table or synthetic mock as user authorization |
| `discord` | `sco.settings` and local storage for diagnostics/preferences; no approved transport | Ships disabled; status-only until privacy/transport work is approved |
| `crew` | `game.actors` spawn/despawn and `game.vehicles` seat/flight-ready; entity observation | Enabled folder; native ownership and seat behavior require game acceptance |
| `spawn` | `game.actors`, `game.entities`, `game.vehicles` seat/flight-ready, `spawn.entities`, `teleport.spatial`, content/widgets | Enabled folder; mutation and streaming lifecycle are native-game-only |
| `mining` | `game.mining` | Release folder exists; setting `enabled` defaults false and capability remains fail-closed |
| `quantum` | `game.quantum` control, `game.spatial`, `game.relocation`, DataCore/storage/widgets for related travel/bookmarks | Enabled folder; control capability is off until a verified backend and cleanup path are ready |
| `teleport` | `game.relocation` plus the spatial/travel service surface and storage | Enabled folder; movement is admission- and capability-gated |
| `inspector` | Read-only `game.actors`, `game.entities`, `game.actor_stats`, `game.gamerules`, and `game.inventory` | Enabled folder; a safe headless snapshot can report unavailable sections; this does not establish native values |
| `npc` | `game.actors` spawn/despawn and `game.entities` transform/observation; optional content/widgets for pickers | Enabled folder; `npc.observe` is read-only; archetype spawn/removal needs a real game build |

Other source entries are not part of that 11-folder native release roster:

| Source entry | Classification |
| --- | --- |
| `gear_presets` | Development-only native module; reads inventory/layout and storage; no product default |
| `loadout` | Development-only native module; has `loadout_lists` data plugin; no product default |
| `titanlink`, `voxel_bridge` | Optional external bridges excluded from normal release staging |
| `inspector_lua` | Alternate non-default Lua implementation, not a second native Inspector release module |
| `picker_catalog`, `quantum_drive` | Data plugins/packs, not native DLL feature modules |
| `plugins/spatial` | Shared travel helper source, not a plugin ID or built module |

The offline source contains per-feature synthetic workflows and checker scripts, including
`tools/check_build_sdk.py`, `tools/contracts_sdk_check.py`, `check_crew_sdk.py`, `check_creative_sdk.py`,
`check_discord_sdk.py`, `check_inspector_sdk.py`, `check_mining_sdk.py`, `check_npc_sdk.py`,
`check_quantum_sdk.py`, `check_spawn_sdk.py`, and `check_teleport_sdk.py`. These compile/check
published SDK consumers and synthetic seams; they are not substitutes for Windows/game
acceptance. Current source also has test workflows for Crew, NPC, Spawn, Contracts, Quantum,
Teleport/travel, and metadata. They remain owned and runnable from sc-offline.

### sc-mp

`plugins/multiplayer` builds the single native plugin target `sc-offline-multiplayer`, product
entry `multiplayer.dll`. Its optional surfaces include `sco.net`, `sco.storage` 1.1,
`sco.widgets` 1.2, private-session control, and spatial/actor/entity/vehicle observations. The
separate relay executable is not a plugin and is not part of this engine profile. The plugin
does not start a network session at load; Host/Join require explicit action.

The sc-mp CMake build on Linux emits `multiplayer.so` while its copied manifest still says
`multiplayer.dll`. The synthetic host check used a temporary copied profile with the entry
adjusted to `.so`; no owner source or release manifest was changed. The Windows product artifact
uses the declared DLL entry.

## Runnable profiles and conformance

`tests/profiles/app_suite.json` loads the four engine plugins with only the synthetic
`game.events` seam and no synthetic game service providers. `profile_mcp_integration` checks the
loaded plugin set, read-only command replies, panel-to-plugin ownership, and that game providers
stay unpublished. It then shuts down cleanly, repeats the startup/teardown in a fresh host, and
verifies that a selected disabled plugin refuses startup. `scenario_profile` runs the synthetic
four-step scenario with the same four modules loaded. These are lifecycle and sandbox evidence,
not native game behavior.

`tests/profiles/scmp_multiplayer.json` and its expectations are a reusable external-module
profile. Run it after building sc-mp against the published RC2 archive and placing the resulting
plugin in a host root:

```sh
node tests/profile_mcp.mjs \
  /path/to/scuba-hostd /path/to/sc-mp-plugin-root \
  tests/profiles/scmp_multiplayer.json \
  tools/scuba-engine-mcp/server.mjs \
  tests/profiles/scmp_multiplayer_expectations.json
```

The profile calls only `multiplayer.status`, verifies `multiplayer.session` is owned by
`multiplayer`, checks stable tags on all 19 controls, tests restart and disabled-folder refusal,
and leaves all game service providers unpublished. It never invokes Host, Join, or movement.
The standard engine CTest does not depend on the separately built sc-mp module.

The MCP widget snapshot now exposes the SDK's stable 1.2 `tag` and reports that feature in its
capabilities. Numeric presentation remains outside this snapshot contract. Native SDK widget
tests still verify callback dispatch and reject invalid panel events.

### Shared-service readiness reconciled with sco-core

Public RC2 headers/status metadata do contain `game.missions`, `game.mining`, `game.relocation`,
`game.quantum`, and `game.inventory.layout`. Current main likewise contains their status rows.
The gap is provider readiness/admission, not a missing name in the published C ABI:

| Consumer | Public contract and current status | Work still needed |
| --- | --- | --- |
| Contracts | `game.missions` 1.0 and `game.economy` are exposed; mission capabilities default OFF in the registry. Core source has an offline mission provider, but it only enables capabilities for a validated/admitted backend. | Core/offline owners must confirm a product-bound ready backend and prove status/offer/accept/progress behavior. Static registry presence is not availability. |
| Mining | `game.mining` 1.0 exists; capability is OFF by default and the product setting defaults false. | Core must prove an admitted adapter and owner cleanup; offline must keep the disabled/fail-closed default until then. |
| Gear presets | RC2 `sc_inventory.h` exposes `game.inventory.layout`; registry marks the capability OFF. | There is no ready product table/provider in the current consumer path. Track this as a provider gap, not as an absent RC2 header. The consumer is development-only. |
| Teleport | RC2 has `game.relocation` 1.1; the product path is gated by offline admission and the relocation/position capabilities. | Confirm product admission/capability state and native movement acceptance; do not infer support from the registry row. |
| Quantum | RC2 has `game.quantum` 1.0; `game.quantum.control` defaults OFF. | Core needs a verified control backend plus cleanup/admission; offline controls remain gated. |
| Build | Public `sco.content`, world-control, raycast, spatial, and owned entity interfaces exist. | The live native provider must publish each required capability; ray/entity operations remain game-only. |
| Crew / Spawn / NPC | Public actor/entity/vehicle services and capability rows exist, including NPC ownership, seat, and flight-ready paths. | Verify provider cleanup, streamed-state semantics, seat authority, and exact-build behavior in game. CTest/mock success is not game proof. |
| Inspector | Read-only actor/entity/stat/gamerules/inventory contracts exist, with some capabilities off or experimental. | Report section-level unavailable/unknown values until a real provider succeeds; do not fabricate zero/default readings. |
| Creative | `game.creative` is in the public contract; offline release folder is disabled. | Keep write behavior disabled at the product layer pending owner policy. SDK availability alone does not turn it on. |

The sco-core catalogue and registry MCP tools publish static SDK metadata, not live connected
provider state. A runtime status source must separately query the host/service tables and
capabilities.

## Cross-repository handoffs

- **sco-core:** reconcile live provider/admission state for missions, mining, relocation,
  quantum, inventory layout, and the read-only Inspector services. Keep the existing plain-C
  `sco_api` ABI 1.1/128-byte table unchanged; new service tables must remain versioned,
  size-prefixed, owner-scoped, and have explicit cleanup semantics.
- **sc-offline:** the owner reports the current local release-roster patch centralizes the 11
  staged modules in `tools/release-plugin-profiles.json` with manifest/module/default validation.
  That patch is not present in the checked `origin/main` tree, so the engine does not copy or
  duplicate its source. Continue to surface exact service capability gates in product status.
- **sc-mp:** keep the relay separate from the plugin profile; resolve Linux staged artifact
  naming so its module suffix and `plugin.ini` entry agree. The host can load the plugin and keep
  the panel, but game movement/entity behavior remains untested here.

No in-game testing, release choice, or published-artifact pin change is part of this profile.
