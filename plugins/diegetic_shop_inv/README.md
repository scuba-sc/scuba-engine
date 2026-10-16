# Shop & Loadout sample plugin

GPL-3.0-only. Native C++20 plugin, id `diegetic_shop_inv`, API 1.1. A
renderer-neutral `scosdk::PanelBuilder` panel (`diegetic_shop_inv.shop`) browses
32 **synthetic sample items** in Armor, Weapons, Components and Prefabs.
Names, prices, classes, record names and container paths are all fictional.
There are no game files or store extracts in this plugin.

The item list uses widgets 1.1 paging (8 visible rows), with a category selector,
item details, local balance, zone-id input, and Buy/Equip/Spawn buttons. A widgets
1.0 host gets the same list without paging attributes; an API 1.0 host gets
commands only. Buttons report unavailable reasons rather than bypassing gates.
Items not supported by the typed loadout slots cannot be equipped.

## Build and test

From the repository root:

```sh
cmake -S plugins -B build-app4
cmake --build build-app4 --target test_shop -j 12
ctest --test-dir build-app4 --output-on-failure
build-app4/diegetic_shop_inv/sco-plugin-check build-app4/diegetic_shop_inv/root/diegetic_shop_inv
build-app4/diegetic_shop_inv/sco-plugin-check build-app4/diegetic_shop_inv/root/diegetic_shop_inv --short-tables
```

`plugins/CMakeLists.txt` is a standalone configure entry point as well as an
add-subdirectory registry. It reuses `cmake/ScubaSdk.cmake` to download the
published `sdk-v2.3.0` host-source kit and verify SHA-256
`0e9f45bc9affb32cf0d158eec396862bf1943d556cc120cf10d5b027e0f96f36`.
For offline development, add `-DSCUBA_SDK_ARCHIVE_DIR=<release-payload-directory>`
to the configure command; this option still verifies the archive hash.
No root CMake or hostd files are changed in this lane. An integrating lane
that builds plugins through the repository's root must add `add_subdirectory(plugins)`.

The kit's `sco_add_plugin` helper builds
`build-app4/diegetic_shop_inv/root/diegetic_shop_inv/{plugin.ini,diegetic_shop_inv.dll}`.
The `.dll` suffix is intentional on all platforms. Installation through CMake
puts it under `data/plugins/diegetic_shop_inv`.
With tests disabled, build target `diegetic_shop_inv` instead of `test_shop`.
For multi-config generators, build with `--config Release` and test with `-C Release`.

## Commands

All names have the prefix `diegetic_shop_inv.`. Integer pages are 1-based and
clamped; command lists contain 4 items per page to fit the host's reply buffer.

| Suffix | Arguments | Meaning |
| --- | --- | --- |
| `status` | none | Service presence and capability summary |
| `categories` | none | Counts in the four sample categories |
| `list` | `page:int` | Current filter's catalogue page |
| `filter` | `category:string` | `all`, `armor`, `weapons`, `components`, `prefabs` |
| `item` | `id:string` | Sample item details and DataCore status |
| `balance` | none | Local balance via `game.economy.read` |
| `buy` | `id:string` | Debit the fictional price via economy read + apply |
| `equip` | `id:string` | Apply one wearable class via `game.loadout.apply` |
| `spawn` | `id:string zone:string x:float y:float z:float` | Initiate a prefab spawn via `game.entities.prefabs` |

Zone ids are decimal or `0x` hex, non-zero, through the full uint64 range.
Positions are finite zone-local metres. Panel Spawn uses `(0,0,0)` and identity
rotation; the command accepts explicit position. Success is spawn initiation,
not a promise of streaming completion or collision. Paths are fixed catalogue
entries, not arbitrary user paths.

Example headless invocation:

```sh
build-app4/_sdk/build/sco-host-sim build-app4/diegetic_shop_inv/root --no-lua --ticks 1 --invoke diegetic_shop_inv.list 2
```

## Service boundaries and limitations

Each action re-queries its table, checks the query-service API extent and
non-nullness, the capability, and the complete member extent plus non-null
function pointer. Native service result codes are preserved.

Buy is **only a local-economy debit**, not an entitlement, item grant, inventory
purchase, persistence operation, or combined buy-and-equip transaction. It reads
balance, refuses insufficient funds, and sends a negative price with the
expected balance and a non-zero once-only payment key. Each explicit Buy is
a new transaction; there is no automatic retry. Keys start from the process
steady clock at plugin load and increment. The service scopes them to the
owner/session. Equip and Spawn are independent sandbox actions and do not
require buying first. A failed equip does not imply rollback.

The public `sco.datacore` 1.x table shipped in this SDK **does not have item
lookup or enumeration**: it queues override patches. The plugin probes its
presence and `state()` (open/loaded), but never writes patches or pretends that
its fictional record-shaped names came from the game. A real record reader and
3D preview belong to future host/studio interfaces; there is no guessed ABI or
mesh renderer here.

Tests cover synthetic catalogue validation, command paging limits, full uint64
parsing, absent/short/null tables, each game capability, payment results and
keys, loadout reports, prefab path/pose forwarding, and widget selection/paging/
button events. CTest also runs the real kit loader in `sco-host-sim` and the
checker's deterministic headless widget pass.

Only macOS clang is verified locally. Linux GCC and Windows MSVC `/W4 /WX`
have not been run. Nothing here has run in Star Citizen. The fictional classes
and paths are not expected to exist there; an actual provider may reject them.
