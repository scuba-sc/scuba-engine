# Agent control plane: drive scuba-engine without a mouse (E-15)

Status: proposed, integration-ready. Targets the native editor (`engine/`) and the Studio MCP
bridge (`tools/scuba-engine-mcp/`, see [studio-mcp.md](studio-mcp.md)). Author: OmO fleet sprint,
2026-10-18. This is a design contract for the implementing agent; each phase names its files, its
acceptance test and its rollback.

## 1. The gap, with evidence

The bridge already exposes plugin **panels** (`widgets.get` / `widgets.event`) and the plugin
command registry through `plugins.invoke`: hostd resolves the name with `sco::ListCommands`, checks
the argument count and each argument type, runs it with `sco::Invoke` and returns the command's reply
text (`src/hostd/plugins_handlers.cpp:22-66`). It also exposes synthetic game primitives
(`mock_game.*`: 20 wire methods registered by `RegisterMockGameHandlers` in
`src/hostd/mock_game/handlers.cpp`), plus `system.status`, `assets.convert` and the three
operator-configured `project_*` tools in `tools/scuba-engine-mcp/developer.mjs`.

Two gaps sit in front of the studio one. The command surface cannot be **discovered** over the wire
- an agent must know names and argument shapes out of band - which is phase P0, and needs no SDK
change. And `send_widget_event` addresses nodes by numeric `node_id`, which a panel redesign moves;
that is ask S-1 in section 8.

What it does **not** expose is the studio itself. The native editor has 26 addressable controls
(`engine/app/main.cpp`, `DrawContent`/`DrawInspector`, lines ~426-671): profile mount,
`Load Product Profile (sc-offline)`, owner storage inventory, `Read committed ASOP selection`,
Admit/Revoke mock capabilities, capability readback, `RespawnLoadoutCompleted`,
`Mission changed (synthetic 1)`, entity inject, `Export product bundle + SHA256SUMS`,
`Frame (F)`, `Remove (Del)`, `Frame selection`, material sliders, transform, mounts.

The only channel to them is OS pointer input. That channel is the weakest evidence in the program:

- Pointer delivery against a window target landed on the wrong control (verified 2026-10-18:
  a click intended for a collapsing header injected `entity 65536 / count 12` into the entity
  field). The build/render had passed; the interaction proof was not valid.
- ImGui's fast-sampled input drops discrete clicks; a bounded ~150 ms press was needed to land.
- Multi-DPI geometry made window-relative coordinates unstable; the client rect had to be restored
  with a guarded Win32 script mid-run.
- A full-desktop capture is the reliable frame, and it carries unrelated private app content that
  must never enter a repo. So the act of producing evidence creates a privacy problem.

Consequence: `Verify complete product native panels and service readback` and
`Capture desktop smaller window and spatial native evidence` cannot be closed durably. Design
direction: **make every studio control an addressable, versioned action, and make the UI a
renderer over that same table.** Then an agent drives the product, not the desktop.

## 2. Design: one action table, three transports

```
                 +---------------------------+
   ImGui  ------> |                           |
   ws/MCP ------> |  engine::ActionRegistry   |----> sco:: / ScoHost / Editor effects
   --drive  ----> |  (single source of truth) |<---- ActionResult + state digest
                 +---------------------------+
```

### 2.1 Registry (new: `engine/src/app/action_registry.{h,cpp}`)

The descriptor is deliberately the **same shape as `sco::Command`**
(`sco-core: include/sco/runtime.h:201-236` - name, title, help, capability, arg defs, bounds
`kMaxCommands = 512`, `kMaxCommandArgs = 16`, `kMaxNameLen = 63`, `kMaxTitleLen = 63`,
`kMaxHelpLen = 255`, `kMaxCapabilityLen = 63`). One vocabulary for plugin commands (already
callable through `plugins.invoke`, which enumerates them with `sco::ListCommands` at
`src/hostd/plugins_handlers.cpp:27-39`) and for studio controls, so an agent sees one address space
and one set of limits.

```cpp
namespace scuba::engine {

using ActionId = std::string;          // stable, dotted, append-only; <= 63 chars (kMaxNameLen)

struct ActionArg {
    const char* name;                  // "path", "entity_class", "mission", ...
    // Mirrors sco::ArgType (runtime.h:190) for the first four; Vec3/Quat are engine-only
    // composites, encoded on the wire as 3 and 4 floats. No pointer argument exists in any
    // form, matching the command registry's Arg union (Int/Float/String/Bool only).
    enum class Type { Int, Float, String, Bool, Vec3, Quat } type;
    bool required = false;
    const char* help = nullptr;        // <= 127 chars, kMaxArgHelpLen
    long long min = 0, max = 0;        // bounds for numeric types; string length cap
};

struct ActionRequest {
    ActionId id;
    std::vector<std::pair<std::string, std::string>> args;  // transport-neutral, parsed by handler
    std::string request_id;            // echoed verbatim into the result for correlation
};

struct ActionResult {
    bool ok = false;
    std::string code;                  // "ok" | SCO_* name | "not_found" | "bad_arg" | "unavailable"
    std::string message;               // one human-readable line, wrapped-safe
    uint64_t generation = 0;           // diagnostics snapshot this result was observed in (2.4)
    std::string state_digest;          // canonical JSON snapshot after the action (section 2.4)
    std::vector<std::pair<std::string, std::string>> outputs;  // e.g. selection text, export path
};

class ActionRegistry {
public:
    struct Entry {
        ActionId id;
        const char* title;             // the SAME text the button shows
        const char* group;             // "product" | "sco.mock" | "scene" | "inspector" | "vfs"
        std::vector<ActionArg> args;
        bool requires_window = false;  // false = safe headless
        std::function<ActionResult(const ActionRequest&)> handler;
    };
    void Add(Entry);                                    // called once at startup
    const std::vector<Entry>& All() const;
    const Entry* Find(const ActionId&) const;
    ActionResult Invoke(const ActionRequest&);          // the only mutating path
};

}  // namespace scuba::engine
```

Rules for the implementing agent:

1. **Registration and rendering share one list.** Each `ImGui::Button(title)` becomes
   `foreach (entry in group) if (ImGui::Button(entry.title)) Invoke({entry.id, entry.args})`.
   The table is built once at startup with captured state (`Editor&`, `ScoHost&`, `JobSystem&`)
   inside the handler closures, so no globals leak into the transport layer. A test asserts every
   registry id has a title and every rendered studio button resolves to a registry id - the UI can
   no longer drift from the API.
- **`Invoke` runs on the game thread only.** Handlers are queued and drained inside the existing
  frame loop (`int Editor::Run` at main.cpp:687 -> `while (!quit && window.Pump())` at 734, next to
  `ed.sco.Tick()` at 736), then the result is published. Never call into sco-core from the transport
  thread: this is already the documented invariant of the method registry ("Handlers run on the
  host's main thread (the sco-core game thread), one at a time", `src/hostd/handlers.h:14-15`) and of
  the widget bridge, and it matches the owner-thread rule pinned for `sc_relocation_v1`.
3. **Refusals are data, not exceptions.** Unknown id -> `not_found`; missing/out-of-bound arg ->
   `bad_arg` with the offending name; capability off / host not started -> `unavailable`; a plugin
   panel that is policy-disabled -> the panel's own refusal. Return the state digest in every case,
   exactly as the mock providers do today (`SCO_UNAVAILABLE` with output unchanged - the pattern
   pinned for inventory, gamerules and missions reads). Capability gating uses the registry's own
   answer, `sco::caps::Has(name)` / `sco::caps::List(...)` with its reason string
   (`sco-core: include/sco/caps.h:34-44`), so "why is this off" is data, not a guess.
4. **Bounded inputs.** Strings capped (name 63, title 63, help 255, capability 63 - the same
   `kMax*` bounds the command registry enforces, `sco-core: include/sco/runtime.h:215-219`); at most
   16 args per action (`kMaxCommandArgs`); numeric args validated against `ActionArg::min/max`; no raw
   pointers, addresses or offsets in any action argument, ever. The bridge keeps refusing non-loopback.
5. **Ids are append-only.** Never renumber, never repurpose, never delete in a minor version.
   Deleting a control means deprecating its id (it answers `unavailable` + `deprecated: true`).

### 2.1bis Reuse the command registry as the bus (the single biggest simplification)

The runtime already implements, in production, exactly the plumbing this design needs. Use it
instead of a bespoke queue (`sco-core: include/sco/runtime.h`):

| Need | Existing primitive |
|---|---|
| descriptor: name, label, help, capability, typed args | `struct Command` - `name`, `title` ("menu label"), `help`, `capability`, `const ArgDef* args`, `nargs`, `fn`, `ctx` (204-213); `ArgDef{name, type, help}` (198) |
| argument types and bounds | `ArgType{Int, Float, String, Bool}` (190), `Arg` union (192-196), `kMaxCommandArgs = 16`, `kMaxCommands = 512`, name 63 / title 63 / help 255 / capability 63 / arg name 31 / arg help 127 (215-220) |
| host-side registration, no plugin prefix squatting | `RegisterCommand(owner, prefix, cmd)` with `prefix = nullptr` for host features, "any `<x>.<y>` name" (225-236); reserved prefixes `sco`, `host`, `menu`, `game` are closed to plugins (223) |
| dispatch from any thread, executed on the game thread | `Invoke(name, args, nargs, done, ctx, owner)`: on the game thread it runs synchronously; off-thread the name and args are copied, queued as a task of `owner`, and `done` runs on the game thread; `BadArg`/`TooMany`/`Unavailable`/`NotFound` are returned as data (249-264) |
| human-readable outcome | the command's reply buffer - "a short message for the player" (`CommandFn`'s `char* reply, uint32_t replySize`, 200-201) |
| capability gating with reasons | `SetCapabilityCheck` wired to `sco::caps::Has` (244-247); entries carry a reason string (`include/sco/caps.h:21-44`) |
| introspection | `ListCommands(const Command** out, size_t max)`, registration order (238-242) |
| crash containment for host-called plugin code | `CalloutGuard` around every task, event callback, command fn and `Invoke` done (266-275) |

So the studio table is **host commands** registered with `prefix = nullptr`, and the UI button,
the WebSocket method and `--drive` are three callers of one `sco::Invoke`. The engine keeps only a
small side table for what the SDK has no field for: `requires_window`, the argument defaults that
mirror each widget's current value, and a group/folder label for the UI. Rule 2 above is then
satisfied by the runtime rather than by new code, and rule 3's result codes are the registry's own
`Result` values. Where a control mutates editor state with no plugin meaning (frame selection,
mount, material sliders), register the same descriptor in the engine-side table and dispatch through
the identical shape, so the transport never learns two protocols.

Verify with one unit case: `plugins.invoke` (hostd, `src/hostd/plugins_handlers.cpp:22-66`) and
`engine.actions.invoke` produce byte-identical reply text for the same command, proving one bus.

### 2.2 The initial table (the 26 controls)

| id | title (unchanged text) | args | window? |
|---|---|---|---|
| `vfs.mount.directory` | Mount | `path` | no |
| `vfs.mount.unload` | (mount row) Remove | `mount_id` | no |
| `scene.frame` | Frame (F) | - | yes |
| `scene.remove_selected` | Remove (Del) | - | yes |
| `inspector.material.set_metallic` | ##metallic | `value 0..1` | yes |
| `inspector.material.set_roughness` | ##roughness | `value 0..1` | yes |
| `inspector.frame_selection` | Frame selection | - | yes |
| `product.load_profile` | Load Product Profile (sc-offline) | `root` (default = current field) | no |
| `product.export_bundle` | Export product bundle + SHA256SUMS | `destination` | no |
| `product.storage_inventory` | Owner storage inventory (read) | - | no |
| `product.asop_read_selection` | Read committed ASOP selection | - | no |
| `sco.mock.admit` | Admit mock capabilities | - | no |
| `sco.mock.revoke` | Revoke mock capabilities | - | no |
| `sco.mock.capability_readback` | Capability admission readback (read) | - | no |
| `sco.mock.fire_respawn_loadout` | RespawnLoadoutCompleted | - | no |
| `sco.mock.fire_mission_changed` | Mission changed (synthetic 1) | `mission`, `state` | no |
| `sco.mock.inject_entity` | Inject entity | `entity_class`, `actor` | no |
| `sco.panel.list` / `sco.panel.render_open` | Plugin controls | `panel_id` | yes |
| `stats.get` | (status read) workers/queued/in flight | - | no |

Every handler body calls the existing boundaries verbatim - `ScoHost::SetMockProfile`,
`MockCapabilities`, `ReadAsopSelection`, `StorageFiles`, `FireMockRespawnLoadout`,
`FireMockMissionChanged`, `AddMockEntity`, `MockEntityCount`, `ExportProduct`, `Plugins`, `Panels`,
`RenderPanel` (`include/scuba/sco/sco_host.h:56-70`). This phase is a **behaviour-preserving
refactor**: no new semantics, so the existing 51-test studio suite is the regression net.

### 2.3 Transports

1. **ImGui** (unchanged UX): buttons, headers and fields render from `All()` per group.
2. **WebSocket / MCP** (new wire methods): `engine.actions.list`, `engine.actions.get`,
   `engine.actions.invoke`, `engine.state.get`, `engine.state.await`, `engine.screenshot`. Serve
   them from a `hostd::Registry` + `hostd::Server` in **both** executables - `scuba-hostd`
   (headless, existing process, :7878) and `scuba-engine` (its own loopback port, default 7879) - so
   an agent drives the product with a window and CI drives it headless through identical verbs.
   Semantics, origin checks and naming carry over verbatim from the bridge: the host receives
   `panel` for widget events and `id` for reads; here `id` for actions, with `request_id` echoed.
3. **Headless drive**: `scuba-engine --drive SCRIPT.json [--data DIR] [--frames N]`. A script is
   `{"actions":[{"id":"sco.mock.admit"},{"id":"product.export_bundle","args":{"destination":"out"}}]}`.
   Exit code contract mirrors the existing `--frames` smoke (0 = every action `ok` and nothing
   logged at error level, 1 otherwise; see the `--frames` contract comment at `engine/app/main.cpp:9-10`)
   so ctest can use it. This is what makes the surface testable in CI and usable by an agent with
   no display.

New MCP tools in `tools/scuba-engine-mcp/server.mjs` (`TOOLS`, same `spec()` shape, same
`endpoint()` loopback guard, same no-retry policy for mutating calls):

```
plugin_commands()                      -> plugins.commands        (read, phase P0)
engine_actions()                       -> engine.actions.list   (read)
engine_describe({id})                  -> engine.actions.get    (read)
engine_state()                         -> engine.state.get      (read)
engine_await({generation, timeout_ms}) -> engine.state.await    (read; resolves on snapshot advance)
engine_invoke({id, args?, request_id}) -> engine.actions.invoke (MAY MUTATE)
engine_screenshot({path?})             -> engine.screenshot     (writes into --data capture dir)
```

`send_widget_event` stays the channel for plugin panels; `mock_set_service` /
`mock_move_camera` stay the scenario primitives. Do not fold them into the action table: they have
different owners, lifetimes and refusal semantics, and merging them would create two sources of
truth for the same capability. The bridge's tool list becomes the union, and its README documents
which surface owns what (studio controls = actions; plugin UI = widget events; world/scenario =
mock_game methods).

**Transport hosting, verified.** The registry and WebSocket machinery already form a reusable
static library: `scuba_bridge` compiles `src/hostd/json.cpp`, `src/hostd/ws.cpp` and
`src/hostd/handlers.cpp` (root `CMakeLists.txt:45-62`), and the editor reaches it transitively
(`scuba-engine` -> `scuba_engine_runtime` -> `scuba_engine_sco` -> `scuba_kernel` ->
`scuba_mock_game` -> `scuba_bridge`, `engine/CMakeLists.txt:36-76`). So the editor needs no new
server code: it constructs a `hostd::Registry` and a `hostd::Server` on a second loopback port
(default 7879) and registers one module per the documented pattern
(`src/hostd/modules.h:3-5`: write `<module>_handlers.cpp` with
`void Register<Module>Handlers(Registry&, HostContext&)`, add one line to the module list). The
registry's own invariant is the threading answer: "Handlers run on the host's main thread (the
sco-core game thread), one at a time" (`src/hostd/handlers.h:14-15`) - which is exactly the
main thread of `Editor::Run` (`engine/app/main.cpp:687`, loop at 734, `ed.sco.Tick()` at 736).
Reuse the existing shutdown seam for tests (`HostContext::requestShutdown`, `system.shutdown`,
`src/hostd/modules.h:26`) instead of inventing a second one.

### 2.4 State digest: build it on the diagnostics spine, not a bespoke struct

`sco-core` already ships the observation machinery this needs, designed for paged runtime clients:
`sco::diagnostics::Snapshot` (host metadata, game lifecycle, plugins with manifest validity and
reason, services, resolutions, redaction counters) behind a budgeted, generation-numbered
`SnapshotStore` with `Latest()` on any thread and `ReadPage(...)` cursors
(`sco-core: include/sco/diagnostics.h:80-131`). The studio digest is therefore
**Snapshot + engine-local additions** (scene, VFS mounts, capture geometry, `last_action`), and
every `ActionResult` carries the snapshot `generation` it was observed in.

That generation is what removes polling from agent workflows: `engine.state.await({generation, timeout_ms})`
resolves when the store advances, so an async flow (`sc_loadout_v1.submit` -> `status`) is awaited,
not slept on - the harness rule for async proof, satisfied with primitives that already exist.

The digest, canonically ordered so two digests compare byte-for-byte in tests:

```json
{ "schema": 1, "generation": 42,
  "host": {"sdk_version":"...","started":true},
  "game": {"services_requested":true,"services_built":true,"services_published":true},
  "plugins": [{"folder":"...","id":"...","state":"loaded","manifest_valid":true,"reason":""}],
  "capabilities": [{"name":"game.inventory.read","ready":true,"reason":""}],
  "entities": 12, "asop_selection": {"present": true, "empty": true},
  "storage": [{"owner":"asop_selector","bytes":8192}],
  "vfs_mounts": [...], "scene": {"objects": 3, "selected": "..."},
  "last_action": {"id":"...","request_id":"...","ok":true,"code":"ok"},
  "diagnostics": {"errors": 0, "warnings": 2, "truncated": false},
  "ui": {"width":1600,"height":900} }
```

Serialize with the existing bounded JSON model (`src/hostd/json.h`, used by every handler) and
respect the snapshot's own truncation/redaction flags - report them, never silently trim.
`capabilities` comes from `sco::caps::List` (name, ready, reason); `asop_selection` must keep
present/absent and empty/intentionally-empty distinct - that semantic exists in
`ScoHost::ReadAsopSelection` and the UI copy, and collapsing it would silently turn a saved empty
fleet into "no selection".

### 2.5 Screenshots as an engine service, not a desktop grab

Add `platform::Window::CaptureClientPng(path)` on the existing D3D12 path
(`engine/src/render/dx12/renderer_dx12.cpp`, `engine/src/platform/win32_window.cpp`,
`include/scuba/render/renderer.h`): copy the resolved backbuffer subresource, convert BGRA8 ->
RGBA8, and write PNG with a small in-tree encoder. **Verified constraint:** the fetched miniz
3.1.2 (`_deps/miniz-*/miniz.h`) provides `mz_deflate` and CRC-32 but **no** `write_png` helper,
so the encoder is IHDR + one `mz_deflate` IDAT stream + IEND, ~120 lines, no new dependency; put
it in `scuba_engine_core`, which already links `scuba_miniz` (`engine/CMakeLists.txt:29`).
Windowed and `--hidden` both work; the result is the client area only, so no desktop content and no
DPI mapping can enter the evidence. Rules: refuse absolute paths outside the `--data` capture root,
refuse to overwrite (the bundle-export rule), cap dimension and file size, and log one line. The
action `ui.capture_view` (`requires_window = true`, plus a camera-pose variant that reuses
`mock_game.camera.move`) replaces every computer-use capture in the QA plan.

## 3. Non-goals

No game certification: admitting capabilities here is synthetic (the UI copy and
`ScoHost::MockCapabilities` say so; keep the wording). No new capability, service or wire
primitive is invented for the game. No arbitrary pointer/address input - commands take typed args
only (no Pointer kind, `runtime.h:192-201`), and every action argument is a string, number or bool.
No live ASOP terminal synchronization (owner deferred next-start only). No auth model beyond
loopback. No plugin-facing ABI change: `sco_*` services and the ABI 1.1 minimum-host safe refusal
stay byte-identical, and nothing in `sdk/**` is touched. No web-studio dependency (it is in
maintenance mode). Nothing in P1-P5 depends on K-37; the SDK asks in section 8 are additive and
each degrades to "unavailable" on an older host.

## 4. Phases, files, acceptance

| # | Deliverable | Files | Acceptance (exact) | Rollback |
|---|---|---|---|---|
| P0 | Discovery of what already exists: `plugins.commands` (hostd) and a `plugin_commands` MCP tool returning name, title, help, arg defs with types, capability and live `caps::Has` readiness for every live command | `src/hostd/plugins_handlers.cpp` (one `register_handler` beside `plugins.invoke`, line 22), `tools/scuba-engine-mcp/server.mjs` (`TOOLS` + method allowlist), `server.test.mjs`, its README | `bun test tools/scuba-engine-mcp` green; against a live hostd the new tool lists every command `plugins.invoke` accepts, and a call with the wrong argument count still returns the existing `kInvalidParams` text | purely additive: delete one registration and one tool |
| P1 | Registry + behaviour-preserving UI refactor | new `engine/src/app/action_registry.{h,cpp}`; `engine/app/main.cpp` (controls ~426-671, `Run` at 687, loop at 734); `engine/CMakeLists.txt` | `cmake --build ... --target scuba-engine` clean; `ctest -R "^native_product_readback$|^engine_core$"` pass; every rendered studio button resolves to an id (new unit case) | revert one commit; UI untouched semantically |
| P2 | `--drive` headless runner + digest on the diagnostics spine | `engine/app/main.cpp`, new `engine/src/app/drive.{h,cpp}`, `include/scuba/sco/sco_host.h` (`InvokeAction`, `StateDigest`, snapshot generation pass-through) | new ctest `engine_drive`: admit -> readback(revoke) -> readback(admit) -> export, exit 0; `--frames N` smoke unchanged | drop the target from ctest |
| P3 | Editor control server + `engine.*` module on `scuba_bridge` | new `src/hostd/engine_actions.cpp`, one line in `src/hostd/modules.cpp`, editor wiring in `engine/src/platform`/`engine/app/main.cpp`, port flag in arg parse | `ctest -R "^e2e_hostd_ping$|^engine_integration$"` pass; unknown id -> `not_found`; non-loopback origin refused | methods are additive |
| P4 | MCP tools | `tools/scuba-engine-mcp/server.mjs` (`TOOLS`, method allowlist), `server.test.mjs`, `README.md`, `docs/engine/studio-mcp.md` §2 table | `bun test tools/scuba-engine-mcp` green; `node integration.mjs` drives `sco.mock.admit` against the real native engine and asserts the digest flip | revert tool additions (7 -> 13 tools, all additive) |
| P5 | Capture service | `engine/src/core/png_write.{h,cpp}` over `scuba_miniz`, `engine/src/platform/win32_window.cpp`, `engine/src/render/dx12/renderer_dx12.cpp`, `ui.capture_view` action | captured PNG opens, client-area size matches, file inside capture root; overwrite refused | remove the action id (reserved) |
| P6 | Docs + QA migration | this file linked from `docs/engine/architecture.md`, `README.md`, `studio-mcp.md`; roadmap E-15 | linkcheck G1 green; QA scenario table (section 6) executable with zero OS input | docs only |

Ship P0 immediately: it is a read-only projection of data the host already validates, so agents can
enumerate plugin commands before any engine refactor lands. Then P1+P2, which convert the flakiest
evidence in the program (native UI interaction) into deterministic runs with no protocol surface.
P3-P5 add the remote channel and capture.

## 5. Protocol invariants to keep

- Append-only ids; a `studio_mcp_unit` case pins the full id list as a golden file.
- Mutating calls are never retried by the bridge (existing policy, `Bridge` in `server.mjs`).
- One request id per invocation, echoed into the result; agent correlation is explicit.
- Bounded args and outputs (string caps, digest cap, PNG size cap); refuse over-limit with data.
- Loopback-only numeric IPv4 endpoint; foreign `Host`/`Origin` refused (`docs/protocol.md`).
- Structured errors carry the digest computed **before** the failed mutation, so a refusal is
  provably non-mutating.
- Windows: build/test with the Python-scripts CMake and ctest on PATH (the studio-MCP `project_*`
  profile already does this; `dev-cmd /host=x64` is NOT required for this tree, verified on the
  Windows dev host).
- Preserve uncommitted work in place: this tree carries fleet edits (relocation cancellation,
  quantum cleanup parity, inventory copy bounds). Branch off the current tip, one commit per phase.

## 6. QA migration: computer-use retired

| Scenario | Old channel | New invocation | Binary PASS |
|---|---|---|---|
| capability revoke/admit readback | OS click + window screenshot | `engine_invoke{sco.mock.revoke}` → `{sco.mock.admit}` | digest `capabilities[*].ready` false then true, with `engine_state()` captured between |
| ASOP save -> fresh process readback | panel clicks, display capture | plugin panel via `send_widget_event` (unchanged) + `engine_invoke{product.asop_read_selection}` in a new process | digest `asop_selection.present && empty` persists across restart |
| product profile load/refusal | typed path into field + click | `engine_invoke{product.load_profile,{root}}` on a good root, then a duplicate-manifest root | `code == "ok"`, folders/loaded counts; then `bad_arg`/refusal with digests equal before/after |
| bundle export + reopen | click, hash file | `engine_invoke{product.export_bundle,{destination}}` -> `--drive` reopen script over the exported root | export `ok`, hash count and mismatch = 0 on independent verify, reopen exit 0 |
| panel render + event readback | click/hover in window | `widgets.get` + `send_widget_event` (existing) | tree contains node and posted value in STATUS |
| spatial multi-angle evidence | desktop screenshots | `mock_move_camera` + `engine_invoke{ui.capture_view}` x4 poses, plus `--drive` at 1600x900 and 1280x800 client | four PNGs at exact client size, content differs per pose |
| geometry regression (clipped guidance) | manual window resize | digest `ui.width/height` sweep via `--drive --frames` | text-wrapped panels present at both sizes, no clipping in captured PNGs |

The pointer-delivery and DPI failure modes above become unreachable by construction: there are no
coordinates in this protocol.

## 7. Risks

1. **Registry/UI drift** if any control is hand-drawn outside the table — prevented by P1's
   "every button resolves to an id" test.
2. **Handler capture lifetime**: closures must not outlive `Editor`/`ScoHost`; clear the registry
   in `Run`'s teardown, keep handlers non-owning.
3. **Digest cost**: build on demand, cap the size, and cache cheap counters — never rebuild it per
   frame.
4. **Transport scope creep**: an agent could reach for arbitrary execution; the method allowlist in
   `server.mjs` (7 -> 13 named methods) is the security boundary, so new capability = a named
   method with a schema, not a general RPC proxy.
5. **Two ways to do one thing**: if the table and `mock_game.*` diverge, agents get ambiguous
   affordances. The mock methods stay scenario primitives; the action handlers call the same
   hostd/mock functions, so there is one implementation underneath.
6. **Headless vs windowed capability split** is real; `requires_window` must be honest, and CI
   asserts every headless-tagged action runs under `--drive --hidden`.
7. **The snapshot is budgeted and eventually fresh.** `SnapshotStore::Advance` allows <= 4 work
   units and a soft 1 ms per call, and `ReadPage` holds 64 tokens for 60 s
   (`sco-core: include/sco/diagnostics.h:109-115`). A drive must therefore read *the generation the
   action reported*, never assume "latest" equals "just now", and must release pages. State the
   staleness bound in `engine.state.get` (`snapshot_lag_ms`) so agents can reason about it.
8. **A second server is a second attack surface.** The editor's control port is loopback-only, on
   by default for the developer build, and refuses foreign `Host`/`Origin` exactly like hostd
   (`README.md`, `docs/protocol.md`). Ship a `--no-control-port` flag; document that local access is
   the trust boundary, same as the existing bridge.

## 8. What to change upstream, and what not to

**First, the negative result, because it is the important finding:** E-15 needs almost no SDK
change. The command registry (section 2.1bis) already provides descriptors, typed args, bounds,
capability gating with reasons, cross-thread dispatch, reply text, introspection and a callout
guard. Do not widen the raw ABI for agent control: `RawFn` is deliberately single-in / single-out
(`runtime.h:126`, `InvokeRaw` at 143), and structured multi-argument work is exactly what commands
are for. Action ids stay in the dotted `<x>.<y>` space, so `sco-plugin-check`'s existing name
rules apply to plugin-contributed actions for free.

The genuine gaps, in payoff order. Each is additive, size-gated where it touches a table, and keeps
ABI 1.1 minimum-host safe refusal: an older host answers `Unavailable` and the consumer degrades to
"not addressable", never a crash.

| # | Ask | Today (verified) | Proposed contract | Unlocks |
|---|---|---|---|---|
| S-1 | **Stable widget tags** | `sco::widgets::Node` = `kind, id, parent, flags, index, label, text, value, minimum, maximum, items, columns, severity, first_row, visible_rows, tooltip`, `Event` = `node_id, kind, index, value, text, row` (`include/sco/widgets.h:10-32`). `id` is numeric and renderer-assigned, `label` is display copy, so panel addressing breaks when a panel is restyled | node extension 1.2: optional `tag` (plugin-owned, stable, ASCII, <= 63 chars, unique in the panel) surfaced on `Node` and accepted where `node_id` is; `sco-plugin-check` refuses duplicate/invalid tags | durable plugin-panel automation: `send_widget_event{panel, tag:"contracts.accept"}` survives any UI redesign |
| S-2 | **Storage introspection** | `sco::storage` exposes `Options{dataRoot, quotaBytes, busyTimeoutMs, budgetMs, durable}`, `Start`, `Stop`, `Started`, `Table()`, `DatabasePath(id)` (`include/sco/storage.h:29-54`). No owner list, no table names, no row counts, no per-owner quota usage - which is why `ScoHost::StorageFiles()` stats files and reports bytes only | append `sco_storage_v2` to the storage service: `owner_ids`, `table_names(owner)`, `stats(owner, table, &rows, &bytes_used, &quota_bytes)`; read-only, game-thread, counts and sizes only, never keys/values | the studio inventory and persistence conformance assert "rows 3 after save, 3 after failed save" instead of "file exists" |
| S-3 | **Snapshot sections for a product host** | `diagnostics::Snapshot` covers host, game lifecycle, plugins, services, resolutions with generation, truncation and redaction counters (`include/sco/diagnostics.h:80-131`) - the right spine, missing product facts | add optional `ProfileMetadata{folders, loaded, disabled_by_policy, preflight_reason}` and `StorageMetadata{owners, tables, bytes}` sections to the existing snapshot, keeping the redaction discipline. Host-side only; **no change to `sco.diagnostics.publisher` 1.0**, whose header states it is "NOT a runtime observation endpoint" and that "runtime clients still need independent endpoint admission/consent" | the digest in section 2.4 becomes a projection of a contract, not an engine-private struct |
| S-4 | **ASOP selection schema** | committed selection is copied text read through the raw ABI (`asop_selector.snapshot`), next-start scope only | versioned record `{schema, ships[], spawned[], created_utc}` (the shape `sc_asop.h` documents as the intended future record) plus a raw method returning the version | agents validate a saved fleet structurally; optional G10 composition keeps its next-start semantics |
| S-5 | **Production mission/broker/economy mechanics** (K-37, unchanged) | `game.missions` / `game.economy` expose a fail-closed typed seam with no production adapter; the studio's synthetic adapter proves plumbing only | bounded core-native mechanics behind one detour owner, current-build sigcheck, no embedded game records; product policy stays in sc-offline | unlocks O-12 (retire the legacy mechanics that live in sc-offline's `src/contracts.cpp` and `src/missions.cpp`) |

No determinism work is needed: `sco::app::Tick(uint32_t nowMs)` takes the clock from the host
(`include/sco/app.h:121`), so `--drive` is deterministic by construction - the contract to state is
that action handlers use the tick's `nowMs` and never read wall time.

## 9. Beyond the SDK

1. **E-16, the console driver**: `scuba-engine --console` reads one JSON action per line from stdin
   and writes one result per line - the same registry as the UI, ws and `--drive`. It is ~40 lines,
   needs no socket (so it works through any PTY), and gives the agent a form that streams. Ship it
   in the same release as P3 so remote and local control have identical semantics; a shared golden
   transcript test runs the same script through `--console` and `engine.actions.invoke`.
2. **Wire self-description**: add `system.methods` (method names + schema version) so a client can
   feature-detect instead of assuming; every method response carries `"schema": 1`. Bump
   `PROTOCOL` in `server.mjs` only with a compatibility note (it is pinned at `2025-11-25`).
3. **CI**: promote the section 6 scenarios into a `--drive` suite run by ctest, and register it in
   the operator `project_*` profile (`tools/scuba-engine-mcp/developer.mjs`: build/check/test,
   absolute cwd, bounded timeout 100-1800000 ms, `taskkill /T /F` on timeout). Today those three
   tools run the whole build; the drive suite gives agents a fast targeted lane.
4. **Mock-conformance shared suite** (E-17): one fixture set per capability run against the core
   provider, the studio mock and (recorded) production, asserting the same reason codes. This
   session's inventory copy-boundary, quantum owner-cleanup and relocation `SAFETY` fixes are the
   test seeds; today each parity fix lives in exactly one suite, which is how they diverged.
5. **Keep computer-use for what only it can prove**: the real Win32 input path (the v0.7.8.1 menu
   `WndProc` / `QS_SENDMESSAGE` producer, O-11). Engine state, interactions and readback go through
   the control plane; OS input is no longer the source of truth for studio QA.
6. **Evidence hygiene**: captures are engine-authored PNGs under `--data`, so the desktop-content
   privacy hazard in section 1 disappears. Any surviving third-party capture path stays out of repos.
7. **Roadmap**: E-15 (this control plane, P0-P6), E-16 (console driver), E-17 (mock-conformance
   suite), and K-38 (SDK packet: S-1, S-2, S-3). S-4 rides O-14; S-5 is K-37.
