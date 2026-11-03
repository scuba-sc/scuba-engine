# Scene documents

Scene documents are editor data. They do not represent the live game world and do not call game
services. `ResolveWorldTransforms` composes each node's local translation, quaternion rotation and
non-uniform scale into a row-vector local-to-world matrix. Nested rotation under non-uniform parent
scale can introduce shear, so the resolved matrix is preserved instead of decomposed back into TRS.
The renderer and CPU picker accept those affine matrices; picking uses an affine inverse, bounds
transform all eight corners, and the D3D12 instance path uses an inverse-transpose normal matrix.
Windows D3D12 compilation and visual validation are still pending.

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

`ProjectStore` opens an existing project directory selected by its caller and receives the complete
set of mounted read-only asset directories. It rejects project roots that equal, contain, or are
contained by any of those asset directories, and rechecks that separation before reads and writes.
It stores a document as `<scene-id>.scene.json` directly inside the project directory. The scene ID
is validated before it is used as a filename; callers cannot supply a path. Saves write a unique
sibling temporary file and atomically replace the destination. Loads reject symlinked scene files,
enforce the file-size cap, and leave the caller's output document unchanged on failure. `ListScenes`
returns sorted IDs for direct-child regular scene files and skips symlinks. `Create` writes atomically
without replacing an existing scene file.

The project writer is separate from the VFS. Asset directories, archives, and `.scpak` mounts stay
read-only inputs. The native editor opens one scene by stable ID with
`scuba-engine.exe --project <existing-project-directory> --scene <scene-id>`; `--scene` defaults to
`main`. The File menu and Scene panel also open a project-scene browser where saved scenes can be
listed, opened, and created without restarting the editor or supplying a scene ID on the command
line. Switching away from unsaved edits asks whether to save, discard the in-memory changes, or
cancel. Creating a scene refuses to replace an existing file. If the default scene file does not
exist, the editor starts an empty, unsaved scene. It does not create the project directory. The app
checks directory mounts through `ProjectStore` and checks archive mount paths separately before
opening the session.

The Scene panel is a document-backed outliner. Stable node IDs are used for selection and the
Inspector edits local translation, local XYZ Euler rotation in degrees, and nonuniform scale. Euler
angles are converted to normalized quaternions for the version 1 document format. Uniform scale
step buttons multiply all local axes together and preserve their ratios. These edits participate in
the same bounded undo history as other scene changes. The viewport shows screen-space hit handles
for the selected node's world X, Y, and Z translation axes. Dragging an arrow moves the node only
along that world axis; the result is transformed through the parent inverse before writing local
translation, and the drag is one undo action. Ctrl+left-drag retains its ground-plane move behavior.
Viewport rotation and scale gizmos are not implemented yet.
Groups can be added and nested. Selected nodes can be renamed, deleted (groups remove their
subtrees), or reparented from the selected-node controls. Reparenting keeps the selected node's
world transform and rejects cycles or moves that would need local shear, which this TRS schema
cannot represent. The Content panel keeps its existing double-click quick placement and adds a
“Place with click” mode: it shows a cyan-tinted viewport preview, click places the model, and Escape
cancels. If a group is selected when placement starts, the new model is parented to that group.
Material edits are currently session-only because materials are not in scene schema version 1.

Saving is explicit through the Scene panel or File menu (`Ctrl+S`). The Scene panel reports saved,
unsaved, and save-error states; there is no automatic save. Switching scenes or quitting with dirty
changes asks to save, discard, or cancel; a window-close request is deferred until that choice is
made. Undo and redo are available from the File menu, Scene panel, and `Ctrl+Z` / `Ctrl+Y`. They
retain up to 64 snapshots or 32 MiB, whichever limit is reached first. A continuous Inspector edit
or Ctrl+left-drag is one history action; uniform-scale steps, group, place, delete, rename, reparent,
clear, and double-click spawn each form one action. Asynchronous grounding is folded into its spawn
action when no later scene edit intervenes. Returning to the last saved document clears the unsaved
state; saving establishes a new baseline without discarding undo history.
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
