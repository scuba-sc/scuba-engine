# diegetic_asop

An ASOP and hangar lift terminal for sco-core hosts. It shows a `sco.widgets` panel built with
`scosdk::PanelBuilder` (`include/scosdk/menu_builder.hpp`) and exposes the same actions as commands,
driving the `game.hangar` 1.0 service (`include/sc_hangar.h`). The panel is meant for the in-game menu
and for scuba-engine's diegetic kiosk preview.

Nothing here has run in Star Citizen. It has been exercised against the SDK's `sco-plugin-check`
stand-in host, the kit's `sco-host-sim` (the real host kit, no game), and a unit test with a
synthetic `game.hangar` table.

## What it does

| Panel control / command | game.hangar call | Gate |
|---|---|---|
| Find lift / `diegetic_asop.find_lift <landing_area>` | `lift_of_landing_area` | `has("game.hangar.lift")` + table size |
| Open lift / `diegetic_asop.lift_open <lift>` | `lift_request(OPEN)` | `has("game.hangar.lift")` + table size |
| Close lift / `diegetic_asop.lift_close <lift>` | `lift_request(CLOSE)` | `has("game.hangar.lift")` + table size |
| Cancel ATC request / `diegetic_asop.atc_cancel <atc>` | `atc_cancel_request` | `has("game.hangar.atc_cancel")` + table size |
| Show last error / `diegetic_asop.last_error` | `last_error` (size handshake) | table size |
| `diegetic_asop.status` | none | none |

Ids are entity ids, typed as decimal or `0x` hex. A successful Find lift fills the lift id field.

Every call is gated in this order:

1. `sco_api` covers `query_service`. On a 1.0 host the plugin runs commands-only and has no panel.
2. `game.hangar` 1.0 answers `query_service`.
3. `has(<capability>)` answers 1. Both capabilities are off by default in the game pack.
4. The provider's `size` covers the function, checked before the pointer is read.

A failed gate answers `SCO_UNAVAILABLE` with the reason and does not touch the game. A failed call
reports the result plus `last_error`'s text.

## game.asop

sco-core 2.3.0 ships `sc_asop.h`, but the mock game does not publish `game.asop` yet (roadmap E-2). On every panel refresh, including
`game.ready`, the plugin queries `game.asop` 1.0. The ASOP section appears only when that query
answers a table. Even then the plugin reads only the table's leading `size` field and calls nothing
in it. When the query fails, the section stays inert and adds no panel nodes. The ASOP calls get
wired once the public header ships.

## Build and test

From the scuba-engine root (the plugin is part of the default build):

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build -R diegetic_asop --output-on-failure
```

The plugin folder is laid out at `build/plugins/diegetic_asop/root/diegetic_asop/`, the shape
sc-offline's `data/plugins/` expects. `cmake --install` places it under `data/plugins/diegetic_asop`.
Manual checks:

```sh
build/plugins/sco-plugin-check build/plugins/diegetic_asop/root/diegetic_asop
build/plugins/sco-plugin-check build/plugins/diegetic_asop/root/diegetic_asop --short-tables
build/_sdk/build/sco-host-sim build/plugins/diegetic_asop/root --no-lua --invoke diegetic_asop.status
```
