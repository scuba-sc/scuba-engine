# Editor productization handoff

This handoff describes the current native editor scaffold and the next dependencies. It is a code and documentation status note, not a Windows or game acceptance receipt.

**Snapshot:** branch `codex/scene-document-v1`, based on reviewed engine main `7d9cf369cf718d6a108dd594288dc7d46267f166`. Record the current `git rev-parse HEAD` and worktree status before testing; this handoff is maintained with the editor implementation.

## Present capability and gaps

| Area | Implemented | Remaining or unverified |
| --- | --- | --- |
| Workspace and viewport | ImGui docking shell; first-run Scene/Mounts left, Inspector/Plugins right and Content/Asset loader/Log/Stats below the central 3D viewport. Existing `scuba-engine-layout.ini` is preserved. | The Win32/D3D12 editor has not been built or visually checked on Windows at this snapshot. |
| Scene editing | Version-1 scene documents, stable node IDs, nested groups, searchable outliner, Inspector TRS fields, selection, reparent/rename/delete/duplicate, project and scene create/open/copy, dirty-state prompts, bounded undo/redo, placement preview, selected model/group framing, and Move/Scale/Rotate gizmos with optional snapping. Project creation uses path entry, preserves existing targets, and avoids mounted asset sources. A scene copy includes current in-memory edits and leaves the active document and its dirty state unchanged. Editor saves compare against the loaded/saved baseline and refuse ordinary external-change conflicts; first saves do not replace a scene file created after opening. | Project dialogs use path entry rather than a native folder picker. The save conflict check is not a filesystem compare-and-swap, so a concurrent writer can still race between compare and atomic replacement. Nonuniform parent transforms can create shear; reparenting refuses cases the TRS document cannot represent. Windows UI behavior needs final verification. |
| Content browser | Read-only directory, ZIP and `.scpak` mounts; hierarchical folders inferred from file paths, search, list/tile modes, text type placeholders, and click/double-click model placement with Escape cancellation. Model candidates are `.gltf`, `.glb`, `.obj`, `.cgf`, `.cga` and `.fbx`; valid contents and dependencies still determine load success. | Tiles do not contain geometry thumbnails. Empty folders and game-specific object-container contents are not enumerated or parsed. Do not infer a format from the video. |
| Rendering and picking | Loaded model instances render in the central viewport; CPU picking and document-backed transforms use resolved world matrices. | D3D12 visuals, input feel, DPI behavior and the new default docking layout remain unverified on Windows. No thumbnail render path or bounded GPU thumbnail cache exists. |
| Game connection | The editor uses the repository's mock game services. | No live-game execution, native provider integration or gameplay acceptance is established. |

## Outliner search

The scene panel searches node names, stable IDs, and model asset paths with a bounded 256-byte UTF-8
query. ASCII letters match case-insensitively; non-ASCII bytes match exactly. Results retain each
matching node's ancestor groups so the hierarchy remains navigable. Search changes neither the
document nor selection and creates no undo entry. Invalid input leaves the full tree visible. Filter
expansion state is separate from the normal tree state, so clearing a search restores the user's
prior collapse choices.

More scene schema, storage, validation and failure behavior is in [Scene documents](scene-document.md). Model decoding details are in [Asset formats](../assets.md), and VFS mount/listing behavior is in [VFS](vfs.md).

## Recommended order

1. **Complete project entry acceptance.** Opening and creating projects use validated path entry, and the project scene browser opens saved non-`main` scenes after project selection. Keep creation separate from mounted source trees and preserve the CLI path. Validate the path/reparse behavior and dialogs on Windows before deciding whether a native folder picker is useful.
2. **Specify object-container support before implementing it.** Identify the exact container format(s), intended hierarchy/metadata, representative redistributable fixtures and error/size limits. Then select a bounded read-only VFS backend or model-decoder adapter. Preserve mount precedence, normalized paths, cancellation, and explicit unsupported/malformed results. Until this input exists, container support is blocked on format definition; no game-specific parser should be guessed.
3. **Design geometry thumbnails on the validated renderer.** Reuse decoded model data, but keep GPU creation and destruction on the render thread. Define a bounded cache, cancellation and invalidation on VFS mount changes, fallback placeholders, and fence-safe texture retirement before adding UI previews. Add synthetic fixtures and verify resource lifetime/memory behavior; the current text tiles must remain an honest fallback.
4. **Run broad acceptance last.** After the remaining scaffold work is complete, use the [Windows editor verification runbook](windows-editor-verification.md) for the full Windows build and interactive acceptance, including project path validation, save/discard/cancel, failed-open preservation, source-directory protection, scene round trip, outliner search/selection, browser, placement and gizmos. Any live-game validation remains a separate owner-run gate with an exact game build, approved provider binary/hash and capability scope; use [Plugin integration](plugin-integration.md) to keep mock, host and live-provider evidence distinct. Focused portable checks during implementation do not substitute for this final acceptance.

**Top next action:** identify the exact object-container formats the editor is expected to support and obtain representative, redistributable fixtures. Keep unsupported formats visible and read-only until their structure, metadata and size limits are known; do not infer a parser from the reference video. The Windows runbook remains the final acceptance stage after the scaffold is complete.
