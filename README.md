# scuba-engine

A cross-platform (Windows, macOS, Linux) studio for building and testing [sco-core](https://github.com/scubamount/sco-core) plugins and UI without running the game: a native sandbox host (`scuba-hostd`) built from the published sco-core SDK, and a browser studio for widget design, 3D in-world panel projection, DataCore and asset inspection, and multi-peer networking tests.

Status: under construction (foundation: hostd skeleton, JSON bridge, studio shell). Licence: GPL-3.0-only ([LICENSE](LICENSE)). This project ships no game files, and nothing here has been verified inside Star Citizen.

## Requirements

- CMake 3.21+, a C++20 compiler (clang, GCC or MSVC), 64-bit
- Node.js 22.12+ (24 is what CI uses) for the studio
- Network access on first configure (the sco-core SDK kit is downloaded), or a local copy of the release assets

## Build scuba-hostd

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Configure downloads `sco-sdk-host-source-sc-2.2.0.zip` from the sco-core `sdk-v2.2.0` release, checks its SHA-256 (`0e9f45bc…f96f36`, the value in the release's `SHA256SUMS`) and builds it under `build/_sdk/`. A mismatched archive stops the configure. To build offline from a local copy of the release assets:

```sh
cmake -S . -B build -DSCUBA_SDK_ARCHIVE_DIR=/path/to/sdk-v2.2.0-assets
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
./build/scuba-hostd                       # http://127.0.0.1:7878/ (studio) and ws://127.0.0.1:7878/ws
./build/scuba-hostd --port 9000 --plugins /path/to/plugins
```

Flags: `--port N` (0 picks a free port), `--bind ADDR` (default `127.0.0.1`), `--studio DIR` (default: this checkout's `studio/dist`), `--plugins DIR` (sco-core plugin root; omitted: no plugin folders), `--data DIR` (sco.storage), `--allow-origin ORIGIN`, `--no-lua`. Stop with Ctrl+C.

For studio development with hot reload, run hostd with `--allow-origin http://localhost:5173` and `npm run dev` in `studio/`; the dev server proxies `/ws` to `127.0.0.1:7878`.

The bridge is unauthenticated and loopback-only by default; it refuses foreign `Host` and `Origin` headers. See [docs/protocol.md](docs/protocol.md) for the message format and [docs/architecture.md](docs/architecture.md) for the layout.
