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
`1b6782bc5c8b7d189cdff4c83e81957f09af35d7`. Generate a development
`host-source-sc` archive from that clean checkout using the engine checkout's
`python tools/package_dev_sdk.py <core-checkout> <archive-dir>`; on Windows,
configure `core.autocrlf=false` before checking out core. The adapter preserves
Git executable modes on both platforms. On Linux, core's unmodified
`python3 sdk/package.py --profile host-source-sc --out <archive-dir>` produces
the same bytes when run from the core checkout.
The archive `sco-sdk-host-source-sc-2.4.0.zip` has SHA-256
`9db31895ffc0806e663fca36be4bbad2d25c231cf9ae894008743d345e78ae3e`.
Configure the engine with `-DSCUBA_SDK_ARCHIVE_DIR=<archive-dir>` and
`-DSCUBA_SDK_HOST_SOURCE_SHA256=9db31895ffc0806e663fca36be4bbad2d25c231cf9ae894008743d345e78ae3e`.
The default remains the published RC2 archive until a published replacement is
available. Separate `coordinated core` CI jobs generate the development kit on
Linux and Windows, check the checkout commit, archive hash, embedded source
metadata and file checksums, and build and run the engine CTest suite against it.
The jobs preserve checkout bytes with `core.autocrlf=false`. Their packaging
adapter uses the clean Git index for executable modes because Windows does not
provide POSIX executable permission bits; it otherwise runs core's packager.
The normal `native` CI jobs continue to exercise the published RC2 default.
Neither development CI nor its archive is a release or game acceptance evidence.
