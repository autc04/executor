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
  to decide which `Item` subclass to create for a given directory entry.

**`Item` / `FileItem` / `DirectoryItem`**: the in-memory representation of a single
Mac file or directory. Each `Item` holds its CNID, parent CNID, host path, and Mac
name. Concrete subclasses (`PlainFileItem`, `AppleDoubleFileItem`,
`AppleSingleFileItem`, `BasiliskFileItem`) implement `getInfo()`, `setInfo()`, `open()`,
and `openRF()` for each storage format.

**CNID mapping**: Mac HFS uses catalog node IDs (CNIDs) to identify files and
directories. `LocalVolume` uses LMDB (via `lmdbcnidmapper.h`) to persist the
CNID ↔ host-path mapping across Executor sessions. `SimpleCnidMapper` is an in-memory
fallback used when LMDB is unavailable.

**Fork storage formats**:
- **Plain**: data fork is the raw file; resource fork is stored in a `._filename`
  (dot-underscore) sidecar or a `%filename` percent-encoded sidecar.
- **AppleDouble**: data fork is the raw file; metadata and resource fork are in a
  `._filename` companion file (macOS-compatible).
- **AppleSingle**: both forks plus metadata in a single file (`.ad` extension).
- **Basilisk**: resource fork stored in a `.rsrc/` subdirectory (Basilisk II format).

**`ItemCache`**: caches `Item` objects and manages CNID assignment. Cache misses scan
the parent directory and call `createItemForDirEntry` on each registered `ItemFactory`.

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
| `src/file/localvolume/basilisk.h` / `.cpp` | Basilisk II fork layout |
| `src/file/localvolume/lmdbcnidmapper.h` / `.cpp` | Persistent CNID mapper (LMDB) |
| `src/file/localvolume/simplecnidmapper.h` / `.cpp` | In-memory CNID mapper |
| `src/file/localvolume/openfile.h` | `OpenFile` interface for reading/writing a fork |
| `src/file/localvolume/mac.h` / `.cpp` | Mac-specific metadata helpers |
| `src/file/localvolume/cnidmapper.h` | `CnidMapper` abstract interface |

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
  a directory ID) survive across sessions. If the LMDB file is deleted, CNIDs are
  reassigned and any saved FSSpec values stored by applications become stale.
- **Hidden files**: `ItemFactory::isHidden()` suppresses sidecar files (like `._foo`)
  from appearing as Mac files. Always override this in new `ItemFactory` subclasses.
- **`upgradeItem`**: when a plain item's sidecar is discovered to contain valid
  AppleDouble data, the item is upgraded to an `AppleDoubleFileItem` in place so
  subsequent opens use the richer format.
- The `LocalVolume` constructor takes a `VCB&` reference; the VCB must remain valid
  for the lifetime of the volume object.
