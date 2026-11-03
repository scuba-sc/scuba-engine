# Windows editor verification

This checklist covers the native scene editor on Windows. It complements [Scene documents](scene-document.md), which defines the file format, editing behavior and limits.

**Code target recorded for this checklist:** branch `codex/scene-document-v1`. Record `git rev-parse HEAD` immediately before testing and use that exact SHA in the results; do not treat an earlier snapshot as evidence for a later build.

**Portable check recorded for source SHA `84e09798aa992073a7e7515f769723e377c01cfc`:** Linux configured against the pinned SDK 2.4.0 source, built `test_engine_core`, and passed CTest `engine_core` (1/1). This covers portable project-root rejection and unsaved-action gate states; it does not compile the Win32/D3D12 app. The Windows editor build, interactive UI checks and game acceptance remain deferred until the scaffold is complete. A headless test does not establish Windows rendering or input behavior.

## Prerequisites and build

- Windows x64, Visual Studio 2022 with **Desktop development with C++** and a Windows 10/11 SDK.
- CMake 3.21 or newer and Python 3 available on `PATH`.
- Network access for the first configure, or local copies of the exact pinned SDK and third-party archives. CMake verifies their SHA-256 before use; the default profile is stable sco-core SDK 2.4.0.

From the repository root, in PowerShell:

```powershell
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 `
  -DSCUBA_BUILD_ENGINE=ON -DSCUBA_BUILD_TESTS=ON -DSCUBA_WERROR=ON
cmake --build build-win --config Release --target scuba-engine test_workspace_layout test_engine_core test_asset_browser
ctest --test-dir build-win -C Release -R "^(workspace_layout|engine_core|asset_browser_model)$" --output-on-failure
```

For offline configure, pass `-DSCUBA_SDK_ARCHIVE_DIR=<directory containing sco-sdk-host-source-sc-2.4.0.zip>` and `-DSCUBA_DEPS_ARCHIVE_DIR=<directory containing the pinned dependency archives>`. Keep the archive names and hashes required by `cmake/ScubaSdk.cmake` and `cmake/ScubaDeps.cmake`; do not substitute an RC or unpublished kit. A successful CTest run covers portable logic, not D3D12 visuals.

## Interactive editor checks

Use a new temporary project and a separate temporary working directory. The scene project stores files in the project directory; ImGui stores `scuba-engine-layout.ini` relative to the process working directory. Keeping both under a unique temporary parent avoids changing repository content or an existing workspace.

```powershell
$case = Join-Path $env:TEMP ("scuba-editor-check-" + [guid]::NewGuid().ToString("N"))
$project = Join-Path $case "project"
$workspace = Join-Path $case "workspace"
New-Item -ItemType Directory -Path $case | Out-Null
New-Item -ItemType Directory -Path $project, $workspace | Out-Null
$editor = Get-ChildItem .\build-win -Recurse -File -Filter scuba-engine.exe |
  Select-Object -First 1 -ExpandProperty FullName
if (-not $editor) { throw "scuba-engine.exe was not found under build-win" }
Push-Location $workspace
try { & $editor --project $project --scene main } finally { Pop-Location }
```

Check each item and record **Pass**, **Fail**, or **Blocked**, plus the evidence noted at the end.

| Check | Expected result |
| --- | --- |
| Project path entry | From an in-memory scene, use File > Open project... and enter a valid existing temporary project folder. The `main` scene opens, or a new unsaved `main` scene is shown if the file is absent. A missing path, regular-file path, malformed scene, or project root overlapping a mounted directory/archive reports an error and leaves the current document and save state intact. |
| First-run workspace | Scene is docked left, Inspector right, Content bottom, and the 3D viewport occupies the center. Scene, Inspector and Content are the selected tabs. Mounts, Plugins, Asset loader, Log and Stats share those docks. |
| Saved workspace | Move a panel, leave the editor open long enough for ImGui’s settings save, close it, and relaunch from the same working directory. The moved layout is restored rather than reset to defaults. |
| Project scene round trip | Start with the empty temporary project. Add a group, place `models/cube.obj` under it from Content, save with Ctrl+S, close cleanly, and reopen the same project. The group, model reference and transforms remain. Inspect `<project>\main.scene.json` as supporting evidence. |
| Unsaved guard | Make an edit and close the window. Cancel keeps the editor and edit open; Save persists it; on a separate edit, Discard exits without changing the saved scene. Repeat through File > Open project... and the scene browser: Cancel keeps the current project, Save persists the current scene before switching, and Discard switches without saving. |
| Hierarchy and Inspector | Add/nest groups, rename a node, reparent while preserving its world transform, and delete a test subtree. Confirm rejected cycle or unrepresentable reparent leaves the document unchanged. Edit translation, rotation and positive nonuniform scale in the Inspector. |
| Viewport tools and history | Select the model. Try Move, Scale and Rotate gizmos, then Undo and Redo each drag. Escape cancels a valid active drag. A near-edge-on rotation ring may be refused with a status message; positive scale limits and unsupported mirrored scale must be enforced. |
| Browser and placement | Browse `models`, search for `cube`, switch Tiles/List, and confirm `README.md` remains visible but unsupported. Tiles show text type placeholders, not image thumbnails. Check double-click placement, Place with click preview, Escape cancellation and viewport selection. |
| Failure and recovery | Use the isolated missing-asset fixture below. The failed node remains in the outliner with a diagnostic; after repairing and remounting the asset source, Retry failed assets loads it without replacing the scene node. |
| Source-directory protection | Record a checksum for a mounted asset file before and after project open/save attempts. The editor refuses a project root that is equal to, inside, or contains a mounted directory or archive, and the source file remains byte-identical. |

### Missing-asset and retry fixture

In a second temporary project, create a valid scene file referencing an asset path that does not yet exist. This one-node schema-v1 fixture can be written while the editor is closed as `<project>\main.scene.json`:

```json
{"format":"scuba.scene","schema_version":1,"scene_id":"main","name":"Retry check","nodes":[{"id":"recover","name":"Recover me","kind":"model","parent":null,"asset":"verification/recover.obj","transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}}]}
```

Open that project and confirm the node remains with a missing-asset error. Create a separate temporary asset directory containing `verification\recover.obj` with invalid OBJ text, mount it at `/` with priority `10`, and click **Retry failed assets**; the node should remain failed and report the decode problem. Replace that file with a copy of `engine\content\models\cube.obj`, unmount and remount the directory (directory mounts use an indexed snapshot), then retry again. The model should load while the scene file still references `verification/recover.obj`. Keep the project and fixture until the test receipt is saved.

## Record the result

Record the tested source SHA, Windows edition/build, Visual Studio and CMake versions, configure/build/CTest exit status, CTest log, which UI rows passed or failed, and the temporary project path. Capture screenshots of the first-run dock layout, reopened saved layout, hierarchy/Inspector, placement preview and any failure/recovery status. Keep logs and screenshots free of account names, private game data and secrets. Do not label skipped or unrun rows as passing.

## Separate owner-run game acceptance

This editor hosts mock game services; none of the checks above enter Star Citizen or establish behavior in a live game. Live acceptance is a separate owner-run gate: first record the exact game build, approved native provider/binary SHA and capability scope, then follow the applicable cases and lifecycle/refusal requirements in [Plugin integration](plugin-integration.md). Record startup, exercised capabilities, refusal behavior and shutdown/cleanup independently from editor results. No game test is authorized or implied by a Windows editor pass.
