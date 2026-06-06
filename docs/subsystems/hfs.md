# HFS Volume Implementation

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

The HFS module reads and writes real Hierarchical File System (HFS) disk images.
It implements the full on-disk B-tree layout used by Mac OS HFS volumes, including
volume header parsing, extents B-tree, catalog B-tree, and file data read/write.
This allows Executor to boot from or access `.dsk` / `.img` disk image files without
any conversion.

## Key Concepts

**Physical layout**: an HFS volume starts at block 2 (`VOLUMEINFOBLOCKNO`). The
`volumeinfo` struct at that block contains the volume descriptor fields (`drSigWord`,
`drAlBlkSiz`, extent records, catalog size, etc.). The signature `0x4244` ("BD")
identifies an HFS volume.

**B-tree structure**: HFS stores its catalog and extents in B-trees. Each tree node
(`btnode`) is a fixed-size block with a forward/backward link, a node type (`indexnode`,
`leafnode`, `headernode`, `mapnode`), and a record count. Index nodes contain key/
pointer pairs; leaf nodes contain the actual catalog or extent records.

**Catalog B-tree**: maps `(parentID, name)` pairs to file or directory records. The
catalog B-tree entry types are `dirRecord` (directory) and `fileRecord` (file). Each
file record contains the `FInfo`/`FXInfo` fields, creation/modification dates, and the
extent records for the first three data-fork and resource-fork extents.

**Extents B-tree**: handles overflow extents when a file has more than three extent
records. Each entry maps `(fileID, forkType, startBlock)` to an extent descriptor.

**`xtntrec`**: three `xtntdesc` (blockstart, blockcount) pairs; the standard extent
record for up to three contiguous allocation runs per fork.

**Block I/O**: physical reads and writes go through `hfsMisc.cpp` which calls the
host-side disk I/O layer. Allocation blocks are mapped from the volume's allocation
block table to physical disk blocks using the allocation block start (`drAlBlSt`) and
size (`drAlBlkSiz`).

**DOS disk fixups** (`futzwithdosdisks.cpp`): some disk images contain an MBR or
partition table that must be skipped to find the HFS volume descriptor. The
`futzwithdosdisks` logic handles this offset.

**Partition support** (`partition.h`): Apple Partition Map parsing for images with
multiple partitions.

## Source Files

| Path | Description |
|------|-------------|
| `src/hfs/hfs.h` | Core types: `volumeinfo`, `btnode`, `xtntdesc`, `xtntrec` |
| `src/hfs/hfsBtree.cpp` | B-tree traversal, search, insert, delete |
| `src/hfs/hfsVolume.cpp` | Volume mount/unmount, VCB initialisation |
| `src/hfs/hfsMisc.cpp` | Physical block I/O, allocation block mapping |
| `src/hfs/hfsFile.cpp` | File open/read/write/close via extents |
| `src/hfs/hfsCreate.cpp` | File and directory creation |
| `src/hfs/hfsHier.cpp` | Directory hierarchy traversal |
| `src/hfs/hfsChanging.cpp` | File modification, rename, delete |
| `src/hfs/hfsHelper.cpp` | Miscellaneous HFS helpers |
| `src/hfs/hfsXbar.cpp` | Cross-volume operations |
| `src/hfs/hfsWorkingdir.cpp` | Working directory support within HFS volumes |
| `src/hfs/futzwithdosdisks.cpp` | DOS / MBR partition offset detection |
| `src/hfs/partition.h` | Apple Partition Map definitions |
| `src/hfs/hfs_plus.h` | HFS+ on-disk struct definitions (partial, read-only) |

## Important Data Structures

- **`volumeinfo`** (guest struct): on-disk volume descriptor at block 2.
- **`btnode`** (guest struct): B-tree node header; `ndType` distinguishes node kinds.
- **`xtntdesc`** (guest struct): `{blockstart, blockcount}` — one contiguous extent.
- **`xtntrec`**: array of 3 `xtntdesc` entries; embedded in catalog file records and
  the volume descriptor.
- **`HVCB`** (alias for `VCB`): the volume control block used by HFS; extended by
  Executor with a host-side file descriptor.

## Key Functions

| Symbol | Description |
|--------|-------------|
| `hfs_mount(dRefNum)` | Mount an HFS volume and populate the VCB |
| `hfs_read_btree_node(vcb, tree, nodeNum)` | Load a B-tree node from disk |
| `hfs_search_catalog(vcb, parID, name)` | Look up a file/directory by name |
| `hfs_allocate_blocks(vcb, count)` | Allocate contiguous allocation blocks |

## Design Notes / Gotchas

- All on-disk structures are big-endian; they are read through `GUEST<>` wrappers, so
  byte-swapping is automatic on little-endian hosts.
- `PHYSBSIZE = 512` is the physical sector size; allocation blocks are always a
  multiple of 512 bytes.
- The `DOSFDBIT` and `ASPIFDBIT` flags ORed into a file descriptor indicate the disk
  access path (standard UNIX fd vs. ASPI). New code should use the standard path.
- HFS+ (Extended HFS) disk images are not writable; `hfs_plus.h` contains read-only
  struct definitions for detecting and partially reading HFS+ volumes.
- `MADROFFSET = 40` is the byte offset of the master directory record from the start
  of the volume descriptor block, used when interpreting older MFS/HFS disk images.
