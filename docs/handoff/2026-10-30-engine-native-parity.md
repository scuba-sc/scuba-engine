# scuba-engine handoff — 2026-10-30 (draft)

Branch `handoff/native-visual-acceptance-20261030` = main `8373c52` + 1 commit
(`Complete bounded native plugin visual acceptance and fix private ImGui context setup`).
Details: `docs/native-parity-acceptance.md` and the three feature notes.
Canonical cross-repo runbook: sco-core branch `handoff/core-integration-20261030`,
`docs/handoff/2026-10-30-cloud-handoff.md`.

## Verified
- macOS Debug: full CTest 85/85 including new `native_designer`, `native_net_sandbox`, `plugin_surface`.
- Windows MSVC 14.29 Release with warnings-as-errors: 91/91 (including `test_plugin_visual` D3D offscreen readback of real `mp_chat` panel; 4-surface resize/recreate over 240 frames).
- Production defect fixed: private ImGui context was not selected after `CreateContext`, so glyphs failed to rasterize; regression added.

## Not verified
- Visible desktop interaction, focus/keyboard release, DPI/minimize, NetSandbox shutdown with no orphan peers.
- Browser-reference comparison; browser editor not retired.
- Designer gaps: Combo/List/Table, source parsing, C/C#/Lua output, folder export (disabled in UI).

## Pending elsewhere
- SDK pin stays rc.2. Engine #37 (Linux eight-peer zero-loss) needs a published 2.4.0 archive/hash plus a fresh Linux run.
- There is a separate dirty tree in the original local parity checkout; this branch is the committed version of that work.
