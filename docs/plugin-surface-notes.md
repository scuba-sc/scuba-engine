# Isolated plugin surfaces

`ui::PluginSurface::Init(ImGuiContext*)` creates a private ImGui context sharing the
main context's font atlas. Call after renderer initialization. Main/game-thread only.
`Render(ScoHost&, panelId, width, height, dt, SurfaceInput, UiFramePtr&)` renders only
the named live sco.widgets panel through the SDK ImGuiRenderer and ScoHost, so events
still undergo SDK validation and dispatch to the owning plugin. Failure clears output.
The caller's context is restored on success, failure and unwinding.

`SurfaceInput` uses local render-target pixels, five held mouse buttons, wheel deltas,
focus, named ImGui key/modifier transitions and UTF-8 text. Pointer validity and keyboard
focus are distinct: preserve focus for typing after a click, and send focused=false to
release input when switching surfaces. Feed button-up even when the pointer leaves the
quad. Do not also route a consumed click to editor picking. Keys are transitions, buttons
are snapshots; text and wheel are per-frame events.

`HitPanel(ray, world, hit)` and the camera/viewport overload return top-left-origin UV
for the renderer's centered unit XY quad. Front is transformed XY winding (+Z in identity).
They refuse back faces, parallel/behind/outside hits, collapsed quads and nonfinite values.
Row-vector affine translation, nonuniform scale, rotation and shear are supported. Map
UV to local pixels via `x=u*width`, `y=v*height`; avoid routing edge UV=1 beyond the target.

Clones own their vertex/index/command buffers and survive another Render or destruction
of PluginSurface. They do not own GPU textures or the main atlas. `OwnerViewport` is null;
`Textures` points to the MAIN context's PlatformIO texture list, not the private context.
Keep the main context/atlas/backend alive through all queued clones, including shutdown.
Use the existing BeginUiFrame/Submit synchronization and main-thread retirement. The main
context must begin a frame before a private frame (ImGui 1.92 shared-atlas requirement),
and finish its frame before submitting, so font texture updates are listed. Do not render
these draw lists with the editor draw data or reuse editor DisplaySize. The renderer must
accept a distinct UiFramePtr per PanelDraw and render it at the surface's own dimensions.

## Portable evidence

Built `scuba_engine_sco` and the actual staged `scuba_plugins` with CMake. Then compiled
`tests/test_plugin_surface.cpp`, `engine/src/ui/plugin_surface.cpp`, the pinned four core
ImGui sources and the SDK's actual src/widgets/imgui_adapter.cpp against the existing
`test_sco_host_product` link libraries. Ran the binary from the repository root with
`build/plugins/runtime` (optional argv[1] overrides plugin root).

Observed RED before implementing missing surface API, panel rendering, input forwarding,
ray helpers and main texture-list ownership. GREEN: `plugin_surface: PASS`. Repeated with
AddressSanitizer + UndefinedBehaviorSanitizer on owned code, ImGui and the SDK adapter:
`plugin_surface: PASS`, no diagnostics. Existing host libraries/plugins were ordinary builds.
Both owned translation units also compile under -Wall -Wextra -Wshadow -Wconversion
-Wsign-conversion -Werror.

The headless test loads real native mp_chat, types into its actual ImGui text field through
mouse press/release + UTF-8 events, ticks ScoHost and checks the plugin-published draft changed.
A Backspace key then changes that published draft again. It also checks caller-context
restoration, concurrent editor-frame preservation, independent clones across resizing,
missing panels/invalid dimensions, texture-list lifetime and destruction before clone retirement.
Ray tests cover UV orientation, translation/scale/shear, camera ScreenRay and refusal cases.

Not proven: D3D12/GPU rendering, Win32 input translation, real game behavior, occlusion against
scene meshes, keyboard focus policy in the application or GPU retirement in the integration.
Wheel forwarding is implemented but not yet asserted through a plugin's scroll state. No
Windows remote was used for this standalone evidence. The candidate subsequently integrated the
surface in CMake, the application and renderer; see [candidate acceptance](native-parity-acceptance.md).
