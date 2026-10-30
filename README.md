# scuba-engine

A cross-platform (Windows, macOS, Linux) studio for building and testing [sco-core](https://github.com/scuba-sc/sco-core) plugins and UI without running the game: a native sandbox host (`scuba-hostd`) built from the published sco-core SDK, and a browser studio for widget design, 3D in-world panel projection, DataCore and asset inspection, and multi-peer networking tests.

Status: integrated out-of-game sandbox. Licence: GPL-3.0-only ([LICENSE](LICENSE)). This project ships no game files, and **nothing here has been verified inside Star Citizen**.

## Feature tour

- **Widgets** renders loaded plugins through the SDK's retained widgets service. The studio bridge advertises widgets 1.1 features, including paging, tooltips and severity; input travels back through hostd.
- **Designer** edits nested widget trees, previews through the same widget renderer, round-trips generated C/C++/C#/Lua source, and exports native plugin folders.
- **Viewport** projects plugin panels onto six synthetic terminal environments as billboards or perspective quads. Select a plugin surface and optionally follow the mock camera; projection and ray-plane input use the same camera.
- **Asset Lab** browses DataCore records and TOML overrides, converts a documented clean-room subset of Cry/DDS/OBJ/glTF, and previews GLB. See [format limits](docs/assets.md).
- **NetSandbox** runs two to eight separate kernel processes with real loopback UDP, private IPC namespaces, widget panels and traffic counters.
- **APP examples** load by default: `mp_chat`, `org_social`, `diegetic_asop`, and `diegetic_shop_inv`. Their panels appear in Widgets and the Viewport surface picker. The mock game supplies the shop's fictional item/prefab catalog and hangar services.

There is **no live game link**: ENG-8 could not implement spatial streaming through the SDK's metadata-only diagnostics contract. The `game.*` values described here come from synthetic mock services, not real game-native adapters. Mock camera events are synthetic, not evidence of a live connection.

## Native engine (phase 1, Windows)

`scuba-engine.exe` is the native editor that will replace the browser studio: C++20, D3D12, a
docking ImGui shell, the sco-core kernel in process (plugins and their `sco.widgets` panels), an
asynchronous OBJ/glTF/CGF model loader on a job pool, and a virtual file system that mounts
directories, zip archives and `.scpak` packs with sco-core's overrides on top. Design:
[docs/engine/architecture.md](docs/engine/architecture.md); VFS: [docs/engine/vfs.md](docs/engine/vfs.md).

```sh
cmake -S . -B build -A x64 && cmake --build build --config Release
build/engine/Release/scuba-engine.exe --open models/cube.obj --mount D:/assets.zip=packs@20
```

Right drag orbits, middle drag pans, the wheel zooms, left click selects, Ctrl + left drag moves
the selection, F frames it. `--warp --hidden --frames N` renders N frames on the software adapter
and exits (the `engine_smoke` test). Phase 1 limits: Windows only, constant PBR materials (no
textures yet), CPU picking.

## Requirements

- CMake 3.30+, a C++20 compiler (clang, GCC or MSVC), 64-bit
- Node.js 22.12+ (24 is what CI uses) for the studio
- Python 3 for native plugin export; Bun and Lua 5.4 for the generated-plugin CTest
- A locally generated, checksum-verified development host-source archive; network access is also needed for third-party dependencies on first configure

## Build scuba-hostd

```sh
cmake -S . -B build -DSCUBA_SDK_ARCHIVE_DIR=<development-assets>
cmake --build build
ctest --test-dir build --output-on-failure
```

Build and CTest before running studio tests: Asset Lab's loader tests consume synthetic files generated at `build/tests/assets-fixtures`. If using another build directory, generate its fixtures and copy them to that location before `npm test`.

Generate `sco-sdk-host-source-sc-2.4.0.zip` using `sdk/package.py --profile host-source-sc`
from a clean sco-core checkout at exact development commit
`e2ad885aa3027ed1162793d30292ca103c2f5b28`. The archive's SHA-256 is
`e9096f7b3c13c89dc4e9b6aec4ed595aaa71778b207823caa4918978363e1cd6`.
Configure verifies it before extraction. It is a development input, not an SDK release;
CI generates the same profile from that commit. A build tree configured before the pin
moved keeps cached `SCUBA_SDK_*` values: use a fresh tree or add `-U "SCUBA_SDK_*"` once.
For the earlier published RC2 build and its asset recipe, use scuba-engine commit
`8373c52012d2f58fc773f796e199de81c16eb571`. An appropriate supported stable SDK can
become the normal baseline once it includes the required contract and passes the
cross-consumer checks; explicit source-based development remains available.

On Windows add `-A x64` to the configure and `--config Release` / `-C Release` to build and ctest.

## Build the studio

The web studio is in maintenance mode; see [studio/MAINTENANCE.md](studio/MAINTENANCE.md) for what is accepted.

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

The `game.*` services are deterministic mock models, not the game's native adapters: they provide no real physics, entitlement, authenticated game networking, or proprietary catalogs. A shop purchase only debits synthetic balance; it grants no inventory. `game.asop` and `game.contracts` return fixed synthetic values; the game pack holds their capabilities off until an in-game run confirms them. Mock tests do not establish real native service support or gameplay. C# and Lua generation support only the published builder subset; native folder export supports C/C++ only. C# requires a separate dotnet environment; CI is its first compile opportunity. Linux GCC and Windows MSVC platform paths require their own builds; a macOS pass is not cross-platform certification.
