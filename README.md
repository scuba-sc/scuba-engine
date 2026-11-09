# scuba-engine

The browser studio is an OS-independent tool for building and testing [sco-core](https://github.com/scuba-sc/sco-core) plugins and UI without the game. It provides widget design, 3D in-world panel projection, DataCore and asset inspection, and multi-peer networking tests. `scuba-hostd` is the native sandbox host; its supported runtime targets are Windows x64 and Linux x64. The native `scuba-engine.exe` editor currently targets Windows x64.

Status: integrated out-of-game sandbox. Licence: GPL-3.0-only ([LICENSE](LICENSE)). This project ships no game files, and **nothing here has been verified inside Star Citizen**.

## Feature tour

- **Widgets** renders loaded plugins through the SDK's retained widgets service. The studio bridge exposes widgets 1.2 features and advertises 1.3 numeric presentation when both the SDK declaration and host snapshot support it; input travels back through hostd.
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

- CMake 3.21+, a C++20 compiler (clang, GCC or MSVC), 64-bit
- Supported native host runtime builds: Linux x64 with GCC/Clang and Windows x64 with MSVC
- Node.js 22.12+ (24 is what CI uses) for the studio
- Python 3 for native plugin export; Bun and Lua 5.4 for the generated-plugin CTest
- For current development: the verified local coordinated Core host-source archive and its source/hash coordinates; third-party dependencies also need network access or local archives

## Build scuba-hostd

For current development, use the [coordinated Core development profile](docs/engine/coordinated-core-development.md). It requires the unpublished `2.4.1-dev.1` host-source kit built from the selected clean Core commit, its independently recorded SHA-256, and checked lifecycle handling. It does not download a release as a fallback:

```sh
export SCUBA_DEV_SDK_ARCHIVE_DIR=/path/to/verified-development-assets
export SCUBA_DEV_SDK_HOST_SHA256=<verified-host-archive-sha256>
cmake --preset coordinated-core-development
cmake --build --preset coordinated-core-development
ctest --preset coordinated-core-development
```

Rebuild all C++ consumers together in a fresh build tree when changing Core: `Platform` and `HostReport` are C++ layouts, even though the plugin C ABI remains frozen. Build and CTest before running studio tests: Asset Lab's loader tests consume synthetic files generated at `build/tests/assets-fixtures`. If using another build directory, generate its fixtures and copy them to that location before `npm test`.

For the historical published release profile, plain `cmake -S . -B build` retains the published sco-core [`sdk-v2.4.0`](https://github.com/scuba-sc/sco-core/releases/tag/sdk-v2.4.0) host-source kit, SHA-256 `a0aaaba07e3d4eb0d8c264912cd6f2ab23f0d237a99fa07816005790a76efc4c` and source commit `6106fa3639cbcc28f78a1ded8de2707de019f4d3`. This is not the current development input. Its existing release workflows and the earlier [bf56 rehearsal](docs/engine/core-main-sdk-rehearsal.md) retain their original coordinates and evidence. For a plugin-only release build, see [the published plugin SDK instructions](docs/engine/plugin-integration.md#build-the-plugins-with-the-published-plugin-sdk-only).

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

The `game.*` services are deterministic mock models, not the game's native adapters: they provide no real physics, entitlement, authenticated game networking, or proprietary catalogs. A shop purchase only debits synthetic balance; it grants no inventory. `game.asop` and `game.contracts` return fixed synthetic values; the game pack holds their capabilities off until an in-game run confirms them. Mock tests do not establish real native service support or gameplay. C# and Lua generation support only the published builder subset; native folder export supports C/C++ only. C# requires a separate dotnet environment; CI is its first compile opportunity. Linux/GCC and Windows/MSVC are the supported native runtime build paths and are validated separately in CI. The pinned sco-core 2.4.0 host kit does not compile with Apple's libc++ because `std::atomic<std::shared_ptr<T>>` is not implemented there; macOS is not a supported native runtime target. Browser Studio use and standalone portable developer tools remain separate from that runtime support statement.
