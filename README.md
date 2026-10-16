# scuba-engine

A cross-platform (Windows, macOS, Linux) studio for building and testing [sco-core](https://github.com/scubamount/sco-core) plugins and UI without running the game: a native sandbox host (`scuba-hostd`) built from the published sco-core SDK, and a browser studio for widget design, 3D in-world panel projection, DataCore and asset inspection, and multi-peer networking tests.

Status: integrated out-of-game sandbox. Licence: GPL-3.0-only ([LICENSE](LICENSE)). This project ships no game files, and **nothing here has been verified inside Star Citizen**.

## Feature tour

- **Widgets** renders loaded plugins through the SDK's retained widgets 1.1 service. Input travels back through hostd, including paging, tooltips and severity.
- **Designer** edits nested widget trees, previews through the same widget renderer, round-trips generated C/C++/C#/Lua source, and exports native plugin folders.
- **Viewport** projects plugin panels onto six synthetic terminal environments as billboards or perspective quads. Select a plugin surface and optionally follow the mock camera; projection and ray-plane input use the same camera.
- **Asset Lab** browses DataCore records and TOML overrides, converts a documented clean-room subset of Cry/DDS/OBJ/glTF, and previews GLB. See [format limits](docs/assets.md).
- **NetSandbox** runs two to eight separate kernel processes with real loopback UDP, private IPC namespaces, widget panels and traffic counters.
- **APP examples** load by default: `mp_chat`, `org_social`, `diegetic_asop`, and `diegetic_shop_inv`. Their panels appear in Widgets and the Viewport surface picker. The mock game supplies the shop's fictional item/prefab catalog and hangar services.

There is **no live game link**: ENG-8 could not implement spatial streaming through the SDK's metadata-only diagnostics contract. Mock camera events are synthetic, not evidence of a live connection.

## Requirements

- CMake 3.21+, a C++20 compiler (clang, GCC or MSVC), 64-bit
- Node.js 22.12+ (24 is what CI uses) for the studio
- Python 3 for native plugin export; Bun and Lua 5.4 for the generated-plugin CTest
- Network access on first configure (the sco-core SDK kit is downloaded), or a local copy of the release assets

## Build scuba-hostd

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Build and CTest before running studio tests: Asset Lab's loader tests consume synthetic files generated at `build/tests/assets-fixtures`. If using another build directory, generate its fixtures and copy them to that location before `npm test`.

Configure downloads `sco-sdk-host-source-sc-2.3.0.zip` from the sco-core `sdk-v2.3.0` release, checks its SHA-256 (`ec6c43d3…a1e79a`, the value in the release's `SHA256SUMS`) and builds it under `build/_sdk/`. A mismatched archive stops the configure. To build offline from a local copy of the release assets:

```sh
cmake -S . -B build -DSCUBA_SDK_ARCHIVE_DIR=/path/to/sdk-v2.3.0-assets
```

On Windows add `-A x64` to the configure and `--config Release` / `-C Release` to build and ctest.

## Build the studio

```sh
cd studio
npm ci
npm test
npm run build      # writes studio/dist, which scuba-hostd serves
```

## Run

```sh
./build/scuba-hostd --data build/data      # studio + four built APP plugins; storage stays in build/
./build/scuba-hostd --port 9000 --plugins /path/to/plugins
```

Flags: `--port N` (0 picks a free port), `--bind ADDR` (default `127.0.0.1`), `--studio DIR` (default: this checkout's `studio/dist`), `--plugins DIR` (override the default `build/plugins/runtime` plugin root), `--data DIR` (sco.storage), `--allow-origin ORIGIN`, `--no-lua`, `--no-mock-game`. Stop with Ctrl+C. Pass an empty plugin directory to run without APP plugins.

The build stages each plugin as `<build>/plugins/runtime/<id>/{plugin.ini,<id>.dll}` on every platform. Hostd depends on that staging target, including when built directly. Run installed/moved binaries with explicit `--plugins` and `--studio` paths.

For studio development with hot reload, run hostd with `--allow-origin http://localhost:5173` and `npm run dev` in `studio/`; the dev server proxies `/ws` to `127.0.0.1:7878`.

The bridge is unauthenticated and loopback-only by default; it refuses foreign `Host` and `Origin` headers. See [docs/protocol.md](docs/protocol.md) for the message format and [docs/architecture.md](docs/architecture.md) for the layout.

## Limits

`game.actor_stats` and `game.actor_stats_write` are mocked only once the pinned kit ships their headers (not in sdk-v2.3.0; see [docs/mock-game.md](docs/mock-game.md)). Not run in game.

Mock services are a deterministic test model, not the game: no real physics, entitlement, authenticated game networking, or proprietary catalogs. A shop purchase only debits synthetic balance; it grants no inventory. `game.asop` and `game.contracts` return fixed synthetic values; the game pack holds their capabilities off until an in-game run confirms them. C# and Lua generation support only the published builder subset; native folder export supports C/C++ only. C# requires a separate dotnet environment; CI is its first compile opportunity. Linux GCC and Windows MSVC platform paths require their own builds; a macOS pass is not cross-platform certification.
