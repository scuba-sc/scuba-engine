# Editor productization handoff

This handoff describes the current native editor scaffold and the next dependencies. It is a code and documentation status note, not a Windows or game acceptance receipt.

**Snapshot:** branch `codex/scene-document-v1`, docs commit `925d23d70bf4ab7099834534dfe21c58a12e2156`; the latest editor implementation commit is `fde7084ddaf67da5a76706696328cdb9c9c59c93`. Record and verify the actual SHA before testing if the branch advances.

## Present capability and gaps

| Area | Implemented | Remaining or unverified |
| --- | --- | --- |
| Workspace and viewport | ImGui docking shell; first-run Scene/Mounts left, Inspector/Plugins right and Content/Asset loader/Log/Stats below the central 3D viewport. Existing `scuba-engine-layout.ini` is preserved. | The Win32/D3D12 editor has not been built or visually checked on Windows at this snapshot. |
| Scene editing | Version-1 scene documents, stable node IDs, nested groups, outliner, Inspector TRS fields, selection, reparent/rename/delete, save/load/create scenes within a project, dirty-state prompts, bounded undo/redo, placement preview and Move/Scale/Rotate gizmos. | The project root is supplied as an existing directory with `--project`; there is no interactive project-root picker or project-folder creation workflow. Nonuniform parent transforms can create shear; reparenting refuses cases the TRS document cannot represent. UI behavior needs Windows verification. |
| Content browser | Read-only directory, ZIP and `.scpak` mounts; hierarchical folders inferred from file paths, search, list/tile modes, text type placeholders, and click/double-click model placement with Escape cancellation. Model candidates are `.gltf`, `.glb`, `.obj`, `.cgf`, `.cga` and `.fbx`; valid contents and dependencies still determine load success. | Tiles do not contain geometry thumbnails. Empty folders and game-specific object-container contents are not enumerated or parsed. Do not infer a format from the video. |
| Rendering and picking | Loaded model instances render in the central viewport; CPU picking and document-backed transforms use resolved world matrices. | D3D12 visuals, input feel, DPI behavior and the new default docking layout remain unverified on Windows. No thumbnail render path or bounded GPU thumbnail cache exists. |
| Game connection | The editor uses the repository's mock game services. | No live-game execution, native provider integration or gameplay acceptance is established. |

More scene schema, storage, validation and failure behavior is in [Scene documents](scene-document.md). Model decoding details are in [Asset formats](../assets.md), and VFS mount/listing behavior is in [VFS](vfs.md).

## Recommended order

1. **Verify the existing Windows vertical slice.** Follow the [Windows editor verification runbook](windows-editor-verification.md) on the recorded implementation SHA. Build the Windows x64 app and focused CTests, then visually check the initial/saved dock layout, scene round trip, prompts, browser, placement and gizmos. Record exact build and UI evidence; do not treat Linux headless tests as Windows integration.
2. **Finish project entry.** After that run, define the smallest root-selection workflow: choose an existing project or create a new directory, validate separation from mounted read-only asset roots, then open/create a scene. Keep the current CLI path and scene-level browser working. Add project-level UI only after observing how the existing workflow behaves on Windows.
3. **Specify object-container support before implementing it.** Identify the exact container format(s), intended hierarchy/metadata, representative redistributable fixtures and error/size limits. Then select a bounded read-only VFS backend or model-decoder adapter. Preserve mount precedence, normalized paths, cancellation, and explicit unsupported/malformed results. Until this input exists, container support is blocked on format definition; no game-specific parser should be guessed.
4. **Design geometry thumbnails on the validated renderer.** Reuse decoded model data, but keep GPU creation and destruction on the render thread. Define a bounded cache, cancellation and invalidation on VFS mount changes, fallback placeholders, and fence-safe texture retirement before adding UI previews. Add synthetic fixtures and verify resource lifetime/memory behavior; the current text tiles must remain an honest fallback.
5. **Run broad acceptance last.** After the scaffold and focused regression checks are complete, run the full Windows suite and interactive acceptance. Any live-game validation remains a separate owner-run gate with an exact game build, approved provider binary/hash and capability scope; use [Plugin integration](plugin-integration.md) to keep mock, host and live-provider evidence distinct.

**Top next action:** run the existing Windows build and interactive checklist against `fde7084ddaf67da5a76706696328cdb9c9c59c93` and record its failures before adding project-root UI, container parsing or thumbnail GPU resources.
