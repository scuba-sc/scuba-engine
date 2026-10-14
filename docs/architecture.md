# scuba-engine architecture

scuba-engine is a studio for building and testing sco-core plugins and UI without the game. It
consumes the published sco-core SDK release (today `sdk-v2.2.0`, the
`sco-sdk-host-source-sc-2.2.0.zip` kit) and never vendors its sources. Nothing in this repository
has run inside Star Citizen; it ships no game files or game data.

## The four subsystems

The engineering plan (v8.0) splits the product into four subsystems:

1. **scuba-hostd**: a native C++20 sandbox host and live bridge daemon. It links the sco-core host
   kit (`sco_app` and the kernel libraries), loads C, C++, C# NativeAOT and Lua plugins headless,
   and exposes a JSON bridge to the studio. Later lanes add the `StudioWidgetRenderer`
   (`sco::widgets::Renderer`), a mock `game.*` services provider, a multi-peer `sco.net`/`sco.ipc`
   sandbox and a live link to a running sc-offline session.
2. **scuba-viewport**: the browser 2D/3D plane (Three.js today, WebGPU later): scene and model
   viewer, asset inspection, anchor placement.
3. **scuba-diegetic**: in-world panel projection: camera-facing billboards and perspective-warped
   3D surface quads (CSS `matrix3d` homography) with ray-plane hit testing.
4. **scuba-designer**: a visual `sco.widgets` 1.1 / `sco.ui` editor with round-trip code generation
   (C++ `scosdk::PanelBuilder`, C#, Lua, C11 node arrays).

The foundation (this lane) delivers subsystem 1's skeleton and the studio shell that 2-4 plug into.

## Module layout

```
CMakeLists.txt              top level; C++20; options SCUBA_BUILD_TESTS, SCUBA_WERROR
cmake/ScubaSdk.cmake        download (or SCUBA_SDK_ARCHIVE_DIR copy), SHA-256 check, extract,
                            add_subdirectory of the kit into build/_sdk/
src/hostd/
  sha1.*, base64.*          primitives for the WebSocket handshake (own code, no vendored deps)
  json.*                    small JSON value, parser, serializer
  http.*                    HTTP/1.1 request-head parser, responses, static path resolution
  ws.*                      RFC 6455 handshake, frame encoder, incremental reader
  handlers.*                Registry (register_handler / dispatch) and Broadcaster
  server.*                  single-threaded poll() server: static files + /ws bridge
  kernel.*                  sco::app boot (headless, no image, no game services), plugin list
  modules.h / modules.cpp   HostContext and RegisterAllModules (one line per module)
  system_handlers.cpp       system.ping / status / methods / shutdown
  main.cpp                  flags, main loop (Poll 16 ms, then sco::app::Tick)
tests/                      ctest: websocket, http+json, handlers, end-to-end (real hostd)
studio/                     Vite + TypeScript + Three.js
  src/protocol.ts           wire types (mirror of docs/protocol.md)
  src/bridge.ts             HostdBridge: typed calls, events, queueing, reconnect with backoff
  src/panels/registry.ts    PanelModule + registerPanel
  src/panels/index.ts       one registerPanel line per panel
  src/panels/home.ts        hostd status placeholder
  src/panels/viewport.ts    Three.js placeholder stage (lazy-loaded)
  src/shell.ts              tab bar + panel host
docs/protocol.md            the bridge envelope, codes, methods, events, access control
```

`scuba_bridge` (static library) holds everything except the kernel, so the protocol pieces are
unit-tested without sco-core. `scuba-hostd` links `scuba_bridge`, `sco_app` (which brings
`sco_host`, `sco_runtime`, `sco_plugins`, `sco_widgets`, `sco_ui`, `sco_storage`, `sco_settings`,
`sco_content`, `sco_ipc`, `sco_net_service`/`sco_net`, `sco_datacore_service`/`sco_datacore`/
`sco_vfs`) and `sco_lua_game` (`sco_lua`).

## Threading

`main` is the sco-core game thread: it calls `sco::app::Start`, then loops `Server::Poll(16)` and
`sco::app::Tick(ms since start)`. Every bridge handler therefore runs on the game thread, so
handlers may call sco-core APIs that require it. `Broadcaster::broadcast` is the only
cross-thread entry point (the kernel log sink may fire from any thread); queued events are sent at
the start of the next `Poll`.

## Adding a module

C++: create `src/hostd/<module>_handlers.cpp` defining
`void Register<Module>Handlers(Registry&, HostContext&)`, declare it in `modules.h`, add one call
in `RegisterAllModules` (`modules.cpp`), and add the file to `scuba-hostd` in `CMakeLists.txt`.
Studio: create `studio/src/panels/<feature>.ts` exporting a `PanelModule` and add one
`registerPanel(...)` line to `studio/src/panels/index.ts`.
