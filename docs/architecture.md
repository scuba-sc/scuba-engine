# scuba-engine architecture

scuba-engine is a studio for building and testing sco-core plugins and UI without the game. It
consumes a verified development `sco-sdk-host-source-sc-2.4.0.zip` generated from exact
sco-core source `e2ad885aa3027ed1162793d30292ca103c2f5b28`; it is not a published release.
The project never vendors its sources. Nothing in this repository
has run inside Star Citizen; it ships no game files or game data.

## Integrated subsystems

The host retains its mock backing and refuses restart if core teardown fails; a
failed start is also treated as retained because core may have exposed backing state.

1. **Hostd** boots `sco_app` headless and publishes 24 synthetic service tables before external
   plugin discovery. It registers system, widgets, mock-game, designer-export, asset and plugin
   command handlers in `RegisterAllModules`. `main` registers NetSandbox with the executable path.
2. **Widgets** serializes retained SDK panels through `StudioWidgetRenderer`. `widgets.event`
   feeds `sco::widgets::RenderPanel`, never bypassing SDK event validation or plugin callbacks.
3. **Viewport** lazy-loads Three.js and projects the selected live plugin panel onto six synthetic
   environments. Billboards and CSS-homography quads share ray-plane input and simulated occlusion.
   `connection.ts` consumes widget snapshots and rereads `mock_game.state` on `mock_game.changed`.
   Opt-in camera following rebases world coordinates into the player's current zone, then maps
   SDK z-up into Three.js y-up as `(x,z,-y)`. Quaternions are conjugated by that basis change.
4. **Designer** maps the editing tree into `WidgetPanel` via `designSnapshot`, then uses the same
   `renderPanelView` and `toDom` as live panels. Preview events mutate the editor model; generated
   source therefore follows input. Native export invokes the SDK scaffold without a shell.
5. **Asset Lab** links `sco_datacore` for record/override work and uses original bounded static
   Cry/DDS/OBJ/glTF readers. Format support and limits are in [assets.md](assets.md).
6. **NetSandbox** brokers separate ticking kernel processes, private JSON-line pipes and IPC
   namespaces. Its sample plugin uses real authenticated loopback UDP; see [net-sim.md](net-sim.md).

No live-link module was delivered by ENG-8. SDK 2.4.0 diagnostics (unchanged since 2.3.0) admits metadata, not spatial
observations. Camera/entity streaming requires a separately reviewed upstream endpoint; this
integration neither invents that endpoint nor transports positions through logs or metadata.

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
  kernel.*                  sco::app boot (headless, no image, synthetic game services), plugin list
  modules.h / modules.cpp   HostContext and RegisterAllModules (one line per module)
  system_handlers.cpp       system.ping / status / methods / shutdown
  plugins_handlers.cpp      plugins.invoke, typed against SDK command definitions
  widgets_bridge.*          retained-panel renderer, snapshots and event validation
  widgets_handlers.cpp      widgets.list / get / event; tick hook
  mock_game/                scene, synthetic game tables and mock_game.* handlers
  net_sim/                  separate-process kernels, broker and sample plugin
  main.cpp                  flags, main loop (Poll 16 ms, then sco::app::Tick)
src/assets/                 DataCore and clean-room format readers
plugins/                    mp_chat, org_social, diegetic_asop, diegetic_shop_inv
tests/                      ctest: websocket, http+json, handlers, end-to-end (real hostd)
studio/                     Vite + TypeScript + Three.js
  src/protocol.ts           wire types (mirror of docs/protocol.md)
  src/bridge.ts             HostdBridge: typed calls, events, queueing, reconnect with backoff
  src/panels/registry.ts    PanelModule + registerPanel
  src/panels/index.ts       one registerPanel line per panel
  src/panels/home.ts        hostd status placeholder
  src/panels/viewport/      projected plugin surfaces and mock camera adapter (lazy-loaded)
  src/panels/widgets/       shared VNode and DOM renderer, live Widgets panel
  src/panels/designer/      editing model, shared-renderer preview and code generation
  src/panels/assets/        DataCore inspector and GLB preview
  src/panels/netsim/        network peer controls, traffic and peer widget panels
  src/shell.ts              tab bar + panel host
docs/protocol.md            the bridge envelope, codes, methods, events, access control
```

`scuba_bridge` (static library) holds everything except the kernel, so the protocol pieces are
unit-tested without sco-core. `scuba-hostd` links `scuba_bridge`, `sco_app` (which brings
`sco_host`, `sco_runtime`, `sco_plugins`, `sco_widgets`, `sco_ui`, `sco_storage`, `sco_settings`,
`sco_content`, `sco_ipc`, `sco_net_service`/`sco_net`, `sco_datacore_service`/`sco_datacore`/
`sco_vfs`) and `sco_lua_game` (`sco_lua`).
The integrated static libraries `scuba_mock_game`, `scuba_assets` and `scuba_net_sim` link those
dependencies transitively. Mock scene reset also reads the APP-4 fictional catalog directly
from `shop::Catalogue()`, compiled into the mock library, so equip/prefab admission stays aligned.

## Plugin loading

Root CMake includes all four APP folders. `scuba_plugins` stages their actual target files plus
manifests into `<build>/plugins/runtime/<id>/`. Hostd depends on that target and uses the staged
root by default; `--plugins` overrides it. Each lane's separate root/stage remains available for
its checker tests. Storage is opt-in with `--data`; the README uses `build/data`.
Loaded panels appear in Widgets and in the Viewport picker, without separate per-plugin panels.
`plugins.invoke {name,args?}` invokes registered commands using their declared SDK argument types.

## Threading

`main` is the sco-core game thread: it calls `sco::app::Start`, then loops `Server::Poll(16)` and
`sco::app::Tick(ms since start)`. Every bridge handler therefore runs on the game thread, so
handlers may call sco-core APIs that require it. `Broadcaster::broadcast` is the only
cross-thread entry point (the kernel log sink may fire from any thread); queued events are sent at
the start of the next `Poll`.
After each kernel tick, `HostContext::tickHooks` pumps widgets. Export, asset and broker calls
are synchronous and can stall that loop. NetSandbox child kernels tick independently.

## Verification boundaries

CTest covers native units, synthetic format fixtures, SDK plugin checkers, generated native
plugins, mock services, two/four/eight-process network scenarios and real-host WebSocket calls
across every delivered module and all four APP plugins. Studio loader tests consume the native
fixtures under `build/tests/assets-fixtures`, so run CTest before `npm test`.
C# output cannot be compiled without dotnet; CI is its first compile opportunity. macOS clang
verification does not substitute for Linux GCC or Windows MSVC builds. No proprietary fixtures,
game execution, live diagnostics attachment, inventory grants or game-format parity are claimed.

## Adding a module

C++: create `src/hostd/<module>_handlers.cpp` defining
`void Register<Module>Handlers(Registry&, HostContext&)`, declare it in `modules.h`, add one call
in `RegisterAllModules` (`modules.cpp`), and add the file to `scuba-hostd` in `CMakeLists.txt`.
Studio: create `studio/src/panels/<feature>.ts` exporting a `PanelModule` and add one
`registerPanel(...)` line to `studio/src/panels/index.ts`.
