# File Manager & Volume Abstraction

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

The File Manager implements the classic Mac OS file system API — `PBCreate`, `PBOpen`,
`PBRead`, `PBWrite`, `PBClose`, `PBGetFInfo`, `PBSetFInfo`, and the full HFS
parameter-block interface. It routes each call to the correct `Volume` implementation
depending on which volume a file lives on. Executor supports two volume backends:
`LocalVolume` (host directory mapped to a Mac volume) and the HFS volume driver
(reading real HFS disk images).

## Key Concepts

**Parameter blocks**: the Mac File Manager uses a family of parameter block structs
(`ParmBlkPtr`, `HParmBlkPtr`, `CInfoPBPtr`, …) passed by pointer to every trap. All
fields are `GUEST<>`-wrapped since the parameter blocks live in the guest address space.
The `Volume` interface mirrors this: each virtual method takes the same `ParmBlkPtr`
argument that the trap received.

**`Volume`**: the abstract base class (`src/file/volume.h`). Every volume in the system
has a `VCB` (Volume Control Block) and a corresponding `Volume*` pointer. The File
Manager dispatch maps a `vRefNum` to its `Volume` and calls the appropriate virtual
method. The `VCB` lives in the guest address space as a `GUEST_STRUCT`.

**Volume reference numbers**: Mac volumes are identified by a signed 16-bit `vRefNum`.
Executor assigns negative numbers to its own volumes, below −100 for HFS images and
starting at −5 for the floppy drive simulation.

**Working directories**: `PBOpenWD` / `PBCloseWD` implement the classic working
directory mechanism. A working directory is a `(vRefNum, dirID)` pair stored in a
guest-side table and referenced by a WD reference number.

**`file.h`**: the internal header that ties together the volume table, VCB list, and
the helper functions used by both `LocalVolume` and HFS.

## Source Files

| Path | Description |
|------|-------------|
| `src/file/volume.h` | Abstract `Volume` base class |
| `src/file/file.h` | Internal helpers, VCB list management |
| `src/file/volume.cpp` | Default `Volume` implementations (PBGetVInfo, etc.) |
| `src/file/fileHighlevel.cpp` | High-level File Manager traps (FSMakeFSSpec, etc.) |
| `src/file/fileMisc.cpp` | Miscellaneous file utilities |
| `src/file/fileUnimplemented.cpp` | Stubs for unimplemented file traps |

## Important Data Structures

- **`Volume`** (abstract class): one instance per mounted volume; holds a `VCB&`
  reference into the guest address space VCB queue.
- **`VCB`** (Mac ABI struct, guest): Volume Control Block — name, allocation block
  size, free block count, reference number, etc.
- **`ParmBlkPtr`** / **`HParmBlkPtr`** / **`CInfoPBPtr`**: Mac parameter block union
  types. All fields accessed via `GUEST<>` accessors.
- **`FCB`** (File Control Block): per-open-file state in the guest address space. Each
  open call allocates an FCB entry and returns a refNum.

## Key Functions / Traps

| Symbol | Trap | Description |
|--------|------|-------------|
| `C_PBOpen` | `_Open` | Open a file for reading or writing |
| `C_PBCreate` | `_Create` | Create a new file |
| `C_PBRead` | `_Read` | Read bytes from an open file |
| `C_PBWrite` | `_Write` | Write bytes to an open file |
| `C_PBClose` | `_Close` | Close an open file |
| `C_PBGetFInfo` | `_GetFInfo` | Get file metadata (type, creator, dates) |
| `C_PBSetFInfo` | `_SetFInfo` | Set file metadata |
| `C_PBGetCatInfo` | `_GetCatInfo` | Get file or directory catalog info |
| `C_PBOpenWD` | `_OpenWD` | Open a working directory |
| `C_FSMakeFSSpec` | — | Convert a partial path to an `FSSpec` |

## Design Notes / Gotchas

- **Dual-fork model**: Mac files have a data fork and a resource fork. `Volume`
  exposes `PBOpenDF` (data fork) and `PBOpenRF` (resource fork) as separate virtual
  methods. The `LocalVolume` backend maps these to separate host-side access paths
  depending on the file format (AppleDouble, AppleSingle, or plain).
- **vRefNum routing**: the File Manager maps a `vRefNum` to a `Volume*` through the
  VCB queue in guest memory. When a new volume is mounted, a new `VCB` is inserted at
  the head of `LM(VCBQHdr)` and a `Volume*` is stored in a parallel host-side map.
- **Error propagation**: every File Manager trap stores its result in the parameter
  block's `ioResult` field (guest byte-order) **and** returns the same value from
  the C++ function. Both must be kept in sync.
- Some traps accept either a `vRefNum`-based or an `FSSpec`-based path; `fileHighlevel.cpp`
  normalises these before calling the `Volume` methods.
