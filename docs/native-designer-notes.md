# Native Designer: bounded authoring dock

## Implemented bounded surface

`DesignerModel` is standard C++20, without ImGui, SDK or OS includes. It owns validated
snapshots and up to 50 undo entries. Mutations are transactional: invalid documents leave the
previous document and history untouched. Limits: 256 nodes, 16 nested node levels, unique
nonzero keys, 31-byte safe/nonreserved plugin ID, 63-byte nonblank title and 255-byte node
strings, valid UTF-8 without NUL. Only Row/Column accept children. Slider ranges must be finite
and ordered; toggle values are zero/one.

`DesignerDock::Draw()` is real ImGui code, compiled by defining `SCUBA_DESIGNER_IMGUI` on the
implementation translation unit. It supplies a dockable window, outline selection, palette,
child/root insertion, sibling ordering, subtree deletion, property editing, undo/redo, synthetic
interactive preview, generated read-only C++ source, clipboard copy and optional folder export.
The regular ImGui window scrolls vertically; source has a horizontal scrollbar. It has no
Win32/D3D12 dependency itself. Call it only on the main thread between UiLayer BeginFrame and
EndFrame. Do not draw from worker/render threads.

The exporter seam accepts `(id, "cpp", source)` and returns a verified folder receipt or throws.
No exporter means a visibly disabled export action, not fake success. The existing hostd
`ExportDesignerPlugin` is reused through this seam; this layer does not implement another file
writer or subprocess runner.

Executed locally with clang++ C++20 and repository warning flags including `-Werror`:

- Portable model tests, also under AddressSanitizer and UndefinedBehaviorSanitizer.
- Headless real ImGui 1.92.8 docking: three frames, every supported widget kind, nonempty draw
  data. This is not proof of Windows rendering, visual layout or all click handlers.
- Existing hostd exporter invoked with this model's generated source and a local SDK scaffold:
  actual plugin.ini/source/CMakeLists.txt written, source and plugin.ini read back by the test.
- Generated all-supported-widget C++ source passed clang++ `-fsyntax-only` against local SDK
  headers. No plugin binary was linked or loaded.

Tests were introduced and observed red before the corresponding model, mutation, generator,
export callback and ImGui dock implementations. A depth-16 boundary regression exposed and
fixed an off-by-one validation bug.

## Application integration and export limit

The candidate integrates the dock into the editor's View menu. The application does not
wire an exporter: folder export remains disabled. The exporter callback is a reusable seam,
not evidence that the application exports or installs plugins. The standalone exporter test
above exercises the existing hostd exporter separately. `Draw()` belongs on the UI thread
between BeginFrame and EndFrame; keep the dock alive across frames.

## Standalone reproducibility

From the repository root (binary path outside the repository):

```sh
clang++ -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Werror \
  -Iinclude -Itests tests/test_native_designer.cpp engine/src/ui/designer.cpp \
  -o "$TMPDIR/native-designer-test"
"$TMPDIR/native-designer-test"
```

For headless ImGui tests add `-DSCUBA_DESIGNER_IMGUI`, `-isystem "$IMGUI"`, and the actual
`$IMGUI/imgui.cpp`, `imgui_draw.cpp`, `imgui_widgets.cpp`, `imgui_tables.cpp` sources. No platform
backend is needed. For the optional real exporter test define `SCUBA_DESIGNER_EXPORT_TEST`,
`TEST_DESIGNER_SDK_ROOT`, `TEST_DESIGNER_OUTPUT`, `TEST_DESIGNER_PYTHON`; include `src`, link
scuba_bridge (or compile its json.cpp, handlers.cpp, designer_export.cpp with the existing
SCUBA_DESIGNER_SDK_ROOT/SCUBA_DESIGNER_PYTHON definitions). Use a new output directory per run:
the existing exporter intentionally refuses overwrite.

## Deliberate gaps versus browser Designer

This is not full Designer parity. Supported: Column, Row, Label, Button, Toggle, Slider,
TextInput and disabled state; C++ generation/export only. Missing: Combo/List/Table, tooltip,
severity, paging, password mode, source-to-model parsing/editable source, C/C#/Lua generators,
JSON persistence/import, browser design interchange and sample selection. Preview state edits
are undoable authoring edits; exported plugins have the existing browser generator's static
panel behavior, not live game bindings. Button preview activation runs no command.

The generated file uses the SDK builder/plugin wrapper, but is not advertised as importable
by the browser's strict generated-source parser. Model accepts 16 levels deliberately rather
than unbounded recursion. Invalid edits are rejected immediately, so intermediate invalid
slider/text drafts do not persist. A separate staged property editor would improve that UX.

The standalone headless evidence does not establish native Windows rendering, clipboard behavior,
plugin loading or visual/click acceptance. See [candidate acceptance](native-parity-acceptance.md)
for the integrated evidence and remaining manual checks.
