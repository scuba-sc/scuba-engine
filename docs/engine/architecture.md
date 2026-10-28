# scuba-engine native runtime: architecture

The native engine replaces the browser studio over the next phases: one C++20 executable that
hosts the sco-core kernel, renders 3D scenes and 2D tools, imports models, and mounts content
through a virtual file system. Phase 1 (this document's "today" column) is the foundation: the
sco-core bootstrap, the window/swap-chain loop, the ImGui shell, the asynchronous model loader
and the VFS. The browser studio and `scuba-hostd` keep working until the native editor reaches
parity (see [Phases](#phases)).

Nothing here has run inside Star Citizen. The engine uses mock game services only.

## Principles

1. **sco-core is the boundary.** The engine is a host of the latest published SDK kit (SDK 2.4.0
   release candidate 2, tag sdk-v2.4.0-rc.2, pinned by SHA-256 in `cmake/ScubaSdk.cmake`). Plugins reach the engine only through
   SDK services and contracts. Plugin UI arrives as `sco.widgets` panels; no plugin sees an ImGui
   type, a D3D12 object or an engine pointer.
2. **One owner per thread.** The main thread owns the window, the sco-core game thread, the UI
   and the scene. The render thread owns the GPU. Worker threads own nothing; they transform
   inputs into immutable outputs.
3. **Immutable handoff.** Data crosses threads as immutable, reference-counted values (decoded
   models, cloned UI frames, frame packets). No subsystem reaches into another's state.
4. **Deterministic lifetime.** GPU objects die only after the frame that used them is fenced;
   loader jobs keep their shared state alive; VFS handles keep their backend alive after an
   unmount.

## Layout

```
include/scuba/              public engine headers (no OS or GPU types)
  core/      status.h  log.h  job_system.h
  vfs/       file_system.h  virtual_file_system.h  pack.h
  asset/     model.h  model_loader.h
  sco/       sco_host.h
  platform/  window.h
  render/    camera.h  picking.h  renderer.h
  ui/        input_router.h  ui_layer.h
engine/
  CMakeLists.txt            targets below
  src/core|vfs|asset|sco|platform|render|ui   implementations
  src/render/dx12/          the D3D12 backend
  app/main.cpp              the editor (scuba-engine.exe)
  tools/scuba_pack.cpp      scuba-pack: build, list, verify .scpak
  content/                  sample content mounted at the VFS root
cmake/ScubaDeps.cmake       Dear ImGui (docking) and miniz, pinned by SHA-256
tests/test_engine_core.cpp  VFS, overlay, jobs, loader, picking (no GPU)
```

| Target | Platforms | Contains | Links |
|---|---|---|---|
| `scuba_engine_core` | all | jobs, log, VFS backends and stack, model decode and loader, camera and picking math | `sco_vfs`, `scuba_assets` (bounded OBJ/CGF readers), miniz |
| `scuba_engine_sco` | all | `ScoHost`: kernel boot, tick, plugins, widget panels | `scuba_kernel` (sco::app + mock game) |
| `scuba_engine_runtime` | Windows | Win32 window, D3D12 renderer, ImGui shell, input router | the two above, Dear ImGui, `sco_widgets_imgui` |
| `scuba-engine` | Windows | the editor | runtime |
| `scuba-pack` | all | .scpak tool | core |

CMake: `SCUBA_BUILD_ENGINE` (default ON). With it on, Windows builds turn on the kit's
host-only `SCO_WIDGETS_IMGUI` adapter and point it at the engine's ImGui. The language level is
C++20; nothing requires C++23.

## Threads and the frame

```
main thread (= sco-core game thread)                 render thread            workers
───────────────────────────────────────             ─────────────            ───────
Pump OS messages
ScoHost::Tick            plugins' tasks/commands/events
PumpLoader               finished models ─► Renderer::Upload ──────────────┐
UiLayer::BeginFrame      waits: render thread done with UI N-1             │  ModelLoader jobs:
  input → InputRouter    UI blocker → move tool → camera/picking           │  VFS read ─► decode
  editor panels + sco.widgets panels                                       │  ─► ModelData (immutable)
UiLayer::EndFrame        clone draw lists                                  │
Renderer::Submit(N) ───────────────────────────────────────►  uploads, scene, grid, UI N
                         blocks while packet N-1 still queued   signals "UI N consumed"
                                                                present, fence; retire
```

- The main thread runs at most one frame ahead of the render thread.
- The ImGui backend is touched by one thread at a time: the render thread records frame N's UI
  (including font and texture updates) before it signals, and `BeginUiFrame` waits for that
  signal before the main thread starts frame N+1.
- Cloned UI frames are destroyed on the main thread (the renderer hands them back in `Submit`).

## sco-core binding (`include/scuba/sco/sco_host.h`)

`ScoHost::Start` boots `sco::app` headless with the published kit, publishes the mock game's
synthetic `game.*` tables (`scuba_kernel`, shared with `scuba-hostd`), and loads plugins from the
staged plugin root. The calling thread becomes the game thread; `Tick` runs once per frame before
the UI is built. Plugins appear in the Plugins panel; every `sco.widgets` panel gets a dockable
window drawn by the kit's `sco::widgets::ImGuiRenderer`, whose events go back through sco-core's
validation to the owning plugin. Engine subsystems talk to sco-core only through `ScoHost`, so a
later engine-side service (asset intake, scene queries) is published as an SDK service, never
reached by pointer.

## Virtual file system ([vfs.md](vfs.md))

A priority-ordered stack of mounted backends (directory, zip, `.scpak`, memory) under mount
points, with sco-core's game-file overrides (`sco::vfs`) composed on top. Every engine read goes
through it: the loader, the content browser, glTF external buffers.

## Asset intake (`include/scuba/asset/`)

`ModelLoader::Request(path, priority)` → job system → VFS read → decode → immutable `ModelData`
→ `TakeCompleted` on the main thread → `Renderer::Upload`. States: queued, reading, decoding,
ready, failed, cancelled; duplicates are coalesced by normalized path, and failures retry on the
next request.

| Format | Today | Notes |
|---|---|---|
| glTF 2.0 (`.gltf`, `.glb`) | yes | accessor decode (float positions/normals, normalized-int UVs, u8/u16/u32 indices), base64 and VFS-relative buffers; Y-up converted to Z-up; node transforms, skins, sparse accessors not yet |
| OBJ | yes | scuba_assets bounded reader; polygons fan-triangulated; Y-up converted |
| CryEngine `.cgf` / `.cga` | yes | scuba_assets bounded reader (static mesh chunks, see [assets.md](../assets.md)); already Z-up |
| FBX | refused with a reason | planned: ufbx (MIT) |

Missing normals are generated (area-weighted). Bounds are computed per mesh and per model.

## Renderer (`include/scuba/render/renderer.h`)

The front end is backend-neutral; D3D12 is the first backend, Vulkan the next.

- **D3D12 today:** one direct queue, flip-discard swap chain (3 buffers, tearing when vsync is
  off), per-buffer command allocators and an upload buffer for frame constants and instance data,
  a fence-gated retire list for GPU objects, WARP fallback (`--warp`) for machines without a GPU.
- **Meshes:** each model lives in one default-heap buffer (positions, normals, indices per mesh),
  copied from a staging buffer on the render thread. Draws are grouped by model: one instanced
  `DrawIndexedInstanced` per mesh per model, per-instance world matrix and material in an
  instance stream.
- **Shading:** metallic-roughness PBR (GGX distribution, Smith geometry, Schlick Fresnel), one
  directional light plus a sky/ground ambient term, tone mapped and gamma corrected. Materials
  are constant factors in phase 1; texture sets come with the texture streaming phase.
- **Grid:** an analytic line grid generated in the vertex shader (no vertex buffer), axes
  colored.
- **Picking:** CPU ray tests against the decoded meshes (bounds, then exact triangles), with
  the instance transform undone. The move tool drags the selection across its ground plane. A GPU
  id buffer and full translate/rotate/scale gizmos are phase 2.

## 2D shell and input (`include/scuba/ui/`)

ImGui (docking branch) runs the editor: a full-window dockspace whose central node is
see-through, so the 3D viewport shows there and panels dock around it. The 3D pass is clipped to
the central node; the UI pass draws over the whole window.

Input goes through an `InputRouter`: layers are asked in order and answer with a reply (unhandled,
handled, capture, release) instead of changing global state. A capturing layer receives every
pointer event until it releases or its button goes up. The editor's chain: UI blocker (ImGui
wants the pointer, or it is outside the viewport) → move tool (Ctrl + left drag) → camera and
picking (right drag orbit, middle drag pan, wheel dolly, left click select, F frame, Del remove).

## Phases

| Phase | Scope | Exit |
|---|---|---|
| 1 (this branch) | core, VFS, sco host, D3D12 loop, ImGui shell, async OBJ/glTF/CGF intake, PBR-lite, CPU picking, move tool | `engine_core` + `engine_smoke` (WARP) green on Windows |
| 2 | gizmos (translate/rotate/scale, snapping), GPU id-buffer picking, scene save/load through the VFS, FBX via ufbx, glTF node hierarchy | editor round-trips a scene |
| 3 | textures and mip streaming (priority IO queue, budget-driven residency), material textures, DDS | Asset Lab parity |
| 4 | studio parity: Widgets, Designer, Viewport (diegetic panels), NetSandbox panels in native UI | browser studio retired; hostd remains for headless CI |
| 5 | Vulkan backend; Linux build of the editor | cross-platform CI (roadmap E-5) |

## Provenance

All engine code in this repository is written for scuba-engine. Third-party code is fetched at
configure time, pinned by SHA-256 and never vendored: Dear ImGui (MIT) and miniz (MIT); see
[THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md). The `.scpak` format is scuba-engine's own.
