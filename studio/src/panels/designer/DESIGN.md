# Widget designer design system

## 1. Direction
An instrument workbench, extending the foundation's dark navy studio. The canvas
is a recessed stage between an outline and inspector; editable source is a
separate lower workbench. No external fonts, imagery, or game assets.

## 2. Color
Tokens in `tokens.css`: canvas #0d1117, surface #111823, elevated #192330,
well #0a0e14, border #263040, ink #d8dee9, muted #9aaabd, accent #8ccaff,
success #6fd08c, warning #e5b567, error #ff9d9d, selection #263c52.
These extend the existing shell palette without changing other panels.

## 3. Typography
Existing system UI stack for controls; Georgia for the panel's editorial title;
ui-monospace for source and identifiers. Scale: caption .75rem, small .875rem,
body 1rem, h4 1.125rem, h3 1.25rem, h2 1.5rem, h1 2rem. Line height 1.5.

## 4. Geometry
Four-pixel spacing scale: .25, .5, .75, 1, 1.5, 2rem. Radius .375rem.
One-pixel rules, 44px minimum control target, 20px checkbox inside its 44px
label target, 2px keyboard focus ring.
Outline 13rem, inspector 16rem, canvas minimum 20rem, source height 22rem.
Wide workbench threshold 64rem; below it sections stack in reading order.

## 5. Primitives
`element`, `button`, `field`, and `select` in `primitives.ts`; all controls use
native semantics and explicit labels. `designer-box` is the reusable surface.
Buttons: normal, hover elevated, pressed selection, disabled muted.
Outline/canvas selection uses a tonal wash, not an accent border.
All focus-visible controls receive an accent ring.

## 6. Layout
The foundation main owns vertical scrolling. Desktop: outline / stage /
inspector; source spans all columns. Mobile: toolbar, outline, stage, inspector,
source. Source owns its own two-dimensional code scroll; no other region
overflows horizontally. Toolbar actions wrap.

## 7. Interaction and accessibility
Tree selection and insertion are explicit buttons. Move up/down and parent
selection provide keyboard equivalents to reordering. Form edits regenerate
source immediately; code input parses immediately and commits only valid source.
Errors use a live status region and never discard the last valid design.
No decorative animation. Synthetic preview controls update model state.
Preview uses the shared ENG-2 snapshot/VNode/DOM renderer, not a parallel
widget implementation. Node selection retains the existing tonal wash.

## 8. Accepted limits
This preview renders retained widget semantics, not an in-game renderer.
C# and Lua builders in SDK 2.2.0 expose only five node kinds and no 1.1 tail.
Unsupported designs show capability errors, never approximate source.
