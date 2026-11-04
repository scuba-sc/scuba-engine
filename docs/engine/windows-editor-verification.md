# Windows editor verification

This checklist covers the native scene editor on Windows. It complements [Scene documents](scene-document.md), which defines the file format, editing behavior and limits.

**Code target recorded for this checklist:** branch `codex/scene-document-v1`. Record `git rev-parse HEAD` immediately before testing and use that exact SHA in the results; do not treat an earlier snapshot as evidence for a later build.

**Portable check recorded for source SHA `417d19dab4cd6b32b6405f58e4248b7fcd70bcee`:** Linux configured against the pinned SDK 2.4.0 source, built `test_engine_core` and `test_scene_outliner_search`, and passed CTest `engine_core` plus `scene_outliner_search` (2/2). This covers portable scene, project, snapping and outliner-search logic; it does not compile the Win32/D3D12 app. The Windows editor build, interactive UI checks and game acceptance remain deferred until the scaffold is complete. A headless test does not establish Windows rendering or input behavior.

## Prerequisites and build

- Windows x64, Visual Studio 2022 with **Desktop development with C++** and a Windows 10/11 SDK.
- CMake 3.21 or newer and Python 3 available on `PATH`.
- Network access for the first configure, or local copies of the exact pinned SDK and third-party archives. CMake verifies their SHA-256 before use; the default profile is stable sco-core SDK 2.4.0.

From the repository root, in PowerShell:

```powershell
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 `
  -DSCUBA_BUILD_ENGINE=ON -DSCUBA_BUILD_TESTS=ON -DSCUBA_WERROR=ON
cmake --build build-win --config Release --target scuba-engine test_workspace_layout test_engine_core test_scene_outliner_search test_asset_browser
ctest --test-dir build-win -C Release -R "^(workspace_layout|engine_core|scene_outliner_search|asset_browser_model)$" --output-on-failure
```

For offline configure, pass `-DSCUBA_SDK_ARCHIVE_DIR=<directory containing sco-sdk-host-source-sc-2.4.0.zip>` and `-DSCUBA_DEPS_ARCHIVE_DIR=<directory containing the pinned dependency archives>`. Keep the archive names and hashes required by `cmake/ScubaSdk.cmake` and `cmake/ScubaDeps.cmake`; do not substitute an RC or unpublished kit. A successful CTest run covers portable logic, not D3D12 visuals.

## Interactive editor checks

Use a new temporary project and a separate temporary working directory. The scene project stores files in the project directory; ImGui stores `scuba-engine-layout.ini` relative to the process working directory. Keeping both under a unique temporary parent avoids changing repository content or an existing workspace.

```powershell
$case = Join-Path $env:TEMP ("scuba-editor-check-" + [guid]::NewGuid().ToString("N"))
$project = Join-Path $case "project"
$workspace = Join-Path $case "workspace"
New-Item -ItemType Directory -Path $case | Out-Null
New-Item -ItemType Directory -Path $workspace | Out-Null
$editor = Get-ChildItem .\build-win -Recurse -File -Filter scuba-engine.exe |
  Select-Object -First 1 -ExpandProperty FullName
if (-not $editor) { throw "scuba-engine.exe was not found under build-win" }
Push-Location $workspace
try { & $editor } finally { Pop-Location }
```

Check each item and record **Pass**, **Fail**, or **Blocked**, plus the evidence noted at the end.

| Check | Expected result |
| --- | --- |
| Project creation, path entry and existing-scene errors | From the in-memory scene, use File > Create project... and enter the new `$project` path. The editor creates an empty saved `main.scene.json` without replacing existing files/folders or writing under mounted asset sources. A missing parent, existing target, regular-file target, or overlapping mounted root reports an error and preserves existing bytes. Then use File > Open project... on a valid project; `main` opens, or a new unsaved `main` scene is shown if absent. A malformed scene leaves the active document and save state intact. |
| First-run workspace | Scene is docked left, Inspector right, Content bottom, and the 3D viewport occupies the center. Scene, Inspector and Content are the selected tabs. Mounts, Plugins, Asset loader, Log and Stats share those docks. |
| Saved workspace | Move a panel, leave the editor open long enough for ImGui’s settings save, close it, and relaunch from the same working directory. The moved layout is restored rather than reset to defaults. |
| Project scene round trip | Start with the empty temporary project. Add a group, place `models/cube.obj` under it from Content, save with Ctrl+S, close cleanly, and reopen the same project. The group, model reference and transforms remain. Inspect `<project>\main.scene.json` as supporting evidence. |
| Scene display name and stable ID | Change the Scene panel display name, apply it, and verify one Undo restores the prior name while Redo reapplies it. Save and reopen; the display name persists while the scene ID and `<scene-id>.scene.json` filename remain unchanged. With a name draft pending, verify Save applies and saves it, Discard drops it, and Cancel keeps the draft editable. Empty/control-character and over-128-byte names must be refused without changing the document; confirm an over-limit paste is not silently truncated into a valid name. |
| Save a copy with unsaved edits | Open a saved scene, make a visible edit without saving, and use Project scenes > Save a copy of the current scene with a new ID. The new scene file contains the in-memory edit and the same node IDs, hierarchy, transforms and asset paths. The original stays active and still reports unsaved. Open the new scene explicitly to inspect it. Try the same destination ID again; the editor reports a conflict and the copied file's checksum remains unchanged. |
| External scene conflict | Open a saved scene, make a local edit, then change and save the same scene file from another process. Press Ctrl+S. The editor must refuse the overwrite, report that the scene changed on disk, keep the local edit dirty, and leave the external file bytes unchanged. Repeat after opening a blank scene and externally creating its scene file; the first save must refuse to replace it. |
| Unsaved guard | Make an edit and close the window. Cancel keeps the editor and edit open; Save persists it; on a separate edit, Discard exits without changing the saved scene. Repeat through File > Open project... and the scene browser: Cancel keeps the current project, Save persists the current scene before switching, and Discard switches without saving. |
| Hierarchy and Inspector | Add/nest groups, rename a node, reparent while preserving its world transform, and delete a test subtree. Confirm rejected cycle or unrepresentable reparent leaves the document unchanged. Edit translation, rotation and positive nonuniform scale in the Inspector. Reset local transform on a nested node; verify the parent is unchanged, the node's world placement follows the reset local TRS, one undo step restores it, and dirty state updates. |
| Frame selection | Select a loaded model and press F; verify its transformed bounds are centered in both landscape and narrow/tall viewport layouts. Select a group with loaded models, nested groups, and one missing asset; verify the loaded descendants are framed and the skipped asset count is reported. Try a group with only missing/loading assets and an empty group; verify clear status without camera jumps. A point/flat model must use the minimum distance; a distant tiny object outside camera float precision must be refused without moving the camera. Confirm focus does not dirty the scene or change undo/selection. |
| Outliner search, hierarchy and selection | Create nested groups and model nodes. Search by node name, stable ID and asset path, using mixed ASCII case. Matching descendants remain visible with their ancestor groups; a selected node outside the results remains selected and offers Clear search. Clear the query and confirm the normal tree's prior collapsed groups are restored. Search must not change scene data, dirty state or undo history. |
| Viewport tools, snapping and history | Select the model. Confirm all snap toggles are off by default. Try Move, Scale and Rotate with snapping off and with bounded steps enabled, then Undo and Redo each drag once. Escape cancels a valid active drag. Scale values reaching zero or below are refused without losing the last valid value. A near-edge-on rotation ring may be refused with a status message; positive scale limits and unsupported mirrored scale must be enforced. Snap controls are editor-session settings and reset on restart. |
| Viewport subtree isolation | With nested groups and models, isolate a group and verify only that group and descendants render and can be picked. Confirm gizmos and framing stay within the isolated subtree. Select a node outside the subtree in the outliner; the filter clears and reveals it. Clear isolation explicitly, verify project/scene switches clear it, and check placement under a visible group versus root placement. Neither isolation action changes the scene file, dirty state or undo history. |
| Browser and placement | Browse `models`, search for `cube`, switch Tiles/List, and confirm `README.md` remains visible but unsupported. Tiles show text type placeholders, not image thumbnails. Check double-click placement, Place with click preview, Escape cancellation and viewport selection. |
| Failure and recovery | Use the isolated missing-asset fixture below. The failed node remains in the outliner with a diagnostic; after repairing and remounting the asset source, Retry failed assets loads it without replacing the scene node. |
| Replace a model reference | Select a loaded model or the missing-asset node below. Choose a different supported model in Content, click Replace selected model..., and inspect the candidate in Inspector. The original reference stays active during loading and after a failed load or Cancel. Apply only after decoding succeeds; verify the node ID, name, hierarchy and TRS are unchanged, one Undo restores the old reference, one Redo restores the replacement, and save/reopen preserves the new path. Repeat with a nested nonuniformly scaled parent. |
| Stale or invalid replacement | Enter a missing, unsupported, malformed, overlong, absolute, or traversal path. The scene/history/dirty state must be preserved on refusal. Stage a valid candidate, then edit the scene, change selection, undo, open another scene/project, or change mounts; the candidate must be discarded. Active placement/gizmo gestures block application. Replacing a node whose old spawn is still loading must retain its authored transform when either load completes. |
| Source-root aliases and case | In the temporary parent, create a junction alias to a test project (`New-Item -ItemType Junction -Path (Join-Path $case 'project-alias') -Target $project`). Mount that alias as a source, then attempt to open `$project`; the editor must report overlap and create no scene file. On a normal case-insensitive volume, repeat by mounting a case-varied spelling of `$project` and opening its original spelling. If the parent permits distinct case-sensitive siblings, verify those distinct roots are allowed and record the filesystem behavior. |
| Mount retarget while a switch is pending | Open a project and mount a separate temporary asset directory. Edit the current scene, request File > Open project... for another valid project and leave the Save/Discard/Cancel prompt open. From a second PowerShell, rename the mounted source directory to a sibling backup and create a junction at its original path targeting the selected target project. Choose Discard. The second open-time check must reject the overlap; the original document remains installed and dirty. Remove the test junction and restore the directory after recording results. Separately, retarget a mounted path to the active project's root; Refresh in the scene browser must show the source-overlap error and clear the stale list, then Ctrl+S must fail with the existing scene-file checksum unchanged and the scene still dirty. |
| Source-directory and archive protection | Record a checksum for a mounted asset file before and after project open/save attempts. The editor refuses a project root that is equal to, inside, or contains a mounted directory or archive, and the source file remains byte-identical. Also verify a mounted archive file stored under the selected project is rejected before creating or replacing any scene. |

### Missing-asset and retry fixture

In a second temporary project, create a valid scene file referencing an asset path that does not yet exist. This one-node schema-v1 fixture can be written while the editor is closed as `<project>\main.scene.json`:

```json
{"format":"scuba.scene","schema_version":1,"scene_id":"main","name":"Retry check","nodes":[{"id":"recover","name":"Recover me","kind":"model","parent":null,"asset":"verification/recover.obj","transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}}]}
```

Open that project and confirm the node remains with a missing-asset error. Create a separate temporary asset directory containing `verification\recover.obj` with invalid OBJ text, mount it at `/` with priority `10`, and click **Retry failed assets**; the node should remain failed and report the decode problem. Replace that file with a copy of `engine\content\models\cube.obj`, unmount and remount the directory (directory mounts use an indexed snapshot), then retry again. The model should load while the scene file still references `verification/recover.obj`. Keep the project and fixture until the test receipt is saved.

### Deferred reference-replacement coverage

The 2026-11-04 model-reference replacement implementation has not been built or executed. At the
final testing stage, add portable regression cases for `ReplaceModelAsset`: missing node, group
node, stale expected path, invalid/overlong/UTF-8/traversal replacement, serialized size-limit
overflow, no-op, preservation of every other field under nested transforms, and undo/redo plus
dirty/save round trips. Refusal must preserve
the complete original document and history.

On Windows, exercise the two replacement rows above and both asynchronous completion orders: a
candidate becoming ready before the loader pump and between the pump and Apply. Verify one GPU
upload per handle, unchanged authored transform, correct retained selection, and visible diagnostics
on decode failure. Canceling a staged candidate shared by another instance must not cancel that
instance's load. Ready-model reload after source edits/remounts is not implemented: document the
session-cache behavior and restart before asserting changed source bytes were loaded. GPU residency,
resource lifetime, and visual behavior still require their own observations.

## Record the result

Record the tested source SHA, Windows edition/build, Visual Studio and CMake versions, configure/build/CTest exit status, CTest log, which UI rows passed or failed, and the temporary project path. Capture screenshots of the first-run dock layout, reopened saved layout, hierarchy/Inspector, placement preview and any failure/recovery status. Keep logs and screenshots free of account names, private game data and secrets. Do not label skipped or unrun rows as passing.

## Separate owner-run game acceptance

This editor hosts mock game services; none of the checks above enter Star Citizen or establish behavior in a live game. Live acceptance is a separate owner-run gate: first record the exact game build, approved native provider/binary SHA and capability scope, then follow the applicable cases and lifecycle/refusal requirements in [Plugin integration](plugin-integration.md). Record startup, exercised capabilities, refusal behavior and shutdown/cleanup independently from editor results. No game test is authorized or implied by a Windows editor pass.
