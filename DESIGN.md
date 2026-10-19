# Native editor workspace

## Direction

An inspection tool with a quiet graphite shell, a clear scene viewport and a
muted teal selection accent. Panels form one workspace rather than floating
utilities. Native ImGui/D3D12 remains the implementation; no browser replacement.
The owner's supplied screenshots are the before-state, not a visual target.

## Layout

- Left dock: Scene and Assets; approximately 240 px at 1600 px window width.
- Center: uninterrupted 3D viewport, with concise camera controls at its edge.
- Right dock: Inspector and Plugin controls; approximately 330 px.
- Bottom dock: Console, Mounts, Asset loader and Stats; approximately 180 px.
- Menus at top; quiet renderer/mock-session summary in a bottom status bar.
- Each dock owns its scroll. Minimum floating panel 220 x 120. Below 1280 px,
  tabs retain controls without overlapping the viewport; panels remain resizable.
- New versioned saved workspace avoids adopting the previous fragmented layout.
  View > Reset workspace restores the usable default without clearing scene data.

## Tokens

Graphite background (0.075, 0.086, 0.098), panel (0.105, 0.118, 0.133),
raised control (0.145, 0.165, 0.184), border (0.22, 0.25, 0.28),
text (0.89, 0.91, 0.93), secondary (0.57, 0.63, 0.68),
selection (0.19, 0.47, 0.47). Semantic warning/error colors remain distinct.
Segoe UI 15 px on Windows, built-in font fallback. Spacing: 4/8/12/16 px;
panel padding 12, control padding 8 x 5; rounding 3 for controls, 0 for docks.

## Primitives and interaction

Docked panel, tab group, section heading, full-width property field, compact
action button, selected scene row, wrapped metadata, scrollable log and status
bar. All use one ImGui style. Hover/focus/active states are visible; numeric
fields retain direct entry and keyboard navigation. Destructive scene removal
stays an explicit action. Plugin controls render through the SDK adapter inside
one named dock, preserving host validation and callbacks.

## Verification and accepted limits

Build with MSVC /WX; real WARP scene with cube/pyramid and independent plugin;
check desktop and smaller window, reset layout, Inspector, tabs and plugin panel.
Camera must still orbit/pan/frame within the central viewport. No game evidence.
ImGui accessibility tree remains limited; native keyboard navigation is enabled.
No new icons, web components, animations, remote fonts or shipped font files.

## Native plugin designer

Designer is a left-dock tab using the existing panel, section and full-width
field primitives. A bounded ordered widget list owns its scroll. Selection
exposes label, disabled state and kind-specific values; add/remove/reorder are
explicit actions. Preview is an in-editor design preview, not game evidence.
Export is an asynchronous named action with busy/success/refusal feedback, never
an implicit build or load. It generates a fresh C plugin through the existing
headless generator and published SDK scaffold; existing folders are not replaced.
The initial native editing subset is Label, Button, Toggle, Slider and TextInput
inside one Column. Other generator kinds remain supported by headless tooling.
