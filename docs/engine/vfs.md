# Virtual file system

`include/scuba/vfs/` gives the engine one namespace over loose directories, archives and
generated content, with sco-core's game-file overrides layered on top.

## Paths

Every path is normalized (`NormalizePath`): ASCII lowercase, `/` separators, no leading,
trailing or repeated `/`, and no `.` or `..` segments. `\Data\Objects//Ship.CGF` becomes
`data/objects/ship.cgf`. Lookups are therefore case-insensitive on every platform, the same rule
`sco::vfs` applies to game paths. A path that doesn't normalize (`../x`, `c:/x`, too long) is
refused, never resolved.

## Backends (`FileSystem`)

| Backend | Open with | Notes |
|---|---|---|
| Directory | `OpenDirectory(root, status)` | Indexed at open (symlinks skipped, at most 1M files). Files created later are found by a direct probe; `List` shows them after a remount. |
| Zip | `OpenZip(path, status)` | Stored and deflated (miniz) entries. A deflated entry is inflated whole into memory at open, with a 1 GiB limit and a CRC check. Encrypted entries, zip64 and multi-disk archives are refused with a reason. |
| `.scpak` | `OpenPack(path, status)` | scuba-engine's own container, below. |
| Memory | `MemoryFileSystem(files)` | Tests, tools, generated content. |

`File` handles use positional reads (`ReadAt`), so a handle has no shared cursor. Disk-backed
reads are safe from several threads at once (overlapped offsets on Windows, `pread` elsewhere).

## The mount stack (`VirtualFileSystem`)

```cpp
vfs::VirtualFileSystem vfs;
Status st;
vfs.Mount(vfs::OpenDirectory("content", st), {"", 0, "content"});            // root, lowest
vfs.Mount(vfs::OpenPack("packs/ui.scpak", st), {"ui", 10, "ui.scpak"});      // serves ui/...
vfs.Mount(vfs::OpenDirectory("dev/ui", st), {"ui", 20, "dev overrides"});    // shadows the pack
auto file = vfs.Open("UI/Hud/Panel.json");
```

- **Resolution:** mounts are searched from the highest priority down; at equal priority the
  later mount comes first. The first mount that has the path serves it and hides it in every
  mount below.
- **Mount points:** a mount at `ui` serves `ui/<path>` from its backend's `<path>`. A mount at
  `""` serves the root.
- **No silent fall-through:** if the winning mount has the file but can't serve it (encrypted
  zip entry, corrupt header), `Open` fails with that reason instead of serving a shadowed copy.
- **Concurrency:** `Mount` and `Unmount` publish a new immutable stack; readers take a snapshot
  and never wait for mounts. An open handle keeps its backend alive after an unmount.
- **Diagnostics:** `Mounts()` lists the stack in resolution order, `Resolve(path)` names the
  mount that serves a path (`label:relative`), and `List(prefix)` returns the union with shadowing
  applied.

## sco-core overrides

`SetOverrides(const sco::vfs::MountTable*)` layers sco-core's game-file override table above the
stack. When the table mounts a path, the file the stack resolved becomes the base of a composed
virtual file (`sco::vfs::Table::Open`): base ranges plus replacement bytes, with sco-core's
merge, priority and inert-mount rules. Plugins' DataCore and `files/` packs therefore apply to
engine reads exactly as they do to the game's.

## .scpak

Read-optimized and deliberately simple: a 32-byte header, entries stored contiguously on 16-byte
boundaries, and one index sorted by path with a CRC-32 per entry. Layout:
`include/scuba/vfs/pack.h`. Only stored entries exist in version 1; a compressed entry is refused
at open.

```sh
scuba-pack build out.scpak some/dir     # every file under the directory
scuba-pack list out.scpak               # also lists .zip archives
scuba-pack verify out.scpak             # every CRC-32
```

## Limits and next steps

- Directory indexes are snapshots; a watcher that republishes the stack is phase 2.
- Large deflated zip entries are inflated in memory; streaming inflate comes with the
  priority IO queue in the texture streaming phase.
- No write API: the VFS is read-only by design. Saves go to a real directory through a writer
  that phase 2 adds.
