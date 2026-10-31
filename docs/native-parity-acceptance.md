# Native parity candidate acceptance

Local candidate based on live main `8373c52012d2f58fc773f796e199de81c16eb571`.
This is not browser retirement evidence, a release, or an in-game qualification.

## Integrated

- Plugin surface: editor-owned isolated ImGui context sharing the backend font atlas; selected plugin rendered through `ScoHost::RenderPanel` and SDK adapter, never a plugin-specific recreation. Each `PanelDraw` owns its own cloned UI. D3D uses that clone, binds its live backend viewport for recording, and retires the clone through the existing main-thread retirement queue. Its upload ring is sized for up to four surface passes plus the editor per buffered frame.
- Plugin surface picker is reopenable from View; `--diegetic [PANEL_ID]` starts with the chosen published panel (default first). Ray/plane UV mapping supplies local pointer, keyboard/text, focus and outside/Escape release. World transform axes use the view matrix columns. This is one camera-facing surface, not six browser environments.
- Designer is reachable from View: bounded outline, properties, palette, mutations, undo/redo, preview, C++ source/copy. Folder export remains visibly disabled in the actual application; the reusable model exporter seam was separately exercised by the worker, not wired as a misleading success action.
- NetSandbox is reachable from View: real separate hostd peers, dedicated broker worker, bounded start/stop/refresh/join/leave/send and counters. Its CMake-generated hostd path names the binary from this build. No peer widget rendering.
- Portable CMake target `scuba_native_tools` builds actual UI and SDK ImGui renderer on macOS, allowing real headless UI/input tests, not just model checks.

The feature-specific notes distinguish standalone evidence from this integrated candidate.
The bounded native surfaces do not establish complete web/editor parity or browser retirement.

## Executed evidence

From this candidate checkout:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
```

Configure and build exit 0. Full CTest: **85/85 passed**, 32.77 seconds, including all three new native tests. Logs: `build/build.log`, `build/ctest.log`. Final incremental rebuild and all three new tests passed again; log `build/build-final.log`.

`plugin_surface` loads the actual candidate's staged mp_chat plugin into ScoHost, submits actual mouse/text/backspace input through pinned ImGui + SDK renderer, and reads the plugin-published changed draft. Also asserts context/frame/texture ownership and clone lifetime, resizing, missing-panel failures, and ray math/refusals. This proves the portable host/UI callback path, not D3D pixels or application focus routing.

`native_designer` exercises validation, transactions, tree operations, undo/redo, C++ generation, callback refusal and actual headless ImGui draw output. `native_net_sandbox` uses **this candidate's newly built hostd**, real separate child processes and loopback broker lifecycle/counters, plus actual headless ImGui draw output. Tests were developed RED→GREEN; worker standalone sanitizer checks also passed, but the complete integrated suite was not sanitizer-built.

Windows-target MinGW C++20 syntax checks with repository warning flags and `-Werror` passed for `engine/app/main.cpp` (including configured hostd branch) and `engine/src/render/dx12/renderer_dx12.cpp`. These are syntax checks, not a Windows link or runtime pass. `git diff --check` passed.

## Subsequent Windows visual acceptance (2026-10-30)

An isolated copy on the Windows validation host at `D:/scuba-visual-20261030` was built with MSVC 14.29 / VS2019, Release, warnings-as-errors retained. Configure/build exits were 0/0; full CTest **91/91 passed**, 29.83 seconds. Evidence: `<SCRATCH>/engine-visual-evidence-20261030` (manifest, source archives, logs, JUnit and actual PPM/PNG readbacks).

The isolated `test_plugin_visual` scene deliberately draws no editor docks. It verifies publication `mp_chat.main` owned by `mp_chat`, renders through the real plugin adapter into a D3D target, routes synthetic pointer/text to the plugin-owned draft, and asserts the plugin publishes `VISUAL_ACCEPT_42`. Readback review can read Chat, No multiplayer session, Room, #general, Messages, Message, Send and the complete input marker. `verify_plugin_visual.py` provides an optional tesseract label gate; it fails on preserved broken pixels and passes on repaired pixels. This is not merely a changed-pixel test.

The first isolated readback exposed a production defect: ImGui CreateContext restores the previous context, so surface initialization configured the editor IO instead of its own. The private context lacked RendererHasTextures, locking lazy glyph rasterization; only scattered glyphs rendered. A regression failed on editor IniFilename corruption before the fix. Explicitly selecting the private context fixes both ownership and complete glyph rendering.

The four-surface test renders 240 WARP/debug-layer-requested frames, shares the real atlas, wraps buffered uploads, resizes the offscreen target 1024×768 → 1280×960 → 1024×768, and releases/recreates all four panel targets twice. Final images show plugin controls and the draft on all four surfaces. Debug-layer availability and hardware validation are not implied by requesting the layer. This proves bounded offscreen resize/retirement, not desktop DPI or minimize/restore.

The 91/91 result above is a handoff claim tied to the original Windows scratch evidence;
that evidence is not available in the cloud checkout. Hosted Windows CI run 41158980350,
job 124412035137, passed 88/88 including both visual tests on merge commit `40b7d7a`;
its tree is identical to PR head `6b9c53c`. A subsequent test change reports before/after
pixel differences inside each panel quad. The default test checks draw-data structure,
readback dimensions, and plugin input, but does not enforce a pixel threshold. Pass
`--pixel-delta-gate` after the normal arguments to opt into the experimental threshold;
it needs a fresh Windows D3D/WARP run and calibration. Pixel differences can have other
causes and do not establish readable text. The earlier result does not cover this change.

## Remaining Windows/platform and manual acceptance

1. Visible Windows interaction: select mp_chat, click draft on world quad, enter/edit/send text, toggle/slider/list/scroll controls in other plugins; verify owning plugin state. Outside/Escape focus transfer, keyboard release, drag release off-surface, camera suppression and subsequent recovery, dock overlap. Test panel replacement while focused, panel disappearance and reopen.
2. Visual review at narrow/wide layouts and display scaling: clipping, keyboard navigation, readable labels, all View reopening paths. Native NetSandbox worker startup/failure/stop during active work and application shutdown; verify no orphan peers.
3. Browser-reference comparison is still needed before retirement. No game or owner workspace has been touched.

## Explicit bounded parity gaps

- Designer: Combo/List/Table and extended attributes (tooltip/severity/paging/password), parsing source to model, C/C#/Lua outputs, persistence/interchange/sample selection, application folder export. Preview does not add gameplay bindings.
- NetSandbox: peer widget rendering/input, plugin refusal details, rich browser traffic views. Status polling is automatic at 400 ms while the dock is visible and expanded, subject to single-flight admission.
- Viewport: one fixed-size camera-following billboard only; no full six-environment authoring/perspective placement, occlusion-aware input or persistence.
- D3D backend remains Windows-only. Portable tests do not establish native GPU or visual acceptance. Browser remains in maintenance mode, not retired.
