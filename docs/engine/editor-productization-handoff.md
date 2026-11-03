# Editor productization handoff

This handoff describes the current native editor scaffold and the next dependencies. It is a code and documentation status note, not a Windows or game acceptance receipt.

**Snapshot:** branch `codex/scene-document-v1`; the current project path-entry implementation is source commit `84e09798aa992073a7e7515f769723e377c01cfc`. Documentation may advance independently, so record the current `git rev-parse HEAD` and worktree status before testing.

## Present capability and gaps

| Area | Implemented | Remaining or unverified |
| --- | --- | --- |
| Workspace and viewport | ImGui docking shell; first-run Scene/Mounts left, Inspector/Plugins right and Content/Asset loader/Log/Stats below the central 3D viewport. Existing `scuba-engine-layout.ini` is preserved. | The Win32/D3D12 editor has not been built or visually checked on Windows at this snapshot. |
| Scene editing | Version-1 scene documents, stable node IDs, nested groups, outliner, Inspector TRS fields, selection, reparent/rename/delete, save/load/create scenes within a project, dirty-state prompts, bounded undo/redo, placement preview and Move/Scale/Rotate gizmos. **File > Open project...** accepts a path to an existing project directory and opens its `main` scene without requiring a scene ID. Project paths are canonicalized and checked against mounted read-only asset directories and archives. | This is path entry, not a native folder picker; project-directory creation and choosing an existing non-`main` scene during initial open are not implemented. Invalid opens must leave the current document intact. Nonuniform parent transforms can create shear; reparenting refuses cases the TRS document cannot represent. UI behavior needs final Windows verification. |
| Content browser | Read-only directory, ZIP and `.scpak` mounts; hierarchical folders inferred from file paths, search, list/tile modes, text type placeholders, and click/double-click model placement with Escape cancellation. Model candidates are `.gltf`, `.glb`, `.obj`, `.cgf`, `.cga` and `.fbx`; valid contents and dependencies still determine load success. | Tiles do not contain geometry thumbnails. Empty folders and game-specific object-container contents are not enumerated or parsed. Do not infer a format from the video. |
| Rendering and picking | Loaded model instances render in the central viewport; CPU picking and document-backed transforms use resolved world matrices. | D3D12 visuals, input feel, DPI behavior and the new default docking layout remain unverified on Windows. No thumbnail render path or bounded GPU thumbnail cache exists. |
| Game connection | The editor uses the repository's mock game services. | No live-game execution, native provider integration or gameplay acceptance is established. |

## Incremental outliner handoff

This snapshot reconstructs the saved editor patch on its exact base and adds only the bounded
scene-outliner search slice. Later offline work from a separate workspace is not present. Project
creation, node duplication, and transform snapping still need to be integrated and reviewed
separately before treating the editor scaffold as complete.

More scene schema, storage, validation and failure behavior is in [Scene documents](scene-document.md). Model decoding details are in [Asset formats](../assets.md), and VFS mount/listing behavior is in [VFS](vfs.md).

## Recommended order

1. **Complete project entry.** The existing-folder path-entry flow is scaffolded. The remaining project-level gap is explicit new-project creation or choosing a non-`main` scene at first open. Keep creation separate from mounted source trees and preserve the CLI path and scene-level browser; add a native folder picker only if it improves on validated path entry.
2. **Specify object-container support before implementing it.** Identify the exact container format(s), intended hierarchy/metadata, representative redistributable fixtures and error/size limits. Then select a bounded read-only VFS backend or model-decoder adapter. Preserve mount precedence, normalized paths, cancellation, and explicit unsupported/malformed results. Until this input exists, container support is blocked on format definition; no game-specific parser should be guessed.
3. **Design geometry thumbnails on the validated renderer.** Reuse decoded model data, but keep GPU creation and destruction on the render thread. Define a bounded cache, cancellation and invalidation on VFS mount changes, fallback placeholders, and fence-safe texture retirement before adding UI previews. Add synthetic fixtures and verify resource lifetime/memory behavior; the current text tiles must remain an honest fallback.
4. **Run broad acceptance last.** After the remaining scaffold work is complete, use the [Windows editor verification runbook](windows-editor-verification.md) for the full Windows build and interactive acceptance, including project path validation, save/discard/cancel, failed-open preservation, source-directory protection, scene round trip, browser, placement and gizmos. Any live-game validation remains a separate owner-run gate with an exact game build, approved provider binary/hash and capability scope; use [Plugin integration](plugin-integration.md) to keep mock, host and live-provider evidence distinct. Focused portable checks during implementation do not substitute for this final acceptance.

**Top next action:** identify the exact object-container formats the editor is expected to support and obtain representative, redistributable fixtures. Keep unsupported formats visible and read-only until their structure, metadata and size limits are known; do not infer a parser from the reference video. The Windows runbook remains the final acceptance stage after the scaffold is complete.
