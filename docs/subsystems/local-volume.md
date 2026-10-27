# Local Volume / Host-Directory Mapping

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

`LocalVolume` maps a directory on the host file system to a virtual Mac HFS volume,
making its contents appear to a Mac application as a mounted disk. This is the primary
way users give Mac apps access to their host files. It supports multiple on-disk file
formats (plain Unix, AppleDouble, AppleSingle, Basilisk II) to preserve the Mac-
specific data fork / resource fork duality that is absent from most host file systems.

## Key Concepts

**`LocalVolume`**: subclasses `Volume` and implements all `PB*` virtual methods by
translating Mac file-system calls into `std::filesystem` operations. It maintains:
- `root`: the host directory path that is the Mac volume root.
- `ItemCache`: a cache of `Item` objects keyed by catalog node ID (CNID).
- `itemFactories`: an ordered list of `ItemFactory` instances tried in priority order
  to decide which `FileRep` (storage format) a file gets. The gauntlet runs at
  Materialize, not during a directory scan.

**`Item` / `FileItem` / `DirectoryItem`**: the in-memory representation of a single
Mac file or directory. Each `Item` holds its CNID, parent CNID, host path, and Mac
name. A `FileItem` owns an optional `FileRep` (its storage-format concern): the
concrete reps (`PlainRep`, `AppleDoubleRep`, `AppleSingleRep`, `BasiliskRep`,
`MacBinaryRep`, `MacRep`) implement `open()`, `openRF()`, `getInfo()`,
`setInfo()` and the sidecar-aware `remove()`/`move()` for each format.

**CNID mapping**: Mac HFS uses catalog node IDs (CNIDs) to identify files and
directories. `LocalVolume` uses LMDB (via `lmdbcnidmapper.h`) to persist the
records and the `(parID, posixName)` and `(parID, Mac name)` indexes across
Executor sessions. A record stores `parID`, `posixName`, an optional `Mac name`,
the `(dev, ino)` identity, and the kind; the host path is derived from the parent
chain, not stored, and identity is a verification value rather than a lookup key.
There is one LMDB environment per volume under `$ExecutorDirectoryMap` (default
`~/.executor/cnidmap1`), named `root` for `/`, `drive-C` for a Windows `C:/`, and
`path-...` for any other root. `CNIDMapper` is a deliberately dumb value
store: it provides records and indexes plus one transaction seam, while the
reconciliation and naming policy lives in `ItemCache`. `SimpleCNIDMapper`
implements the same backend interface in memory; it is the reference backend and
the runtime fallback when the persistent store cannot be opened.

**Fork storage formats**:
- **Plain**: data fork is the raw file, no resource fork exists. Can be upgraded
  to 'Macintosh' or 'AppleDouble' (depending on platform).
- **Macintosh**: on macOS only, uses xattrs to access the real mac's resource fork
- and legacy metadata.
- **AppleDouble**: data fork is the raw file; metadata and resource fork are in a
  `._filename` companion file (macOS-compatible).
- **AppleSingle**: both forks plus metadata in a single file (`.ad` extension).
- **Basilisk**: resource fork stored in a `.rsrc/` subdirectory (Basilisk II format).
- **MacBinary**: both forks plus Finder metadata in a single `.bin` file with a
  128-byte header. Detected last (just before the plain fallback), so a sidecar
  pair describing a file named `foo.bin` wins over the MacBinary interpretation.

**`ItemCache`**: caches `Item` objects and owns all CNID policy: reconciliation,
get-or-create, batch naming, recursive delete and move. Listing a directory stores
a descriptor per child and assigns each child a CNID; an `Item` is realized from a
descriptor on demand (`makeItem`: `DirectoryItem` for a directory, bare `FileItem`
otherwise). The file's storage format is decided later, on first access, by
`materialize()`.

**Materialization**: `FileItem` holds an optional `FileRep`; `open()`, `openRF()`,
`getInfo()`, `setInfo()`, `deleteItem()` and `moveItem()` call
`ensureMaterialized()` first, which runs the `ItemFactory` gauntlet
(`createRepForFile`) on first access, and again only if the gauntlet's probe trace
has gone stale. Scanning a directory therefore does not probe every entry; only
entries actually touched are classified.

**`FCBExtension`**: a host-side extension to the guest FCB (File Control Block). It
stores the `OpenFile` pointer and the fork selector for an open file descriptor. The
`openFCBX()` method allocates a slot in the FCB extension table and returns a file
reference number.

## Source Files

| Path | Description |
|------|-------------|
| `src/file/localvolume/localvolume.h` / `.cpp` | `LocalVolume` class |
| `src/file/localvolume/item.h` / `.cpp` | `Item`, `FileItem`, `DirectoryItem` |
| `src/file/localvolume/itemcache.h` / `.cpp` | `ItemCache` — CNID-to-Item mapping |
| `src/file/localvolume/plain.h` / `.cpp` | Plain Unix file item |
| `src/file/localvolume/appledouble.h` / `.cpp` | AppleDouble / AppleSingle items |
| `src/file/localvolume/macbinary.h` / `.cpp` | MacBinary items |
| `src/file/localvolume/basilisk.h` / `.cpp` | Basilisk II fork layout |
| `src/file/localvolume/lmdbcnidmapper.h` / `.cpp` | Persistent CNID mapper (LMDB) |
| `src/file/localvolume/simplecnidmapper.h` / `.cpp` | In-memory CNID mapper |
| `src/file/localvolume/openfile.h` | `OpenFile` interface for reading/writing a fork |
| `src/file/localvolume/mac.h` / `.cpp` | Mac-specific metadata helpers |
| `src/file/localvolume/cnidmapper.h` | `CNIDMapper` abstract interface |

## Important Data Structures

- **`Item`**: base class; `cnid_`, `parID_`, `path_`, `name_`.
- **`ItemInfo`**: `{file.info, file.xinfo, modTime, creationTime}` union for file/dir;
  mirrors Mac's `FInfo`/`FXInfo` and `DInfo`/`DXInfo`.
- **`LocalVolume::FCBExtension`**: per-open-file host state not present in the guest FCB.
- **`LocalVolume::NonexistentFile`**: `{parent, name}` — a resolved but not-yet-created
  file location, used by `createCommon`.

## Design Notes / Gotchas

- **Fork transparency**: `open()` returns the data fork; `openRF()` returns the
  resource fork. The concrete `Item` subclass decides where to find each fork on the
  host.
- **CNID persistence**: LMDB stores CNIDs so that open-by-CNID calls (`PBOpenDF` with
  a directory ID) survive across sessions. If the database is deleted, CNIDs are
  reassigned and any saved FSSpec values stored by applications become stale.
  If the database cannot be opened at mount time (permissions, disk full, a
  corrupt or old-schema database, or a directory that belongs to another root),
  `LocalVolume` prints a prominent warning and mounts with `SimpleCNIDMapper`
  instead of aborting the emulator; persistence resumes on a later run once the
  problem is fixed. Failures after mount (LMDB I/O errors, corrupt records)
  surface as `ioErr` from the failing File Manager operation.
  Identity validation assumes `(dev, ino)` is stable across a restart; on tmpfs or
  overlayfs a fresh mount changes it, and the next re-list resets the persisted
  mapping (Phase 11a accepts this limitation). `--dont-use-inodes` disables
  identity validation for the whole process: `statPath()` reports no usable
  identity, so matching falls back to host name and kind (and rename recovery is
  off); records written while it was on re-adopt inodes on the next reconcile.
  Identity validation is chosen per filesystem instance (`st_dev`), classified
  lazily from the host filesystem type: block-backed local filesystems use it,
  while tmpfs, overlayfs, NFS, FUSE and unknown types fall back to name+kind. A
  volume can span several devices (nested or bind mounts).
- **Hidden files**: `ItemFactory::isHidden()` suppresses sidecar files (like `._foo`)
  from appearing as Mac files. Always override this in new `ItemFactory` subclasses.
- **Factory order matters**: `itemFactories` is tried in order and the first match
  wins. MacBinary is a single-file format, so it is registered *after* the
  sidecar/xattr backends (AppleDouble, Basilisk, Mac) and just before the plain
  fallback; otherwise a `._foo.bin` companion would never be seen. See
  `docs/ai/2026-10-04-macbinary-localvolume-backend.md`.
- **`upgradeItem`**: when a plain item's sidecar is discovered to contain valid
  AppleDouble data, a richer `FileRep` is installed on the item in place so
  subsequent opens use the richer format.
- The `LocalVolume` constructor takes a `VCB&` reference; the VCB must remain valid
  for the lifetime of the volume object.
