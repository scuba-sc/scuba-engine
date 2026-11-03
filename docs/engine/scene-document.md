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
enforce the file-size cap, and leave the caller's output document unchanged on failure.

The project writer is separate from the VFS. Asset directories, archives, and `.scpak` mounts stay
read-only inputs. The native editor opens one scene by stable ID with
`scuba-engine.exe --project <existing-project-directory> --scene <scene-id>`; `--scene` defaults to
`main`. If the scene file does not exist, the editor starts an empty, unsaved scene. It does not
create the project directory. The app checks directory mounts through `ProjectStore` and checks
archive mount paths separately before opening the session.

The Scene panel is a document-backed outliner. Stable node IDs are used for selection and the
Inspector edits local translation, quaternion rotation, and nonuniform scale. Groups can be added
and nested; deleting a group removes its subtree. The Content panel keeps its existing double-click
quick placement and adds a “Place with click” mode: it shows a cyan-tinted viewport preview, click
places the model, and Escape cancels. If a group is selected when placement starts, the new model
is parented to that group. Material edits are currently session-only because materials are not in
scene schema version 1.

Saving is explicit through the Scene panel or File menu (`Ctrl+S`). The Scene panel reports saved,
unsaved, and save-error states; there is no automatic save. Without `--project`, the editor still
uses the same document and outliner in memory and says that the session will not persist.

`ResolveWorldTransforms` first validates the document, then emits resolved matrices in the original
node order regardless of parent ordering in the serialized array. It leaves its output unchanged
if composition overflows or a world transform is singular or numerically unstable. The editor now
loads and saves the selected document, draws its model nodes with resolved matrices, routes viewport
picks back to stable node selection, and stores asset placement in the document. Scene browser,
undo/redo, drag-and-drop placement, and Windows D3D12 visual validation remain follow-on work.
