# Local Volume / Host-Directory Mapping

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

`LocalVolume` maps a directory on the host file system to a virtual Mac volume, so a
Mac application sees host files through the normal File Manager interface. On Unix,
`MountLocalVolumes` mounts `/` as the local volume; on Windows it mounts each drive.
The volume's name and `vRefNum` come from the VCB passed to the constructor.

It supports several on-disk conventions that preserve the Mac file model: plain Unix
files, AppleDouble, AppleSingle, Basilisk II, MacBinary, and (on macOS only) native
xattrs.

## The Mac file model

The File Manager presents a volume as a tree of files and directories with properties
that the rest of this document assumes:

- **Names** are MacRoman byte strings (the classic Mac character encoding), compared
  case-insensitively and limited to 31 characters (31 MacRoman bytes). A Mac name may
  not contain `:`; the File Manager uses `:` as its path separator. Two host names
  that fold to the same Mac name must be disambiguated (see *Naming*), and a host
  name that cannot be represented in MacRoman is degraded rather than rejected.
- **Catalog node IDs.** Every file and directory has a 32-bit **CNID** (catalog node
  ID) that is unique within the volume and denotes that item for its lifetime.
  Applications hold CNIDs indirectly in `FSSpec`s and file ID references, and the File
  Manager addresses directories by CNID (`dirID`), so a CNID must not change when an
  item is renamed or when Executor restarts. HFS stores CNIDs on disk; a host
  directory does not, so Executor persists them in an LMDB database (see *CNID store
  and identity*). CNID 1 is the parent of the volume root and CNID 2 is the root.
- **Two forks.** A file has a data fork (the ordinary file contents) and a resource
  fork (structured Mac resources). They are opened separately, have independent
  lengths, and either may be empty.
- **Finder metadata.** A file has Finder information (`FInfo`: type, creator, flags,
  icon location, folder), extended Finder information (`FXInfo`), and creation and
  modification dates. Directories have `DInfo`/`DXInfo` and a child count
  (`ioDrNmFls`) that includes both files and subdirectories.
- **Addressing.** A volume is selected by a `vRefNum`, a directory by a CNID
  (`dirID`), and a child by a name relative to a directory. Paths are colon-separated
  sequences of names; index-based enumeration (positions `1..n`) uses the volume's
  listing order.

## Design goals

- **Resolve a path without listing directories along the way.** Locating `dir/file`
  means asking the host for metadata (`stat`) for each component in turn; it must not
  read the containing directory's entries to find the next component.
- **Answer a directory's child count cheaply.** The File Manager asks for `ioDrNmFls`
  of every directory it displays. Answer it from a readdir tally or an already-built
  listing, without constructing per-child state or probing file formats.
- **Classify a file's storage format only when the file is used.** Building a
  directory listing collects names, kinds, and each entry's host identity; it does not
  read file contents, companion (sidecar) files, or xattrs. The format is decided the
  first time the file is opened or its metadata is read.
- **Preserve CNIDs and Mac names.** A same-directory host rename keeps the CNID, and
  CNIDs and decided Mac names survive restarts through the CNID store.
- **Keep identity separate from storage format.** An item can have a CNID and a name
  without its format being known; changing the format (for example, upgrading a plain
  file to AppleDouble) must not change its identity.

## Architecture at a glance

```
LocalVolume            File Manager traps, resolution, FCB bookkeeping
   ├── ItemCache       object model, CNID policy, realization registry, LRU
   │     ├── CNIDMapper      records + indexes + one transaction seam
   │     └── Item            one realized file or directory
   │           ├── DirectoryItem   Count / List / NameResolve machinery
   │           └── FileItem        owns an optional FileRep
   └── ItemFactory      format gauntlet (tries each format's rep)
```

`LocalVolume` is a `Volume`; `ItemCache` owns all CNID and naming policy; `CNIDMapper`
is deliberately a dumb value store; `FileRep` isolates the storage format from the
identity object.

## Core objects

### `LocalVolume`

`LocalVolume` implements the `Volume` virtual `PB*` methods. It owns:

- `root`: the host directory that is the Mac volume root.
- `itemCache`: the object model (see below).
- `itemFactories`: the ordered format gauntlet, plus the `defaultItemFactory` (plain)
  and `upgradedItemFactory` (AppleDouble, or Mac on macOS).
- `fcbExtensions`: per-open-file host state (see *Fork I/O and FCBs*).

The CNID backend is selected at construction: `LMDBCNIDMapper` if its database can be
opened, otherwise `SimpleCNIDMapper` with a warning on stderr (permissions, a full
disk, a corrupt or old-schema database, a reused database directory, …). Losing
persistence is a per-run degradation, not a mount failure.

`LocalVolume` keeps the `VCB&` it was constructed with; the VCB must outlive the
volume. The constructor also warms the root item (CNID 2), but nothing pins it.

### `Item`, `DirectoryItem`, `FileItem`

The object model is **link-free**: no `Item` owns another `Item`. A directory stores a
descriptor list, and parenthood lives in the CNID store. A realized `Item` is kept
alive only by the realization LRU or by an external reference (an open FCB, a local
`shared_ptr`).

- **`Item`** — identity and common operations: `itemcache_`, `parID_`, `cnid_`,
  `path_`, and an optional decided `name_`. Its record stores no path; `path_` is
  derived from the `parID` chain plus `posixName` and is kept current when the item is
  relocated after a host rename. `Item::name()` is the only name reader and funnels
  through `ItemCache::ensureNamed`.
- **`DirectoryItem : Item`** — the directory stages and their payloads:
  an optional `count_` + `countStamp_` (Counted), and a descriptor list
  (`descriptors_`) + `stamp_` + `listed_` (Listed). Descriptors are sorted by
  `(prefix, posixName)`. There is no child `Item` list. A listed directory's
  `cacheWeight()` is its descriptor count (so an empty listed directory weighs 0);
  an unlisted one weighs 1.
- **`FileItem : Item`** — owns an optional `FileRep`, the probe trace that explains
  how the rep was chosen, a per-item open-fork count, and the data/resource
  write-access refnums. Format-specific behavior is entirely in the rep.

### `ItemCache`

`ItemCache` owns the object model and all CNID policy. Its state is:

- `items_`: `cnid -> weak_ptr<Item>`, the realization registry. An `Item`'s destructor
  removes its entry.
- an LRU of realized items (files and directories) with a total weight budget; see
  *Memory bound*.
- the `CNIDMapper` and the root path/factory references.

Its policy entry points are the per-directory diff (`mapDirectoryContents`),
get-or-create (`resolveChildCNID`), batch naming (`nameGroup`), recursive delete
(`deleteCNID`), move (`moveCNID`), realization (`realizeByCNID`,
`realizeByHostPath`, `realizeDescriptor`), and the File Manager's delete/rename/move
operations. Compound read-modify-writes run inside one backend transaction.

### `FileRep` and `OpenFile`

A `FileRep` is the file's storage-format concern; it is constructed lazily and owned
by a `FileItem`, and it decides where each fork lives on the host. Callers see only
a data fork and a resource fork:

- `open(perm)` / `openRF(perm)` return the data and resource forks as `OpenFile`
  objects.
- `getInfo(info)` / `setInfo(info)` read and write Finder metadata.
- `nameSource(filename)` is the filename the displayed Mac name is derived from. The
  default is the host filename; MacBinary strips a trailing `.bin`.
- `hostName(macFilename)` is the inverse of `nameSource`, used when a rename or move
  must encode a guest name back into a host name.
- `remove()` / `move(newPath)` mutate the host files the format owns; the default is a
  single host file, while the sidecar formats also touch their companions.
- `checkTargetFree(newPath)` rejects a move onto a host path occupied by a different
  file; a case-only rename on a case-insensitive host, or a rename onto a hard link of
  the same file, is allowed.

`OpenFile` is the fork interface (`getEOF`, `setEOF`, `read`, `write`, `canWrite`).
Fork opens go through `FileItem::open`/`openRF`, which wrap the result in a
`ForkGuard` so the item's open-fork count follows the handle's lifetime. Host open,
read, and write failures are translated by `hostError(errno)` into Mac errors
(`dskFulErr` for `ENOSPC`/`EDQUOT`/`EFBIG`, `wPrErr` for `EROFS`, `permErr` for
`EACCES`/`EPERM`, `fnfErr` for `ENOENT`, otherwise `ioErr`).

### `CNIDMapper`

A per-volume value store. It provides records and two parent-keyed indexes, plus
`withTransaction(std::function<void()>)` as its only policy-shaped operation.
`ItemCache` composes reconciliation, naming, delete, and move from these primitives;
the backend never decides what a record means.

- **Record**: `{ parID, posixName, macName?, identity, kind }`. The host path is
  derived, never stored. `macName` is absent until naming decides it.
- **Indexes**: `parID -> { posixName -> cnid }` (the directory's child set and the
  `(parID, posixName)` lookup) and `parID -> { folded macName -> cnid }` (name lookup).
  There is deliberately no inode-keyed index; inode pairing for rename recovery is
  built from a scan on demand.
- **Primitives**: `get`, `getChild`, `getChildren`, `getName`; `put`, `erase`,
  `putChild`, `eraseChild`, `putName`, `eraseName`, `allocate`.
- **Transaction seam**: the in-memory backend runs the callback directly; LMDB opens
  one write transaction. The callback must be re-runnable, because LMDB retries it
  after growing the map or adopting another process's resize.

`SimpleCNIDMapper` implements the same interface with in-memory maps. It is the
reference backend, the runtime fallback, and the backend used by the policy tests.

## States and stages

These are optional, orthogonal states of an object, not a pipeline. List implies
Count (and CNIDResolved for every descriptor it stores); Realize is a cache event,
not a state.

| Object | State | Meaning |
|---|---|---|
| `DirectoryItem` | Instantiated | identity only |
| `DirectoryItem` | Counted | visible-child count known; no descriptors retained |
| `DirectoryItem` | Listed | descriptor list stored; implies Counted |
| `Item` | CNIDResolved | a store record exists for it |
| `Item` | Named | its `macName` has been decided |
| `FileItem` | Materialized | it has a `FileRep` |

- **Count** (`DirectoryItem::countVisible`) reads a directory once, filters hidden
  names, and keeps a tally plus a stamp. It performs no CNID work and constructs no
  items.
- **List** (`DirectoryItem::ensureListed` → `listDirectory`) reads a directory once,
  stats each entry, runs the CNID diff, and stores the descriptor list plus a stamp.
  It creates no child items; descriptors are `(posixName, kind, prefix, cnid)`.
- **CNIDResolve** (`ItemCache::resolveChildCNID`) gives an entry a CNID and `parID` by
  looking up its `(parID, posixName)` binding or creating a record.
- **NameResolve** (`DirectoryItem::resolveGroup` → `ItemCache::nameGroup`) decides the
  Mac names of one `macNamePrefix` group.
- **Materialize** (`FileItem::ensureMaterialized`) runs the format gauntlet and
  constructs the `FileRep`. It does not read the file's data.
- **Realize** turns a store record into a live `Item`; any record can be dropped and
  rebuilt, because a realized item is a function of its record plus live host state.

## CNID store and identity

CNIDs are `int32_t`; CNID 1 is the parent of the volume root, CNID 2 is the root, and
new CNIDs are allocated monotonically from a persisted counter starting at 3.

The record's `identity` is the `(device, inode)` pair of the host file, with a `valid`
flag. Identity is a verification value, not a lookup key: a CNID denotes a **binding**
(`parID`, `posixName`), so hard links are distinct bindings with distinct CNIDs even
though they share an inode. `sameIdentity()` reports "same" when either side is
invalid, which makes matching fall back to name and kind.

Identity use is decided per filesystem instance (`st_dev`), lazily and cached for the
thread: block-backed local filesystems with stable inodes (ext2/3/4, XFS, Btrfs,
F2FS, ReiserFS, NILFS2, OCFS2, bcachefs) use it; tmpfs, overlayfs, NFS, FUSE and
unknown types fall back to name+kind. A volume may span several devices through
nested or bind mounts. Windows currently exposes no usable identity through the stat
path, so it always uses name+kind.

`--dont-use-inodes` disables identity process-wide; `statPath()` then reports no
usable identity for every file. Records written while it is in effect have an invalid
identity and are re-stamped from a name+kind match on a later reconcile; a usable
identity is never replaced by an unusable one.

### The LMDB backend

There is one LMDB environment per volume under `$ExecutorDirectoryMap` (default
`~/.executor/cnidmap1`). The directory name is readable: `root` for `/`, `drive-C`
for `C:/`, and `path-...` for any other root. The environment stores a schema version
and the next-CNID counter in a `config` database, records in `mappings`, and the two
indexes in `posixNames` and `names`. Records are versioned big-endian byte strings,
not host structs; index keys prefix a 4-byte parent CNID so a directory's children
form one cursor range.

The map starts at 256 MiB (virtual address space) and is grown when the transaction
fills it; `MDB_MAP_RESIZED` from another process is adopted and the operation retried.
If the environment cannot be opened at all, the volume falls back to
`SimpleCNIDMapper` as described above; if it opens but a later operation fails, the
File Manager operation reports `ioErr`. Deleting the database reassigns CNIDs, so
saved `FSSpec` values held by applications become stale. There is no database GC or
compaction; records for paths that are never revisited are not pruned.

## Reconciliation: the per-directory diff

Listing a directory diffs the stored child set against a fresh scan inside one write
transaction. For each stored child, where an occurrence is a scanned entry with the
same identity and kind:

- an occurrence with the same `posixName` and kind → unchanged: keep the CNID and
  the decided Mac name; re-adopt a usable identity if the record had none;
- exactly one occurrence at a new name → **same-directory rename**: keep the CNID,
  adopt the new `posixName`, clear the Mac name (it is re-derived), update the index;
- no occurrence → gone: delete the record recursively;
- several unmatched occurrences → delete the old record; each occurrence becomes a
  new entry.

Unmatched scanned entries become new records with fresh CNIDs. The rules are
order-independent; unmatched records are processed in CNID order so the result does
not depend on scan order. Index entries are erased only while they still map to the
record being removed or re-homed, so a stale record's removal cannot delete a binding
that rename recovery just gave to a survivor in the same pass.

Consequences:

- A **same-directory host rename keeps the CNID**; a delete-and-recreate at the same
  name, a **kind change** at a name, a rename that leaves the inode with several names
  at once, or a move to another directory is a new CNID.
- A **cross-directory host move** has no rename detection: the old directory's diff
  deletes the record and the new directory's diff creates a new one. A move issued
  through the File Manager keeps the CNID, because it updates the record explicitly.
- Recovered renames update live `Item`s in place (path re-derived, Mac name cleared),
  so an open FCB survives a host rename. Because a record's path is derived from its
  parent chain, renaming a directory changes no descendant record.
- With no usable inode, matching is by name and kind: rename recovery is off, and a
  same-name replacement is not detected.
- A failed scan is never committed as an empty listing. A readdir failure or a
  per-entry `stat` failure aborts the listing; the old listing is dropped and the
  operation reports `ioErr`.
- Concurrent modification during a scan is unguarded. `readdir` may skip or repeat
  entries while the directory changes, so a torn scan can read as a delete-plus-create
  and renumber an entry.

## Naming

The displayed Mac name is derived from the host basename by the winning rep's
`nameSource()`: the host filename for every format except MacBinary, which drops a
trailing `.bin` (so `foo.bin` appears as `foo`). A rename or move asks the rep for the
inverse (`hostName()`), so a MacBinary item named `bar` is stored as `bar.bin`.
Content-derived names (the filename embedded in a container header) are never used.

Naming is batch-only and group-scoped. While listing, each descriptor carries a
case-folded grouping key:

```
hostGroupPrefix(posixName) =
    truncate26(stripAll(toMacRomanFilename(posixName, 0)))
```

- `toMacRomanFilename(posixName, 0)` is the same host→Mac filename conversion used
  for the final name (it maps `:` to `/` and degrades unrepresentable bytes to `?`).
- `stripAll` repeatedly removes a trailing `.bin` and a trailing `#<digits>`.
- `truncate26` leaves room in a 31-byte candidate for the ellipsis that marks
  truncation and a `#` plus up to three digits. A candidate keeps 30 source bytes
  without a disambiguator, and `30 - 1 - d` with a `d`-digit one, so the 26-byte key
  is always a prefix of every candidate its group can generate; two different groups
  can therefore never mint the same name. Grouping is deliberately conservative:
  over-grouping costs laziness, under-grouping would be a bug.
- The key is case-folded, consistent with the case-insensitive Mac name space.
  Descriptors are sorted by `(prefix, posixName)`, so a group is a contiguous range
  found by binary search; there is no separate group index.

To decide a group (`ItemCache::nameGroup`), each undecided member gets the first
candidate `toMacRomanFilename(nameSource, index)` whose folded form is unused in that
directory, for `index` 0 through 999. The first candidate is the plain name; later ones
get `#1`, `#2`, … disambiguators. Names are sticky per binding: once decided, a name
is not reassigned, and a rename clears it on both the record and the live item. A
request carries its binding, and is dropped if another writer renamed the record after
the listing. Members that would need an index above 999 stay unnamed (and therefore
invisible: no name lookup can reach them, and indexed enumeration stops with `fnfErr`
when it reads their name) rather than minting a name that breaks the grouping bound.

Only `.bin` entries can change their name because of their format, so only they are
materialized during naming; every other entry is named from its host name without
probing. Listing order is `prefix`-major then `posixName`.

## Caching and staleness

All validation is at access; there are no timers. Three scopes are checked
independently.

### Directory listings and counts

A `DirectoryItem` holds an optional count and an optional descriptor list, each with
the `FileStamp` it is valid for. `FileStamp` is `(mtime, size)` in the host's native
resolution (not `last_write_time`, whose one-second resolution would miss same-second
changes, and which on Windows opens the file and so fails on a directory that cannot
be read). `statPath` returns the stamp, the identity, and the kind from one raw stat.

- `ensureListed()` stats the directory; if `listed_` and the stamp matches, the
  descriptors and count are valid (O(1)).
- Otherwise it scans. The scan has two passes over one name vector: `readdir` runs to
  completion first, then each name is `stat`ed. This keeps a directory stream that
  needs several `getdents` fills from spanning the per-entry stats. Entries that are
  neither regular files nor directories are dropped; hidden names are filtered, as are
  names the host cannot address (on Windows: `aux`-style device names and other names
  Win32 path parsing mangles).
- The stamp is taken **before** the scan and stored with the result, so a change
  arriving during the scan makes the next access re-list rather than serving a state
  that was never fully observed.
- A failed scan (readdir or per-entry stat failure) is not committed: the listing is
  dropped and the caller reports `ioErr`.

`countVisible()` returns the descriptor count if the directory is already Listed
(without revalidating the listing), a cached tally if its stamp is valid, or a fresh
readdir tally (hidden and unaddressable names filtered, no stats) otherwise. The count
includes subdirectories as well as files, matching HFS valence. A failed count scan is not
cached, and it does not fail the `PBGetCatInfo` that asked for it: `ioDrNmFls` is
reported as `0` (unknown) instead. Guests enumerate a directory by asking for index
1, 2, … until a call fails, so an error on a directory whose *contents* cannot be
read would be read as "past the last entry" and silently truncate the parent's
listing. Listing the directory itself still reports `ioErr`. Count's listed fast
path deliberately does not revalidate: re-listing to answer a count would defeat
Count, and `ioDrNmFls` is a snapshot in HFS terms anyway.

### Binding checks

Lookups that bypass a listing (access by CNID and host-path resolution) validate the
binding directly: derive the path from the record's `parID` chain and `posixName`,
`stat` it, and compare kind and identity. A match is valid regardless of directory
stamps. A failure deletes nothing: the parent is realized, its listing flushed, the
diff reconciles (recovering a same-directory rename or dropping a gone entry), and the
access retries once. The parent's own binding may be stale, so the retry walks upward
until an ancestor validates or a parent record is missing.

An already-realized item handed out by a listing is trusted as of the last access; its
descriptor came from a stamp-validated scan. Per-entry metadata is never cached:
`getInfo` stats live, and fork metadata is read from the rep when asked.

### Rep staleness

A rep caches parsed state (a MacBinary header, AppleSingle descriptors), so it must be
revalidated against every host file classification consulted, not just the file it
ultimately reads. The gauntlet records a **probe trace** of the facts it relied on:

- `RegularFileObservation(path)` / `NoRegularFileObservation(path)` — candidate
  presence; absent candidates are recorded too, so a sidecar appearing later is
  noticed;
- `UnchangedFileObservation(path, stamp)` — a content check (magic number) was used;
- `AttributeExistsObservation` / `AttributeDoesntExistObservation` — macOS xattrs.

`probeHolds()` re-tests those facts: presence checks re-`stat`, attribute checks
re-read the xattr, and `UnchangedFileObservation` is deliberately **ignored**. The
content that decided the format is trusted, not re-read: a file rewritten in place to
another container format leaves a stale rep until an existence or attribute fact
changes. This is the deliberate tradeoff that keeps rep validation cheap; the
realistic external operations (adding or removing a sidecar) are covered.

`ensureMaterialized()` validates before every `open`/`openRF`/`getInfo`/`setInfo`/
`deleteItem`/`moveHost`. It does not rebuild while a fork is open, so the guest's
handle is never stranded beside a replacement parsed object. A path change (rename or
move) marks the whole trace stale because the candidate set is path-derived, so the
next materialization without an open fork re-runs the gauntlet.

### Memory bound

An LRU over realized items — files and directories alike — bounds memory. The queue is
ordered by use: an item is re-touched when handed out, and a directory again when it is
listed or counted. Its budget is a relative weight, 500,000 units
(`localVolumeCacheBudget()`): one unit for a realized file or an unlisted directory,
while a listed directory weighs its descriptor count. One huge directory can
therefore outweigh many small items.

Eviction drops the LRU's strong reference only. An item no one else holds dies with
its payload; one an external consumer holds keeps its listing or rep. A later List
rebuilds it from the store with the same CNIDs. The root is not pinned: it is warmed
at construction and thereafter behaves like any other item. An open FCB owns its
`FileItem` through a strong reference until the FCB closes; deleting an item drops it
from the LRU and the registry immediately, but open holders keep it alive.

## File formats and classification

Classification is the `ItemFactory` gauntlet run by `FileItem::ensureMaterialized`.
If the path is not a regular file, the gauntlet records that fact and returns a plain
rep; otherwise it records the regular-file fact once and each factory probes for its
format, appending its own facts. The first factory that claims the file produces the
rep; the last factory (`ExtensionItemFactory`, plain) claims any remaining regular
file.

Order matters:

1. **AppleSingle** — first 8 bytes are the AppleSingle magic.
2. **AppleDouble** — a companion exists: `%name` first, then `._name`.
3. **Basilisk** — `.rsrc/name` or `.finf/name` exists.
4. **Mac** (macOS only) — a Finder-info or resource-fork xattr exists.
5. **MacBinary** — a `.bin` file with a valid 128-byte MacBinary header (and, if
   present, a valid CRC).
6. **Plain** (`ExtensionItemFactory`, `defaultItemFactory`) — the fallback.

AppleSingle precedes AppleDouble, so a file whose content is AppleSingle wins over an
adjacent sidecar. MacBinary follows the sidecar/xattr formats, so a `._foo.bin`
companion makes `foo.bin` an AppleDouble file rather than MacBinary.

| Format | Host layout | Metadata source |
|---|---|---|
| Plain | data fork is the raw file; `openRF` is an empty fork | hard-coded `TEXT`/`ttxt` FInfo; no FXInfo |
| AppleSingle | one file: AppleSingle header + descriptor table; data fork entry 1, resource fork entry 2, Finder info entry 9 | Finder-info entry |
| AppleDouble | raw data fork + `%name` or `._name` companion holding Finder info and the resource fork | companion Finder-info entry |
| Basilisk | raw data fork + `.rsrc/name` and/or `.finf/name` | `.finf/name` FInfo |
| Mac | raw data fork + `com.apple.FinderInfo` / `com.apple.ResourceFork` xattrs (macOS) | Finder-info xattr |
| MacBinary | one `.bin` file: 128-byte header, data fork, padded resource fork | header FInfo and dates (`0` falls back to the host) |

- **Hidden names.** `%*`, `._*`, `.rsrc` and `.finf` are filtered by name, regardless
  of whether they contain valid data. `isHidden` is consulted by listings, counts, host
  path resolution, and recursive directory deletion.
- **Ownership.** A file delete or move touches only the companion scheme that won
  classification; an AppleDouble companion in the other scheme is left alone. A
  directory is not rep-classified: its own `._name`/`%name` companions are moved with
  it, and on delete its hidden children are removed first and its own companions
  afterwards.
- **Creation.** New files are created by `defaultItemFactory`, i.e. as plain files.
  AppleSingle and MacBinary are not the default creation path and are never used as
  upgrade targets.

### Format upgrades

A plain item cannot store metadata beyond its hard-coded FInfo, and its empty resource
fork cannot be written. When `setInfo` asks for another type, or a write/setEOF hits
the empty resource fork, `PlainRep`/`EmptyFork` signal `UpgradeRequiredException`.

`LocalVolume::upgradeItem` then:

1. creates the richer storage beside the existing data fork (AppleDouble on Unix, Mac
   xattrs on macOS), which is also the write-access test for the primary file;
2. asks the upgrade factory to confirm it now claims the file;
3. stashes the old fork objects without destroying them;
4. swaps the rep and verifies it is no longer plain;
5. opens all replacement forks for the affected FCBs;
6. commits the new FCB state.

The upgrade targets only *add* storage, so the host file keeps its inode and the item
keeps its CNID. Any failure restores the old forks and reports a File Manager error
(via `OSErrorException`) instead of leaving an FCB with a missing handle.

## File Manager interface

### Resolution

- **By name** (`resolve`, `resolveRelative`): Mac paths are colon-separated; `resolve`
  handles the volume-name special case, and `resolveRelative` walks components,
  treating an empty component as the parent. Each component is looked up through the
  directory-side `child(name)` funnel.
- **By CNID** (`resolveDir` → `realizeByCNID`): validate the binding (see above),
  returning the registry's live item or building a new one; on a stale binding,
  re-list the parent and retry once.
- **By index** (`resolveForInfo` with a positive index): the parent is Listed, then the
  descriptor is selected and realized; the name stays undecided until read.
- **By host path** (`realizeByHostPath`, via `nativePathToFSSpec`): walk components
  stat-first, CNIDResolve each one, and realize it. No ancestor is listed and the walk
  does not name the target; the target's Mac name is decided lazily on first read.
  Hidden host paths are not addressable.

`DirectoryItem::child(name)` is the lookup funnel: it first consults the Mac-name
index, then ensures the directory is Listed, NameResolves the target's group, and
looks the name up again. Because realizing may recover a rename, it re-checks the name
after realization. `child(index, includeDirectories)` selects from the descriptor list
and leaves naming lazy.

The naming funnel is `Item::name()` → `ItemCache::ensureNamed` →
`DirectoryItem::resolveName`, which lists the parent if necessary and NameResolves the
group holding the item. Every read of a Mac name goes through it, so a caller never
observes an undecided name; a name that cannot be decided reports `fnfErr` or `ioErr`.

### Metadata

`getInfoCommon` fills `CInfoPBRec`:

- **Files**: fork sizes come from opening both forks read-only and asking their EOF,
  falling back to the stat size for the data fork and `0` for the resource fork when a
  fork cannot be opened; FInfo/FXInfo come from the rep (zeroed when the format's own
  metadata cannot be read); modification date is the host modification time shifted to
  the Mac epoch. Creation date is not stored and reports `0`; MacBinary can supply both
  dates from its header. None of these failures fail the call — see *Directory
  listings and counts* for why.
- **Directories**: `ioDrNmFls` is `countVisible()` (files and subdirectories); the
  modification date is the host directory mtime, taken from the raw `stat` (not
  `last_write_time`, which on Windows opens the directory); Finder info fields are
  zeroed. A directory whose contents cannot be read reports an unknown count (`0`)
  rather than failing the call (see *Directory listings and counts*).
- File attributes and the open-fork refnum come from the FCB table.

`setInfoCommon` writes FInfo/FXInfo and the dates through the rep. A plain rep that is
asked to store a non-`TEXT` type triggers the upgrade described above; a failure is
reported as an ordinary File Manager error.

### Fork I/O and FCBs

`LocalVolume::openCommon` resolves the item, checks the per-fork write-access refnum
(a second writer gets `opWrErr`), opens the requested fork, and allocates a host-side
`FCBExtension`. It fills the guest FCB with the CNID (`fcbFlNum`), parent (`fcbDirID`),
and Mac name, and sets the write bit from the fork's `canWrite()`. The extension
remembers the guest FCB pointer, its refnum, the `OpenFile`, and the owning
`FileItem`; extensions are indexed by `(refNum - 2) / 94`. The resource-fork bit in
the guest FCB's `fcbMdRByt` selects which fork an open FCB refers to.

`PBRead`/`PBWrite` implement positioning and fork I/O; a short read reports `eofErr`,
and a write whose rep refuses with `UpgradeRequiredException` upgrades the item and
retries once. `PBClose` clears the write-access marker and the extension. `PBSetEOF`,
`PBGetFPos`, and `PBSetFPos` operate on the open fork. `PBOpenWD` records a working
directory for the volume.

### Create, delete, rename, move

- **Create**: split a colon path at the last component, resolve the parent, reject an
  existing guest name, and also reject a host path already occupied under the default
  plain encoding (a MacBinary item named `bar` lives at `bar.bin`, so creating
  `bar.bin` must not open it). The parent listing is then dropped so the next access
  re-lists and assigns the new record. Directory creation creates the host directory
  and returns the new directory's CNID.
- **Delete**: a file deletes the host files its rep owns; a directory with any visible
  child reports `fBsyErr` without touching anything, while an apparently empty
  directory removes its hidden sidecars (and its own companions) before removing the
  directory. A scan failure reports `ioErr` before anything is removed. `ItemCache`
  then drops the item from the registry/LRU, deletes the record recursively, and
  flushes the parent listing.
- **Rename**: reject `:`; reject an existing guest name unless it resolves to the same
  item (allowing a case-only rename); encode the host name through the rep
  (`hostName`), let the rep check the host destination, perform the host operation,
  then update the store binding. Open FCB names are refreshed.
- **Move** (`PBCatMove`): `ioNewDirID` is the destination directory and `ioNewName`
  is the new name (nil keeps the current one); the same guest-name and host-path
  checks apply. The host move runs first, then the record is re-homed with the same
  CNID and the old/new parent listings are flushed. Open FCBs get the new parent and
  name.

A create, delete, rename, or move updates the host first and the CNID store second
(create has no record to update, and simply drops the parent listing). A tear is
tolerated because the next listing's diff reconciles it: a same-directory rename by
inode keeps its CNID, while a cross-directory move becomes delete + create.

### Host path to FSSpec

`nativePathToFSSpec` canonicalizes the input (following symlinks), requires the result
to lie under the volume root, resolves it through `realizeByHostPath`, and requires a
decided, non-empty Mac name. It returns `nullopt` for a path outside the volume, a
hidden sidecar, an undecidable name, or any CNID-backend failure. A path that is not
under this volume is simply not matched by this volume.

### Invalidation

| Event | Action |
|---|---|
| create / dir-create | host operation, then clear the parent's listing; the next access re-lists and creates the record |
| delete | delete the host files, drop the item from the registry/LRU, delete the record recursively, flush the parent |
| rename / move (File Manager) | update the record's parent/name in place, flush the old and new parent listings |
| external change | the next listing's stamp check re-lists; the diff renames, deletes, or creates records; a failed binding check flushes the parent and retries |
| external sidecar add/remove | the rep's probe trace fails, so the next materialization re-runs the gauntlet |

### Error vocabulary

`LocalVolume` reports problems as `OSErrorException` (a Mac `OSErr`). The trap
boundary translates `boost::filesystem::filesystem_error` (missing file →
`fnfErr`, permission → `permErr`, otherwise `ioErr`), `OSErrorException` itself, and
any remaining `std::runtime_error` (LMDB failures, corrupt records) to `ioErr`. Host
I/O failures inside reps use `hostError(errno)` so that `ENOSPC`/`EDQUOT`/`EFBIG`,
`EROFS`, `EACCES`/`EPERM`, and `ENOENT` keep their specific Mac meanings.

## Filesystem contracts and limitations

- **Symlinks are followed transparently and are not items.** A link to a file is that
  file (its rep is chosen from the target), a link to a directory is that directory,
  and identity is the target's inode. A link whose target escapes the volume root is
  still listed and followed like any other entry; however, host-path resolution
  canonicalizes first, so such a path is not addressable through
  `nativePathToFSSpec` when the root is not `/`.
- **Non-regular, non-directory entries** (sockets, FIFOs, devices) are dropped from
  listings. `countVisible` does not stat, so a count can include them where a listing
  would not.
- **An unreadable directory still reports metadata.** Its `ioDrNmFls` is `0` until a
  scan succeeds, and enumerating it (or resolving a child inside it) reports `ioErr`.
  Failing the directory's own `PBGetCatInfo` would abort a guest's enumeration of the
  *parent*, which cannot tell that error from "past the last entry" (`packages/go`'s
  Browser is one such guest). Its dates come from the raw `stat`, so they do not
  depend on the contents being readable.
- **An unreadable file still reports metadata.** Its dates and data-fork size come
  from the stat, the resource fork is reported as empty, and Finder info that lives
  inside the file's format (MacBinary, AppleSingle, AppleDouble) is zeroed when it
  cannot be read. Opening the file for actual I/O still reports the host error, as
  does the same for a name the narrow Windows path encoding cannot open.
- **The Mac name space is case-insensitive**, matching the folded name index; host
  names that differ only by case are disambiguated with `#N`.
- **Host changes are observed as of the last access.** A change racing the guest's
  other traps is not transactional; a torn scan can renumber entries.
- **Content changes are trusted.** A file rewritten in place to a different container
  format keeps its old rep until an existence or attribute fact changes (see *Rep
  staleness*).
- **Cross-directory host moves are delete + create** (new CNID); only a File Manager
  move preserves it.
- **No CNID database GC.** Records for directories that are never revisited remain.
- **Names Win32 cannot address are not listed** (Windows only). A `Z:`-style
  volume over a Unix filesystem can contain names that Win32 path parsing remaps:
  device names (`aux`, `nul`, `com1`, … with or without an extension), names ending
  in a dot or space, and names carrying `< > : " | ? *` or control characters. They
  are returned by `readdir` but cannot be `stat`ed through a full path, so List and
  Count filter them out (see *Directory listings and counts*). On a POSIX host every
  name is addressable and nothing is filtered.
- **Windows** exposes no identity (name+kind matching), and the LMDB record encoding
  uses `fs::path::string()`, which is ANSI-code-page based; a MacRoman name the code
  page cannot represent is reported as `ioErr` or stored as `?`. Persistence needs a
  UTF-8 or native-wide encoding.
- **A corrupt parent chain** that cycles indirectly (`A.parID = B`, `B.parID = A`)
  makes path derivation loop; only a self-cycle is guarded.
- **A very deep root** whose sanitized database directory name exceeds `NAME_MAX`
  cannot create its LMDB environment; the volume then runs with in-memory CNIDs and
  no persistence.
- **Rename/move/create host-path checks** treat a failed `stat` as "destination
  free"; the following host call reports the real error, and directory moves do not
  run the rep's host-path check at all (only the guest-name check applies).
- **At most 1,000 names per prefix group.** Entries that would need a disambiguator
  above `#999` stay unnamed and invisible, while still counting toward `ioDrNmFls`.
- Changing how the volume root is chosen and mounting behavior, and the HFS backend,
  are outside this subsystem.

## Important data structures

| Structure | Contents |
|---|---|
| `Item` | `itemcache_`, `parID_`, `cnid_`, `path_`, optional decided `name_` |
| `DirectoryItem` | `descriptors_` sorted by `(prefix, posixName)`, `listed_`, `stamp_`, optional `count_` + `countStamp_` |
| `Descriptor` | `posixName`, `isDir`, case-folded `prefix` (group/sort key), `cnid` |
| `FileItem` | optional `FileRep`, probe trace, probe-stale flag, open-fork count, write-access refnums |
| `FileRep` / `OpenFile` | format operations and fork handle interface |
| `CNIDMapper::Mapping` (`CNIDRecord`) | `parID`, `cnid`, `posixName`, optional `macName`, `identity`, `isDir` |
| `FileStamp` | `(mtime, size)` plus a valid flag |
| `FileIdentity` | `(device, inode)` plus a valid flag |
| `PathStat` | stamp, identity, kind, existence, and a stat error code |
| `ProbeObservation` / `ProbeTrace` | one recorded classification fact / the list the gauntlet left |
| `ItemInfo` | FInfo/FXInfo (or DInfo/DXInfo) union plus Mac-epoch modification and creation times |
| `LocalVolume::FCBExtension` | guest FCB pointer, refnum, `OpenFile`, owning `FileItem` |
| `LocalVolume::NonexistentFile` | resolved parent directory plus the name to create |

`ItemCache` additionally owns the per-directory diff inputs `DirEntry`
(`posixName`, identity, kind) and `NameRequest` (`cnid`, binding `posixName`,
`nameSource`).

## Source files

| Path | Description |
|------|-------------|
| `src/file/localvolume/localvolume.h` / `.cpp` | `LocalVolume`, mount-time backend selection, File Manager operations, FCB extensions |
| `src/file/localvolume/itemcache.h` / `.cpp` | `ItemCache` — realization, CNID policy, diff, naming, delete/move, LRU |
| `src/file/localvolume/item.h` / `.cpp` | `Item`, `DirectoryItem`, `FileItem`, `FileRep`, `ItemFactory`, probe vocabulary |
| `src/file/localvolume/cnidmapper.h` | `CNIDMapper` interface and record type |
| `src/file/localvolume/lmdbcnidmapper.h` / `.cpp` | Persistent LMDB backend |
| `src/file/localvolume/simplecnidmapper.h` / `.cpp` | In-memory reference/fallback backend |
| `src/file/localvolume/filestamp.h` / `.cpp` | `statPath`, `FileStamp`, `FileIdentity`, identity-policy classification |
| `src/file/localvolume/filenames.h` / `.cpp` | `isAddressableFilename`: whether a host name can be addressed (Win32 device names, mangled characters) |
| `src/file/localvolume/openfile.h` | `OpenFile`, `EmptyFork`, `hostError`, `UpgradeRequiredException` |
| `src/file/localvolume/plain.h` / `.cpp` | Plain files |
| `src/file/localvolume/appledouble.h` / `.cpp` | AppleDouble and AppleSingle |
| `src/file/localvolume/basilisk.h` / `.cpp` | Basilisk II `.rsrc`/`.finf` layout |
| `src/file/localvolume/macbinary.h` / `.cpp` | MacBinary detection and forks |
| `src/file/localvolume/mac.h` / `.cpp` | macOS xattr metadata and resource fork (macOS only) |
| `src/file/localvolume/stats.h` | Optional counters and the LRU budget (used by tests) |

## Testing

The native test suite exercises the subsystem end to end:

- `tests/localvolume_ops.cpp` drives the File Manager against the mounted `/`
  `LocalVolume` and interleaves host-side operations; `tests/localvolume_test_util.h`
  provides the fixture and `fm*` helpers.
- `tests/localvolume_itemcache.cpp` drives `ItemCache`'s CNID policy through its
  public API with the in-memory backend.
- `tests/localvolume_cnidmap.cpp` covers the mapper contract, LMDB persistence, and
  transaction rollback.
- `tests/localvolume_macbinary.cpp` covers detection and fork I/O for that format.

Tests that need inode identity or working permission enforcement detect when the host
file system cannot provide them and skip. The realization cache exposes
`resetLocalVolumeStats()` and `localVolumeCacheBudget()` so tests can observe
"enumerate once", count reuse, and forced eviction without timing assumptions. A
disabled large-directory micro-benchmark can be run explicitly.
