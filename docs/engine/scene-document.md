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

## Project storage

`ProjectStore` opens an existing project directory selected by its caller and receives the complete
set of mounted read-only asset directories. It rejects project roots that equal, contain, or are
contained by any of those asset directories, and rechecks that separation before reads and writes.
It stores a document as `<scene-id>.scene.json` directly inside the project directory. The scene ID
is validated before it is used as a filename; callers cannot supply a path. Saves write a unique
sibling temporary file and atomically replace the destination. Loads reject symlinked scene files,
enforce the file-size cap, and leave the caller's output document unchanged on failure.

The project writer is separate from the VFS. Asset directories, archives, and `.scpak` mounts stay
read-only inputs. This slice does not create a project UI, asset browser, click-to-place tool,
hierarchy panel, transform gizmo, undo system, or game adapter; those can build on this document
contract in later slices.

`ResolveWorldTransforms` first validates the document, then emits resolved matrices in the original
node order regardless of parent ordering in the serialized array. It leaves its output unchanged
if composition overflows or a world transform is singular or numerically unstable. The editor's
existing flat in-memory sample scene is not yet loaded from or saved back to `Document`; project
UI, outliner, click-to-place and undo/redo remain follow-on work.
