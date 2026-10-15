# Diegetic viewport design contract

## Foundation
The foundation uses DOM-built `PanelModule` mounts with cleanup, a dark navy
shell, blue selection, system body text and compact bordered controls.
Inspected: shell, home, viewport, main, panel registry and panel registration.
There are no shared button/card classes or design tokens upstream.

## Tokens
`tokens.css` extracts the foundation background, raised surface, ink, border,
selection and success/warning colors. Added muted ink, stage grid, dark floor,
terminal glass and shadow tokens keep all new colors in one place. The stage
reads those same CSS colors into Three.js materials.
Spacing is a 4 px base: 1, 2, 3, 4, 6 and 8 units. Border is 1 px; radius is
6 px from the shell. Type: caption 11 px, small 12 px, body 14 px, h4 16 px,
h3 20 px, h2 24 px, h1 32 px. Body inherits the shell; instrumentation uses
the platform monospace stack. No new fonts or dependencies.

## Layout
The viewport is a bounded grid: title, wrapping controls, flexible stage,
readout. The stage owns canvas and panel overlay, clips off-screen geometry
and never scrolls. Controls wrap on narrow screens. Minimum stage height
is 320 px, or 280 px in the compact layout so wrapped live readouts remain
visible. A 680 px breakpoint stacks the title; the studio shell is unchanged.

## Primitives and states
Scoped DOM primitives: labeled select, labeled range with output, button,
checkbox, status readout and terminal surface. Controls share color, spacing,
radius and type tokens. Hover uses selection wash; focus-visible uses a
blue outline; disabled panel interaction follows projection visibility.
The terminal is a 360 x 420 local-pixel surface with a three-column keypad,
action tiles and status. Geometry dimensions are mathematical units, not
decorative spacing. Preset content is synthetic.

## Interaction
Native selects and ranges drive preset, yaw, pitch, distance and FOV.
Mode buttons expose aria-pressed. Pointer input maps to local panel pixels
through a camera ray/plane intersection for quads and inverse screen scale
for billboards. Keyboard activation remains native HTML behavior.
External surfaces mount into the same transformed element, including a
DOM renderer for sco.widgets. An action reports local coordinates and count.
Occlusion inserts a synthetic blocker and hides/disables the whole surface.
The plugin-surface selector uses the same native select primitive and the
ENG-2 renderer inside a scrollable 480 x 560 local-pixel surface. Snapshot
events refresh it; widget input is dispatched through hostd. Follow mock
camera is opt-in; its checkbox returns to orbit framing when unchecked.
The SDK's z-up pose maps to the viewport's y-up frame. There is no live link.
The shared shell wraps all six panel tabs on compact widths rather than
letting the merged registry widen the document. Selection uses a tonal wash.

## Motion
No ambient animation. Render on input and resize only; no reduced-motion
special case is needed. This is an engineering inspection stage, not a hero.

## Accessibility
Every control has a label. Status changes use a live region, the canvas has
an accessible description, the synthetic terminal uses native buttons.
Hidden, clipped and occluded panels are removed from focus with `hidden`.

## Accepted constraints
Dark theme follows the foundation. Panels crossing a clip plane are hidden
rather than partially clipped. Mode B maps a planar parallelogram, not a
curved display. Billboard occlusion uses the anchor; quad occlusion uses
the center ray. No claim of per-pixel occlusion or in-game verification.
