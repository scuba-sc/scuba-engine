# Asset Lab

Asset Lab is a local authoring tool. Point it at files in **your own local
installation**, or at models you created. This repository contains no game data,
extracts, textures, meshes or proprietary fixtures. It does not find an install,
unpack a P4K, contact game services, or write into the installation. Nothing here
has been tested in Star Citizen.

Build the host and studio as described in the README, start `scuba-hostd`, then
select **Asset Lab**. Paths in the panel are paths on the machine running hostd,
not browser uploads. The bridge has the same local-filesystem authority as hostd;
keep its default loopback binding.

## Database workflow

1. Open a local `Game2.dcb`. The linked **sco_datacore** library validates the
   schema before any browsing. Records are grouped by struct, searchable by name,
   type or GUID, and paged in groups of 100 (RPC maximum 500).
2. Select a record to see typed field paths and base values. References link to
   target records by GUID. **Structs** displays inherited property definitions.
   Opaque structs remain opaque; the SDK decides which layouts are patchable.
3. **Stage field** adds a `[[set]]` operation to the TOML editor. Edit its value.
   Pointer operations can instead be authored with the kit's `pointer` syntax.
   Arrays, instance creation and new-record operations use the SDK pack grammar;
   the editor accepts the entire grammar, not a second reduced dialect.
4. **Check pack** parses the TOML using the kit. With a DCB open it also resolves
   every operation and emits/revalidates a trial patch; refused or partial packs
   cannot replace the last accepted pack. The inspector still shows **base
   values**, not a materialized patched database.
5. **Download TOML** checks again and downloads canonical SDK `WritePack` output.
   It does not overwrite the source DCB or input pack.

Opening a `.toml` alone lists its semantic operations and any declared struct
names. A pack is not a database: inherited structs, resolved field values and
reference targets require the user's base DCB. Opening another file replaces
the host-wide Asset Lab session (shared by local studio clients).

## Supported geometry

The Cry reader is new code written from the chunk-table model and binary field
descriptions, not copied from another converter implementation. The public
declarative CGF format description consulted for stream, mesh and material field
semantics is `cgf.xml` in the Cryengine-Converter project:
`https://github.com/Markemp/Cryengine-Converter/blob/master/cgf.xml`.
No converter source or game files were used as fixtures or vendored.

The supported subset is deliberately explicit:

| Input | Accepted layout | Output and limits |
| --- | --- | --- |
| `.cgf`, `.cga` | `CrCh`, little-endian file version `0x746`; 16-byte table entries (type/version/id/size/offset); mesh `0x800`; stream `0x800`; material-name `0x800` | Static local-space triangles. Float32 XYZ positions/normals, float32 UV pairs, uint16/uint32 indices. Material name is retained as mesh metadata. |
| `.obj` | Text `v`, `vn`, `vt`, `f`; positive and relative negative indices | Faces fan-triangulated; use convex polygons or pre-triangulate. UV/normal indices expanded independently. Incomplete optional normals/UVs are omitted. `o`, `g`, `s`, `usemtl`, `mtllib` are accepted but material files are not followed. |
| `.glb` | glTF 2.0, JSON chunk then one embedded BIN chunk | Static triangle primitives, scalar/vector accessors, embedded images; buffer ranges, accessor strides and index ranges checked. |
| `.gltf` | glTF 2.0, one relative local `.bin` buffer | Converted to self-contained GLB. Images must already use bufferViews. Network/data URIs, path traversal and symlink escapes refused. |

Unknown Cry file, mesh, stream or material versions are errors. Overlapping chunks,
duplicate IDs, missing streams, compressed mesh streams, nonzero mesh flags,
mesh subsets, non-finite vertices and out-of-range indices are refused.
Unrelated bounded chunks are ignored. Node transforms, scene hierarchy, skins,
animation/controller tracks, physics, split streams, newer StarEngine packed
vertex encodings and external `.mtl` XML interpretation are **not supported**.
`.cga` means its static mesh chunks, not animation playback. This is not a claim
that arbitrary contemporary StarEngine assets can be decoded.

The glTF importer refuses sparse accessors, matrix accessors, morph targets,
skins, animation and required extensions other than `MSFT_texture_dds`. It is a
bounded static-geometry importer, not a complete glTF conformance validator.

## DDS and glTF

The DDS reader validates magic, 124-byte header, 32-byte pixel format, dimensions,
DX10 header, typed DXGI format, single-2D shape, complete mip sizes and exact
payload length. It supports legacy DXT1/DXT3/DXT5, ATI1/ATI2, BC4U/S and BC5U/S,
plus these DX10 formats:

| BC family | DXGI values |
| --- | --- |
| BC1 | 71, 72 |
| BC2 | 74, 75 |
| BC3 | 77, 78 |
| BC4 UNORM/SNORM | 80, 81 |
| BC5 UNORM/SNORM | 83, 84 |
| BC6H UF16/SF16 | 95, 96 |
| BC7 UNORM/sRGB | 98, 99 |

Typeless DXGI formats, cubes, arrays, volumes, zero dimensions, impossible mip
counts and trailing/split texture payloads are refused. Dimensions are capped
at 16384. The native reader does **not** implement a software BC decoder or
transcode to PNG on export. A DDS is converted to a textured quad GLB that embeds the original DDS,
losslessly, using the standard **MSFT_texture_dds** extension. This is glTF 2.0
with a required extension, not universally viewable core-only glTF.

The studio GLTFLoader plugin reads that embedded bufferView into a Three.js
CompressedTexture. BC1-3 need GPU S3TC, BC4-5 RGTC, and BC6H/BC7 BPTC support.
For GPUs without S3TC, BC1 has a small clean-room RGB565/interpolation decoder for
previewing the top mip (limited to 4096x4096, at most 64 MiB of RGBA). Export still
preserves the original compressed DDS. If the GPU lacks another format, the panel
says so and still permits downloading the converted GLB. BC6H is preserved
without HDR exposure/tone-mapping authoring. Texture preview treats the DDS as a
base-color image; legacy DDS does not carry an explicit color-space declaration.

DDS field definitions follow Microsoft's DDS Programming Guide and DDS_HEADER /
DDS_HEADER_DXT10 structures. GLB container structure follows Khronos glTF 2.0;
embedded DDS follows Microsoft's MSFT_texture_dds specification. These are
format descriptions; no third-party reader implementation is included.

## Custom model pack import

Enter a `.glb`, `.gltf` or `.obj` model path and expand **Import a custom model as
a pack**. Choose a new destination folder and a lowercase relative `.glb` VFS
path. Check the TOML editor, then create the pack:

```text
chosen-pack/
  datacore/
    overrides.toml
  files/
    objects/custom/model.glb
```

The TOML is the checked editor contents, canonicalized by the kit. An empty
override pack is valid if no record change is wanted. Existing destination
folders are never overwritten. Input models are read-only. A failed write
removes only the new partial destination.

**A GLB in `files/` is an authoring asset, not an engine-native CGF replacement.**
No glTF-to-Cry encoder or runtime glTF loader is claimed. The layout is ready
for a VFS consumer, but mounting arbitrary `files/` replacements in-game and
mapping imported meshes into game records remain consumer work. DataCore
overrides are actual SDK packs; their applicability depends on the user's DCB.

## API and limits

All methods use the foundation `{id, method, params}` envelope. `-32060` means an
asset format or filesystem operation was refused; `-32602` means bad parameters.
Handlers live in `src/hostd/assets_handlers.cpp`, registered through
`RegisterAssetsHandlers`. Native reader APIs are in `src/assets/formats.h`.

| Method | Parameters | Result |
| --- | --- | --- |
| `assets.open` | `{path}` | `{kind:"dcb" or "toml", records, structs, pack}` |
| `assets.records` | `{offset?, limit?, filter?}` | `{items:[{name,guid,struct,file?}], total, offset}`; TOML items also include field/line |
| `assets.structs` | `{}` | `[{name,parent?,opaque?,properties?:[{name,type,conversion,typeIndex}]}]` |
| `assets.fields` | `{record}` (name or GUID, optional `guid:` prefix) | `[{path,type,value,array,depth,reference?:{name,guid,struct,file}}]` |
| `assets.pack` | `{text}` | `{text}` (validated canonical pack) |
| `assets.export` | `{}` | `{text}` (last accepted pack) |
| `assets.convert` | `{path}` | `{glb:base64, bytes}` |
| `assets.import` | `{path,destination,virtualPath}` | `{created:true}` |

File limit: 256 MiB for models/textures, 2 GiB for DCBs, SDK 4 MiB per TOML.
Preview transport: 24 MiB of GLB before base64. Geometry: 4 million vertices
and 12 million indices per Cry/OBJ mesh. Field traversal follows at most eight
pointer hops. Conversion and database operations are synchronous on hostd's
main thread; large inputs can delay other RPCs. No background task system or
progress protocol is introduced by this lane.

## Verification

Run from the worktree root (studio tests consume native generated fixtures):

```sh
cmake -S . -B build -DSCUBA_SDK_ARCHIVE_DIR="$SDK_ASSETS"
cmake --build build -j 10
ctest --test-dir build --output-on-failure
cd studio
npm ci
npm test
npm run build
```

`tests/asset_fixtures.h` generates a triangle chunk file, a 4x4 red BC1 DDS,
typed DX10 BC1-7 headers and a two-record DCB with a GUID reference. Nothing is
read from a game install. Native tests cover every truncation of the triangle,
DDS, DCB and GLB, corruption and byte mutations, import traversal, record paging,
reference resolution, rejected overrides, new-pack import, `sco-dcb lint` and
`sco-dcb check`. Vitest loads the native GLBs with the actual studio GLTFLoader,
checks geometry and embedded DDS mip payloads, and loads an imported OBJ pack.

For reproducible macOS browser evidence, run the built host from the worktree
root on port 17966 in a separate terminal, then run from the worktree root:

```sh
bun tests/asset_visual.mjs "$VISUAL_DIR"
```

The script uses Bun's built-in WebKit view, generates ten `eng6-*.png` captures
at desktop/mobile widths, and exercises database browsing, typed override checks,
three mesh angles, the BC1 preview, OBJ pack creation and a refused traversal.
It uses only the native synthetic fixtures and writes its test pack under the
build tree. It does not use an installed game's files.
