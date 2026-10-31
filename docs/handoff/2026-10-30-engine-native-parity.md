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

## Coordinated source-development dependency

For source integration, use sco-core PR #304 commit
`571fe8e2c911d7c63c372103724896feaabe58f2`. Generate a development
`host-source-sc` archive from that clean checkout with
`python3 sdk/package.py --profile host-source-sc --out <archive-dir>`.
The archive `sco-sdk-host-source-sc-2.4.0.zip` has SHA-256
`065e5dbc0377bd51e1cf09a3282f3a63c0ec69f502ef512c371469120089b8e9`.
Configure the engine with `-DSCUBA_SDK_ARCHIVE_DIR=<archive-dir>` and
`-DSCUBA_SDK_HOST_SOURCE_SHA256=065e5dbc0377bd51e1cf09a3282f3a63c0ec69f502ef512c371469120089b8e9`.
The default remains the published RC2 archive until a published replacement is
available; CI using that default does not test the coordinated development source.
This local development archive is not a release or game acceptance evidence.
