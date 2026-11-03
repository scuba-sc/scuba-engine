# Scene documents

Scene documents are editor data. They do not represent the live game world and do not call game
services. `ResolveWorldTransforms` composes each node's local translation, quaternion rotation and
non-uniform scale into a row-vector local-to-world matrix. Nested rotation under non-uniform parent
scale can introduce shear, so the resolved matrix is preserved instead of decomposed back into TRS.
The renderer and CPU picker accept those affine matrices; picking uses an affine inverse, bounds
transform all eight corners, and the D3D12 instance path uses an inverse-transpose normal matrix.
Windows D3D12 compilation and visual validation are still pending.
The Windows build and interactive checklist is in [Windows editor verification](windows-editor-verification.md).

## Version 1

`scuba::scene::Document` is serialized as UTF-8 JSON with these top-level fields:

| Field | Meaning |
| --- | --- |
| `format` | `scuba.scene` |
| `schema_version` | `1` |
| `scene_id` | Stable lowercase identifier used for the project filename |
| `name` | Display name |
| `nodes` | Flat list of nodes linked by persistent IDs |

Each node has an `id`, `name`, `kind` (`group` or `model`), optional `parent`, optional `asset`, and
`transform` with three-element translation and scale arrays plus a four-element quaternion array.
Model assets use normalized relative paths in the engine VFS namespace. Groups can contain groups
or model instances; model instances cannot contain children. IDs are lowercase ASCII strings and
are not array positions, so reordering nodes does not change identity. The restricted alphabet
avoids path separators, case-collision behavior, and reserved Windows device filenames.

Names and paths must be valid UTF-8. Validation rejects duplicate IDs, missing or non-group parents,
hierarchy cycles, nesting deeper than 256, invalid transforms, path traversal in asset references,
more than 8,192 nodes, and files larger than 8 MiB. Rotations must be normalized quaternions; scale
components must be finite and positive. Unknown fields and schema versions are rejected so a future
document cannot be silently rewritten while losing fields this version does not understand.

## Project storage and editor session

`ProjectStore` opens an existing project directory selected by its caller. The editor checks that
directory against all currently mounted read-only sources, including archive files, both before
opening and again before each scene-store operation. `ProjectStore::Open` also records mounted
directory roots and rechecks them on each operation. Paths are canonicalized before component
comparison, so existing directory symlink aliases are resolved. On Windows, case-folded components
count as the same path only when the filesystem reports those prefixes as the same object; this
accounts for case-insensitive volumes while allowing distinct names in a case-sensitive directory.
These checks catch a mount path that has been retargeted since project selection. They are path
checks, not a filesystem lock: behavior under a concurrent external reparse-point swap between
validation and I/O is not established.

The store writes `<scene-id>.scene.json` directly inside the project directory. Scene IDs are
validated before use as filenames; callers cannot supply a path. Saves write a unique sibling
temporary file and atomically replace the destination. Loads reject symlinked scene files, enforce
the file-size cap, and leave the caller's output document unchanged on failure. The editor loads
into a candidate document and only installs it after the complete scene is valid, so a malformed
existing scene does not replace the active document. `ListScenes` returns sorted IDs for direct-child
regular scene files and skips symlinks. `Create` writes atomically without replacing an existing
scene file.

The project writer is separate from the VFS. Asset directories, archives, and `.scpak` mounts stay
read-only inputs. The native editor can open one scene by stable ID with
`scuba-engine.exe --project <existing-project-directory> --scene <scene-id>`; `--scene` defaults to
`main`. The File menu also offers **Open project...**, where the editor accepts a path to an existing
project directory and opens its `main` scene, or starts a new unsaved `main` scene if that file is
absent. **Create project...** accepts a new folder path whose parent directory already exists. It
creates an empty, saved `main.scene.json`; it does not create parent folders, replace an existing
file or folder, or copy or modify mounted assets. Creation builds the scene in a uniquely named
sibling staging directory and publishes that directory with an atomic no-replace operation, so a
failed initialization does not expose a partial project. The editor checks mounted source overlap
before asking about unsaved edits and repeats the path/source checks immediately before publishing.
The exclusive directory publish is implemented for Windows and Linux; other platforms fail closed.
Both project dialogs use path entry rather than a native folder picker. The File menu and Scene panel
also open a project-scene browser where saved scenes can be listed, opened, and created without
restarting the editor or supplying a scene ID on the command line. Switching away
from unsaved edits asks whether to save, discard the in-memory changes, or cancel; a failed save
keeps the switch pending, and a failed project open leaves the current session in place. Creating a
scene refuses to replace an existing file. If the default scene file does not exist, the editor
starts an empty, unsaved scene. The app tracks mounted directory and archive sources. It rejects a
project root that overlaps them before opening or mounting a source, and rechecks the current source
paths for scene listing, opening, creating, and saving. A failed recheck/load leaves the current
scene and its dirty state untouched; when a requested switch had a pending unsaved-changes prompt, a
failed target open does not install the target document. Directory symlink aliases and archive
files inside a project root are covered by portable tests. Junction behavior, case-only path
spellings, and Windows reparse-point retargeting still require the Windows checklist; Linux tests do
not establish those Windows semantics. Project creation's final no-replace publish prevents an
existing target from being overwritten, but path checks are not a filesystem lock: a concurrent
reparse-point or mount change between the final check and publish is not eliminated. Windows
junction and reparse-point behavior remains pending native validation.

The scene browser can **Save a copy of the current scene** under a new scene ID. It copies the
current in-memory document, so edits made since the last save are included in the new file. Only the
document's scene ID changes; node IDs, hierarchy, transforms, asset references, and display name are
preserved. The copy uses the project store's atomic create-without-replace path and rechecks mounted
source separation. If the ID is invalid, already exists, the source root changed, or persistence
fails, the existing scene file and active document remain unchanged. A successful copy does not
switch scenes, mark the active scene saved, change node selection/undo history, or clear its dirty
state; the new scene appears in the browser and can be opened explicitly. Windows UI behavior remains
pending.

The Scene panel is a document-backed outliner. Its bounded search accepts up to 256 UTF-8 bytes and
matches names, stable IDs, and model asset paths. It shows each match with its ancestor groups,
expands the context needed to reach matches, and leaves selection and scene data untouched. ASCII
letters match case-insensitively; non-ASCII UTF-8 bytes match exactly. Invalid UTF-8, control
characters, and oversized queries are rejected and the full tree remains visible. Stable node IDs
are used for selection and the Inspector edits local translation, local XYZ Euler rotation in degrees, and nonuniform scale. Euler
angles are converted to normalized quaternions for the version 1 document format. Uniform scale
step buttons multiply all local axes together and preserve their ratios. These edits participate in
the same bounded undo history as other scene changes. The viewport shows screen-space hit handles
for the selected node's world X, Y, and Z translation axes. Dragging an arrow moves the node only
along that world axis; the result is transformed through the parent inverse before writing local
translation, and the drag is one undo action. Ctrl+left-drag retains its ground-plane move behavior.
The Scene panel switches the viewport gizmo between Move, Scale, and Rotate. Scale mode changes one
positive local scale component per drag; handles follow the node's local rotation and full parent transform.
Under a nonuniform parent those handles may not be perpendicular. Values outside the scene's
positive scale limits are refused, and mirrored scale is not representable by the scene schema.
Rotate mode drags a local-axis ring; the pointer ray is intersected with the affine-transformed ring
plane, so nonuniform parents can make the rings elliptical. The gesture accumulates wrapped angle
steps and records one undo action. If the ring plane is edge-on or ill-conditioned from the current
view, the drag is refused. Rotation changes local TRS; it does not promise rotation around a world
axis or a circular ring under a nonuniform parent.
Optional editor-session snapping controls are off by default and are captured when a drag starts.
They are not stored in scene documents. Move snapping
quantizes the world-axis displacement from the drag start in world units (step 0.001 to 100,000).
Rotate snapping quantizes the accumulated local-axis angle from the drag start in degrees (step 0.1
to 180). Scale snapping quantizes an additive local-scale delta from the drag start (step 0.001 to
1,000). A snapped scale that reaches zero or becomes negative is refused by the positive-scale
validation; the last valid drag value remains in the document. Snapping does not alter the default
continuous drag behavior, and each drag remains one undo action with the existing Escape rollback.
The mode controls are disabled during a gesture. If the selected node, gizmo mode, asset-placement
state, or scene history changes externally, the next pointer move stops the gesture; any accepted
partial edit remains one undoable action. Escape rolls back the active gizmo gesture if no separate
scene edit has superseded it. If another edit intervenes, rollback is refused and the current
transform is kept. If pointer capture is interrupted, the last valid transform is kept as one
undoable edit. Handles that project
end-on are not pickable; the tool refuses near-parallel ray/axis movement instead of applying an
unstable translation. An active asset-placement preview hides the transform gizmo and owns viewport
clicks. The native Content and viewport UI remain Windows-only and still need a Windows compile and
visual validation.
Groups can be added and nested. Selected nodes can be renamed, deleted (groups remove their
subtrees), duplicated (including descendants), or reparented from the selected-node controls.
**Duplicate subtree** and `Ctrl+D` create fresh stable IDs for every copied node, remap the copied
hierarchy, retain model asset references and descendant-local transforms, and move the copied root
one local X unit. The copied root gets a ` Copy` name suffix when it fits the name limit; longer
valid names are preserved unchanged. The edit is transactional: an invalid ID, invalid resulting
transform, or scene node-limit failure leaves the document unchanged. Reparenting keeps the selected node's world
transform and rejects cycles or moves that would need local shear, which this TRS schema cannot
represent. The Content panel browses the mounted VFS hierarchically. Because VFS listings
return files rather than directory records, folder rows are inferred from normalized file paths; the
panel does not invent empty folders or parse game-specific object-container formats. Search is
ASCII-case-insensitive and recursive within the current folder; it filters immediate folder names
and file paths, then shows only matching files. The model
decoder recognizes `.gltf`, `.glb`, `.obj`, `.cgf`, `.cga`, and `.fbx` as model-format candidates;
other file formats remain visible but are not offered for placement. A recognized extension does not
guarantee valid model contents or resolved dependencies: the model loader reports decode and
dependency failures when a file is opened. Files absent from the mounted VFS are not listed; existing scene
references to missing files remain in the outliner and show their load failure. The browser treats
mounted assets as read-only. Mount directories, ZIP archives, and `.scpak` archives through the
existing Mounts panel; the browser navigates the paths those VFS backends expose. Directory listings
reflect the VFS mount's indexed snapshot, so changes made on disk appear after unmounting and
remounting that directory. The panel keeps its double-click
quick placement and “Place with click” mode: it shows a cyan-tinted viewport preview, click places
the model, and Escape cancels. If a group is selected when placement starts, the new model is parented
to that group. Mount changes clear the selected asset and cancel any active placement preview, so a
stale VFS path cannot silently refer to a different mounted winner. The browser validates the VFS's
canonical sorted unique path list and shows the resolved mount source in the file tooltip. Same-path
overlays use the VFS winner. Material edits are currently session-only because materials are not
in scene schema version 1. The browser refuses catalogs larger than 65,536 entries and shows at most 4,096 folders
and 4,096 files per view, reporting omitted rows so users can narrow the search or enter a folder.
It reads through the VFS `ListPage` API, which treats its argument as a directory path (the empty
path is the VFS root), merges sorted backend pages, and returns at most 4,096 unique paths per call.
This is component-scoped directory enumeration; legacy `List` keeps its raw-prefix behavior. The page
cursor uses the full normalized VFS path, and the mount-stack snapshot ID makes a mount change fail
explicitly instead of mixing pages from different overlays. Built-in
directory, ZIP, `.scpak`, and memory backends support pages. A custom backend that only implements
the legacy `FileSystem::List` API produces an explicit browser listing error; the VFS does not fall
back to materializing its full listing. The legacy `List` API remains available and retains its
previous behavior for existing callers.

The Content panel can switch between list and tile views. Tiles use text type placeholders (`FOLDER`,
`3D MODEL`, and `UNSUPPORTED`); the editor has no asset-thumbnail source and generates or loads no
image previews. Tile and list rows are clipped to the visible range, while the browser model still
limits each view to 4,096 folders and 4,096 files. Search, folder navigation, selection, unsupported
file visibility, and supported-model placement are shared by both views.

Page delivery is bounded, but backend indexing is still eager. Each built-in backend retains its
full file index and a sorted vector of path references for paging, so that auxiliary vector also
grows with the entire mounted catalogue. Directory mounts scan and index up to 1,048,576 files by
default when mounted. ZIP mounts read the complete central directory at open: its size is a 32-bit
field (up to 4 GiB minus one byte, subject to the archive bounds and available memory), with at most
65,534 entries because Zip64 is unsupported. `.scpak` mounts read their complete index at open, capped
at 64 MiB and 1,048,576 entries. Large archives can therefore still require substantial time and
memory to mount. The browser also keeps its bounded catalogue of up to 65,536 entries after paging;
the improvement is that it no longer first materializes an unbounded merged `Entry` vector. The
Windows-only ImGui browser and D3D12 viewport have not been compiled or visually validated here;
portable tests cover the browser model, not UI behavior.

Saving is explicit through the Scene panel or File menu (`Ctrl+S`). The Scene panel reports saved,
unsaved, and save-error states; there is no automatic save. Switching scenes or quitting with dirty
changes asks to save, discard, or cancel; a window-close request is deferred until that choice is
made. Undo and redo are available from the File menu, Scene panel, and `Ctrl+Z` / `Ctrl+Y`. They
retain up to 64 snapshots or 32 MiB, whichever limit is reached first. A continuous Inspector edit,
Ctrl+left-drag, or transform-gizmo drag is one history action; uniform-scale steps, group, duplicate,
place, delete, rename, reparent, clear, and double-click spawn each form one action. Asynchronous
grounding is folded into its spawn action when no later scene edit intervenes. Returning to the last saved
document clears the unsaved state; saving establishes a new baseline without discarding undo history.
Without `--project`, the editor still uses the same document and outliner in memory and says that
the session will not persist.

`ResolveWorldTransforms` first validates the document, then emits resolved matrices in the original
node order regardless of parent ordering in the serialized array. It leaves its output unchanged
if composition overflows or a world transform is singular or numerically unstable. On open, the
editor installs the validated document, rebuilds model instances by stable node ID, and requests
each asset path from the current VFS mounts. Ready models are uploaded and drawn with their resolved
world matrices; the same instances feed viewport picking. The outliner and Inspector remain backed
by the document even when a model cannot be loaded. A missing, unsupported, or malformed model file
leaves its saved node in place, marks the outliner row as failed, and reports the loader error in
the outliner and Asset Loader tooltips and the log. The document is not rewritten to remove the
reference. Invalid asset-path syntax (including traversal or a non-normalized VFS path) rejects the
scene load; browser opens show the error and leave the current document untouched, while startup
reports the error and leaves the saved file untouched. Failed scene model loads are not retried
automatically. After correcting a file or mounting/remounting its directory or archive, use **Retry
failed assets** in the Scene panel. Retries use the loader's normalized-path cache, so objects that
share one failed asset share one new request, while already loaded models are left alone. Failed
nodes and their saved references remain in the document if the retry also fails; the row and loader
diagnostics then show the new result. Replacing files inside a ZIP or `.scpak` requires unmounting
and remounting the archive before retrying, because archive backends use their opened index.
Selection is editor-session state: opening a scene clears the previous selection; nodes can be
selected from the outliner, and loaded models can also be selected in the viewport. The project
scene browser is available from the File menu and Scene panel. Windows D3D12 compilation and visual
validation remain pending; the focused Linux core tests do not establish Windows rendering behavior.
