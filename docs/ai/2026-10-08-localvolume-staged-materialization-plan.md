# Plan: staged materialization for `LocalVolume`

> **AI-generated.** This document was produced with the assistance of an AI
> language model and may contain inaccuracies. It is a *plan*, not a record of
> implemented behaviour.

- **Date:** 2026-10-08
- **Status:** Phases 0–3 and 5 done; Phase 4 sub-step 1 done (rep naming rule); Phase 4 sub-steps 2–4 and Phases 6–7 planned
- **Area:** `src/file/localvolume/`
- **Related:** `docs/subsystems/local-volume.md`,
  `docs/ai/2026-10-04-macbinary-localvolume-backend.md`

## Summary

Two performance problems in `LocalVolume` share one architectural cause: an
`Item` is all-or-nothing (identity *and* file format), and an `ItemCache`
directory is either "cached" (fully enumerated and every child realized) or "not
cached". Assigning a CNID, reporting a directory's item count, and listing a
directory all collapse onto one expensive "fully cache the whole directory"
operation.

This plan separates the stages that have been conflated. They are optional,
orthogonal **states of an object**, not a pipeline:

| Object | State | Meaning |
|---|---|---|
| DirectoryItem | Instantiated | identity only |
| DirectoryItem | **Counted** | its visible-child count is known (one readdir; no descriptors kept) |
| DirectoryItem | **Listed** | its children are described by a stored descriptor list; implies Counted |
| Item | **CNIDResolved** | it has a `CNID` (a mapper record exists) |
| Item | **Named** | its `macName` has been decided |
| FileItem | **Materialized** | it has a `FileRep` |

The model is **link-free**: an `Item` owns no other `Item`; a `DirectoryItem`
holds a descriptor list rather than child `Item`s, identity lives in the
`CNIDMapper`, and realized `Item`s are kept alive only by an LRU while in use.
The file-format concern is **split out of `Item`** (into a `FileRep`), so an
`Item` can exist without knowing its format, and **a `CNID` is keyed by the host
file's inode**, not by name. The headline fixes are that **path resolution must
not enumerate any directory** and that **reporting a count must not enumerate or
materialize the directory's contents**.

## Findings (motivation)

### F1. Path resolution walks and fully caches every ancestor

`Executor::cmdlinePathToFSSpec` routes a host path (no `:`) to
`Executor::nativePathToFSSpec` (`src/file/fileMisc.cpp:245-251`), which calls
`LocalVolume::nativePathToFSSpec` (`src/file/localvolume/localvolume.cpp:867`).
That canonicalises the path (following the `result` symlink into `/nix/store/…`)
and calls `ItemCache::tryResolve(fs::path)` (`itemcache.cpp:169`).

`tryResolve(path)` walks the path component by component from the volume root and
calls `cacheDirectory(dir)` on **every ancestor**. Each `cacheDirectory`
(`itemcache.cpp:42`) does:

- one `directory_iterator` over the directory;
- `SimpleCNIDMapper::mapDirectoryContents` (`simplecnidmapper.cpp:18`) — sorts
  all entries and runs the batch CNID diff;
- `LocalVolume::createItemForDirEntry` (`localvolume.cpp:96`) for **every
  entry**, probing all factories: `fs::is_directory` / `fs::is_regular_file`
  stats, AppleSingle 8-byte magic read (`appledouble.cpp:117`), AppleDouble
  sidecar stats (`appledouble.cpp:30`), Basilisk `.rsrc`/`.finf` stats
  (`basilisk.cpp:16`), MacBinary 128-byte read + CRC for `.bin`
  (`macbinary.cpp:430`), then the plain fallback;
- `DirectoryItem::populateCache` (`item.cpp:67`) — upper-cases every name and
  inserts into an ordered `std::map`.

For `~/Projects/Executor/result/m68k/Dialog.bin` the canonical path is
`/nix/store/4ikmp1…-retro68.samples/Dialog.bin`, so the ancestors cached are
`/`, `/nix`, `/nix/store`, the hash directory, and `m68k`. `/nix/store` has
**296,240 entries** on this machine.

The volume root is `/` itself (`MountLocalVolumes`, `localvolume.cpp:944`), which
is what makes `/nix/store` reachable and subject to this.

### F2. Scope of the launch-time enumeration (does it recurse?)

No. Caching `/nix/store` enumerates **one level** and constructs an `Item` (CNID
+ Mac name) for each of its ~296,240 entries. A subdirectory entry becomes a
`DirectoryItem` (`item.cpp:49-57`) whose constructor only stores its fields;
`contents_` stays empty and `cache_valid_` stays `false`. Nothing recursively
calls `cacheDirectory` on the children. So launch cost is
O(entries in `/nix/store`), not O(entries in the whole subtree). The
subdirectories are enumerated only if/when the guest later navigates into them.

### F3. Reporting a directory's count forces full realization

`LocalVolume::getInfoCommon` (`localvolume.cpp:436`) reports the count as:

```cpp
itemCache->cacheDirectory(dirItem);
pb->dirInfo.ioDrNmFls = dirItem->countItems();   // == contents_.size()
```

`ioDrNmFls` can only be read from the fully materialized `contents_`
(`item.h:113`), so *asking for a count* costs the whole `cacheDirectory` path
above. Note this counts subdirectories as well as files, which matches HFS
`dirVal` (`hfsBtree.cpp:1625-1668` adjusts `dirVal` for both `regular` and
`directory` kinds) — so the count semantics are intentional and must be
preserved.

Consequence: when the guest lists a folder and asks each displayed subdirectory
for its `ioDrNmFls`, every one of those subdirectories is fully realized. That is
the "must cache every sub-directory to report the number of sub-items" problem.

### F4. The cache policy is a band-aid

`cleanDirectoryCache` (`itemcache.cpp:26`) evicts a directory after **1 second**
or when more than ~20 are cached. A directory larger than what can be built in
1 s is therefore rebuilt repeatedly during a single listing session — the exact
cost the LRU was meant to bound.

### F5. Mac names are batch-assigned and factories cannot rename

Names are chosen by the CNID mappers from the host filename during batch
enumeration (`simplecnidmapper.cpp:59-70`; LMDB equivalent
`lmdbcnidmapper.cpp:223-228`), with `#N` suffixed by `toMacRomanFilename(name,
index)` (`simplecnidmapper.cpp:66`) for collisions. `ItemCache::cacheDirectory`
hands the finished name to the factory (`itemcache.cpp:85`), so a factory cannot
override it. Content-derived names (an embedded filename in an AppleDouble /
AppleSingle / MacBinary header) are not used. See
`docs/ai/2026-10-04-macbinary-localvolume-backend.md`, Decision 1.

## Root cause

`ItemCache` / `DirectoryItem` fuse independent concerns:

| Concern | Currently requires |
|---|---|
| Host path → CNID | materializing every ancestor directory |
| Directory item count (`ioDrNmFls`) | materializing that directory |
| Directory listing | materializing that directory |
| Mac name | materializing that directory (batch) |

and `CNIDMapper` is batch-shaped (`mapDirectoryContents(dirID, allEntries)`)
because batch mode is *possible* only as a consequence of always caching a
directory whole. **Whole-directory batching is an artifact** — assigning a single
entry a CNID needs only its parent id and its host path. But **batching by shared
`macNamePrefix` is required**: disambiguators must be assigned consistently for
the whole group, and assignment must query the mapper for the disambiguators it
has already handed out. The mapper stores each mapping's host path and reuses it
across enumerations, so identity is *today* path-keyed; the enumeration
dependency lives in `ItemCache::tryResolve(fs::path)` (`itemcache.cpp:169`),
which has no path → CNID index and must enumerate each ancestor to find the child.

## Decisions

Decisions marked **[revised]** changed while folding in the link-free object
model; those marked **[revisit]** may need a further decision before their phase.

1. **Content-derived names are dead.** No name is ever read from an
   AppleDouble/AppleSingle/MacBinary header. All names are transformations of the
   host basename.
2. **Sidecars are always hidden by name**, independent of whether they are
   valid. `._`/`%` (AppleDouble) and `.rsrc`/`.finf` (Basilisk) are filtered
   purely from `readdir` output; no validity check is involved.
3. **[revised] Identity is keyed by the host inode, not by name.** A `CNID`
   follows the *file*: a rename keeps it, and a delete-and-recreate at the same
   name gets a new one. The mapper assigns the `CNID` (and its `parID`) at
   **CNIDResolve**; the `macName` is decided separately, at **NameResolve**, and
   is therefore optional until then. `macName` never determines the `CNID`.
4. **Naming avoids known conflicts.** Deciding a name assigns a `macName` that
   does not collide with any name already assigned in that directory. **[revisit]**:
   confirm the "known" set (the group's host names plus the directory's assigned
   names) is the intended scope.
5. **[revised] `macNamePrefix` is a conservative grouping key** (see *Names*).
   Over-grouping costs only laziness; under-grouping would be a bug. The group is
   the contiguous run of equal prefixes in a prefix-sorted descriptor list, found
   by binary search (no separate group index).
7. **Caching is validation-based**, not timer-based (see *Caching*).
8. **Re-enumeration must not renumber or rename existing entries.** The staged
   design must preserve this. **[revisit]**: the reconciliation match key becomes
   the inode, not the path, so a delete-and-recreate at an existing name is now a
   *new* entry by design (Decision 3), not a reused `(parID, posixName)` identity.
9. **[revised] Identity creation and naming are transactions.** The per-directory
   diff and a **NameResolve** over one group are each one atomic
   read-modify-write, so concurrent writers converge. **[revisit]**: the batch is
   now the prefix group, not the whole directory.
10. **Identity is split from format.** `Item` may be instantiated without knowing
    its file format; the format lives in a `FileRep` (see *Object model*).
11. **[revised] Catalog order is `macNamePrefix`-major, host path as the
    tie-break.** A `Listed` directory's descriptors are sorted by
    `(macNamePrefix, posixName)`, and that is the listing order. (Was: host-path
    sort order.) Revisit if this proves objectionable to a Mac user.
12. **delete/rename/move update the mapper and flush the whole directory.**
    `deleteCNID`/`moveCNID` keep the CNID mapping correct; the `ItemCache` flush
    stays directory-wide. Finer-grained invalidation is deferred (*Future Ideas*).
13. **[revised] Count and List are separate directory stages.** Count keeps only
    a tally (no descriptors, no mapper work); List stores the descriptor list and
    yields the count for free (see *Stage model*). Counts vastly outnumber
    Listings (`ioDrNmFls` is requested for every directory a listing shows), and
    List costs memory, so the two must not be conflated.
14. **[revised] Staleness is validation-based, in three scopes** (see *Caching*):
    directory contents and file representation use `(mtime, size)` from one raw
    `stat`; identity uses `(dev, ino)`. External changes are reconciled at access,
    not on a timer.
15. **Items are link-free.** No `Item` owns another; a `DirectoryItem` stores a
    descriptor list, not child `Item`s. Identity lives in the `CNIDMapper`, and
    realized `Item`s are retained only by an LRU (see *Object model*).

## Names

### `macNamePrefix`

While listing a directory we know a grouping key, **not** the name:

```
macNamePrefix = truncate(30 - maxDisambiguatorDigits)(
                    stripDisambiguator(
                        stripBin(
                            toMacRoman(first31(posixName)))))
```

- `toMacRoman(x)` — raw conversion to MacRoman (not `toMacRomanFilename`'s
  truncation logic).
- `stripBin` — drop a trailing `.bin`, so a MacBinary `foo.bin` groups with a
  plain `foo`.
- `stripDisambiguator` — drop a trailing `#<digits>`.
- `maxDisambiguatorDigits = 3`; truncate to `30 - 3 = 27` bytes to leave room for
  a generated `#999`.

The prefix is computed at enumeration and stored on each descriptor. A `Listed`
directory keeps its descriptors sorted by `(macNamePrefix, posixName)`, so a group
is exactly the contiguous range of descriptors with equal prefix, found by binary
search — no separate group index. Entries in a group are the ones that can
collide.

### The final name

The final Mac name is the raw name or the `.bin`-stripped name, chosen by the
winning rep: MacBinary → stripped; every other rep → raw. The rule belongs to
the rep, not a standalone extension check: a `foo.bin` with a `._foo.bin`
companion is AppleDouble and keeps `foo.bin`. The mapper's `macName` field holds
the chosen name.

## Stage model

These are optional, orthogonal **states** of an object — not a pipeline, with no
implied ordering except where noted: List implies Count, and Realize is not a
state at all (see below).

| Object | State | Meaning |
|---|---|---|
| DirectoryItem | Instantiated | identity only |
| DirectoryItem | **Counted** | its visible-child count is known (one readdir; no descriptors kept) |
| DirectoryItem | **Listed** | its visible children are described by a stored descriptor list; implies Counted |
| Item | **CNIDResolved** | it has a `CNID` (a mapper record exists) |
| Item | **Named** | its `macName` has been decided |
| FileItem | **Materialized** | it has a `FileRep` |

`CNIDResolve` and `NameResolve` are separate, and both are separate from
`Materialize`: an item can be addressed by `CNID` without being named, and named
without its format being known. That is what lets an ancestor be walked without
listing or naming it.

Count and List are separate directory stages. `ioDrNmFls` is queried for *every*
directory a listing displays, so counts vastly outnumber listings; and while List
is only slightly more expensive than Count in *time*, it retains a descriptor per
child and so costs more *memory*. Hence a cheap Count that keeps only a tally, and
a heavier List that keeps the descriptors.

`ioDrNmFls` is a snapshot by nature (HFS keeps a stored valence; the host
directory is live), so a readdir tally is no less correct than counting the same
live directory eagerly — document the staleness bound rather than pretending to
exactness.

Materialize does not read file data — it establishes the item's representation
(which rep, where the forks live, Finder metadata), and does not imply reading a
whole file.

### Count (DirectoryItem)

Read the host directory once, apply the hidden-name filter, and keep only the
tally plus a validation stamp. No descriptors, no CNID-mapper work, no `Item`s —
so a Counted directory costs O(1) memory.

### List (DirectoryItem)

Read the host directory once and store an ordered **descriptor list** — one
`(posixName, kind, macNamePrefix, optional cnid)` per visible child (sidecar names
filtered by name) — plus a validation stamp. The descriptors are sorted by
`(macNamePrefix, posixName)`. The count is `descriptors.size()`: List implies
Count. No child `Item` is created or retained.

### CNIDResolve (Item)

Give an entry a `CNID` (and its `parID`) by looking it up in the mapper — keyed by
`(dev, ino)` — or creating a record on a miss. This needs only the entry's parent
and its host identity, not the parent's listing, which is what lets path
resolution descend without enumerating.

### NameResolve / Resolve (a directory's group)

Decide `macName`s for one `macNamePrefix` group — the contiguous run of equal
prefixes in a `Listed` directory's descriptor list — assigning each a name that
does not collide with names already assigned in that directory. This is the
atomic naming batch (Decision 9). A whole-directory Resolve is the special case
of doing it for every group.

### Materialize (FileItem)

Construct the `FileRep` (the format concern). See *Classify*.

### Classify

For now Classify is just another name for Materialize: we decide the rep by
building it. The two words are kept distinct so they *could* be split later
(*Future Ideas*).

### Realize (a cache event, not a state)

Turning a mapper record into a live `Item` object is not a stage. It is a cache
event driven by the realization LRU (see *Caching*): any record can be realized on
demand and dropped arbitrarily, because a realized `Item` is a pure function of
its mapper record plus live host state.

## Object model

**Items are link-free.** No `Item` owns another `Item`: a `DirectoryItem` stores
descriptors, not children, and the parent→child relation lives entirely in the
`CNIDMapper`. A realized `Item` is held only by the realization LRU or by an
external reference (see *Caching*).

- **`Item`** — identity: `parID`, `cnid`, `path`, and `macName`, plus the identity
  ops (`deleteItem`, `moveItem`). `cnid` is assigned at **CNIDResolve**; `macName`
  is decided separately at **NameResolve** and is therefore *optional* until then
  (the authoritative copy lives in the mapper record). `deleteItem`/`moveItem`
  stay the public API here but delegate the host-file mutation to the rep, so a
  file's format need not be known until it is Materialized.
- **`DirectoryItem : Item`** — the two directory stages and their stamps: an
  optional `count_` (+ stamp), and an optional **descriptor list** sorted by
  `(macNamePrefix, posixName)` with a lazily populated per-descriptor `CNID`
  (one `CNID` per entry once CNIDResolved). No child `Item`s.
- **`FileItem : Item`** — holds an optional `FileRep`.
- **`FileRep`** — the format concern: `open`, `openRF`, `getInfo`, `setInfo`, and
  the host-file mutation `remove`/`move` (default = a single host file; the
  sidecar formats AppleDouble and Basilisk override). Concrete `PlainRep`,
  `AppleDoubleRep`, `AppleSingleRep`, `BasiliskRep`, `MacBinaryRep`, `MacRep`.
  Built at Materialize.

`ItemCache::items_` (`cnid -> weak_ptr<Item>`) is the realization registry, backed
by an LRU that owns the realized `Item`s (files *and* directories). So "find an
already-realized child" is answered there, not by the parent. `upgradeItem`
becomes a `FileRep` swap (it still rewrites the host file into the richer
encoding).

## Caching

Three jobs are conflated in the current LRU + 1 s timeout (`itemcache.cpp:26`):
realizing items, bounding memory, and staying fresh. Split them.

### What a DirectoryItem caches

`DirectoryItem` carries two optional pieces of state, one shared validation
stamp, and a lazily populated per-descriptor CNID:

```cpp
std::optional<int> count_;                  // Counted: a tally only
std::vector<Descriptor> descriptors_;       // Listed: sorted by (macNamePrefix, posixName)
                                            //   posixName, kind, macNamePrefix, optional cnid
bool listed_ = false;
FileStamp stamp_;                           // (mtime, size) at the last scan
```

- `listed_` implies the count (`descriptors_.size()`); `count_` is then dropped.
- `count_` alone means Counted but not Listed.
- Both payloads are valid iff `stat(dir) == stamp_`. When validation fails, *both*
  are dropped before rescanning, so a stale listing is never served merely
  because a fresh count was taken.

### Validation (staleness), independent of wall-clock time

`cacheDirectory` becomes `ensureListed(dir)`:

```
if listed_ and stat(dir) == stamp_: return   # L0, O(1)
relist(dir)                                  # L1
```

- **L0 (O(1))**: `stat` the directory itself; if the stamp is unchanged, the
  descriptors and count are all still valid.
- **L1**: re-readdir and re-run the mapper diff (reusing CNIDs by inode,
  assigning new ones, `deleteCNID`ing children no longer present).

The stamp is `(mtime, size)` taken from **one raw `stat`**, with a before/after
re-stat around the `readdir` so a change racing our own scan is caught (retry on
mismatch). Do **not** use `fs::last_write_time()` for this: this project's `fs`
is Boost.Filesystem, whose `last_write_time()` returns `std::time_t` — one-second
resolution, which would miss same-second changes and make the validation test
flaky.

Counting caches the same way: `Count(dir)` returns `descriptors_.size()` if Listed
and valid, else a cached `count_` if its stamp is valid, else rescans. A repeated
`ioDrNmFls` request is O(1) after the first scan.

### Identity staleness (the third scope)

A directory stamp only runs on a List. The lookups that deliberately bypass a List
— `CNID -> Item` and path resolution — validate the *mapping* directly: `stat` the
record's path and compare `(dev, ino)` and the kind against the record. Match →
valid, whatever `(mtime, size)` say; `ENOENT` → the name is gone; different inode
→ a different file now holds the name. It is nearly free because realization
already stats the path for its kind. A mapping check validates an existing binding
only, never discovers a new entry, so List remains the bulk path and the miss
fallback.

Two limits bound inode identity. **Hard links** (one inode, several names) are not
injective, so `nlink > 1` entries key identity on `(dev, ino, posixName)` (distinct
per name, no rename tracking) — this is why `nlink` is stored — and `/nix/store`
can contain hard-linked files. And some filesystems/mounts supply no usable inode
(`d_ino == 0`), so identity needs a name-keyed fallback. A reused inode (a file
deleted and a new one allocated the same inode before re-enumeration) will match
in the diff and reuse the CNID; a name-keyed diff has the same class of race.

There is deliberately **no per-entry `(filename, mtime)` level**: per-entry
metadata is not cached — `Item::getInfo` stats live, and fork/Finder-info reads
hit the rep live — so the only per-entry staleness is the materialized rep, below.

### Reconciliation of external changes

Reconciliation *is* L1: a host change is observed on the next access to the
affected directory, with no FileManager operation. The mapper matches the fresh
scan against its stored children **by inode**:

- an inode seen again at the same name is unchanged (keeps its CNID and decided
  `macName`);
- an inode seen at a new name is a **rename**; the CNID follows and the name
  updates;
- a scanned inode with no stored record is **new** (new CNID);
- a stored inode absent from the scan is gone (`deleteCNID`ed, recursively).

Consequences:

- An **external rename keeps the CNID** (Decision 3); a delete-and-recreate at the
  same name, or a kind change at a name, is a new CNID (the inode differs).
- **Path resolution** descends by stat + `(dev, ino)` (see *Stage model*,
  CNIDResolve) and never enumerates; a stale binding is validated or recreated.
- **Read errors**: a directory that cannot be read reports `ioErr` from the lookup
  funnel (`ItemCache::child`).
- The staleness bound is "as of the last access". Changes racing the guest's
  other traps are inherently un-transactional (HFS is not transactional either).

CNID changes made cross-instance through LMDB, with no host change, are invisible
to the stamp; that is Phase 6. Identity stability across re-enumeration (CNIDs and
decided names survive for an unchanged entry) is provided by the mapper's inode
match, pinned by `DisambiguatedNamesStableAcrossDelete`,
`MoveKeepsDisambiguatedNameAndPosixName` and `CnidStableAcrossRenameThenMove` —
*not* an open question.

### Materialized-rep staleness

A rep caches parsed state (`MacBinaryFile::header_`,
`AppleSingleDoubleFile::descriptors`) and can outlive the listing that built
it. Record the primary host file's `(mtime, size)` on the `FileItem` at
Materialize; at the start of `open`/`openRF`/`getInfo`/`deleteItem`/`moveItem`,
drop the rep if the stamp changed (it rematerializes). Do not revalidate a rep
whose fork is currently open — the guest holds it, so the change surfaces on
reopen.

### Memory bound

An LRU over realized `Item`s — files *and* directories — bounds memory; eviction
drops the LRU's strong reference (the item then dies unless externally
referenced), and is safe because re-realization rebuilds from the mapper. Charge
**total items** (Σ live `Item`s) rather than directory count — the risk is one
huge directory (e.g. `/nix/store`), not many small ones. Default budget:
**500,000 items**. The root is pinned only as a cheap heuristic (the volume
anchor / `resolveDir(2)`), not because the item graph is rooted anywhere.

Evicting a `DirectoryItem` frees its descriptor list; the mapper record survives,
so a later List rebuilds it with the same CNIDs. Items referenced by an open FCB
survive. This removes the 1 s / 20-directory policy entirely.

## Component changes (sketch)

### `CNIDMapper` (`cnidmapper.h`)

Backing store, per volume (a **dumb value store**; resolving and staleness policy
live in `ItemCache`):

- **record** per CNID: `{ parID, posixName, macName?, mtime, (dev, ino), nlink }`.
  A plain path/name and an explicit `mtime` snapshot, not a cached
  `fs::directory_entry`. `macName` is optional until NameResolve; `(dev, ino)` and
  `nlink` carry identity (Decision 3).
- `(dev, ino) -> cnid` — identity, for stat-first path resolution and rename
  recovery.
- `parID -> {cnid}` — the directory child set: the reconciliation diff and the
  names-in-use set.
- `(parID, macName case-folded) -> cnid` — name lookup without listing.

It deliberately does **not** keep `(parID, posixName) -> cnid`: the only caller
that wanted it is path resolution, which stats each component and uses
`(dev, ino)` instead.

Operations:

- CNIDResolve: look up `(dev, ino)`, or create a record under a parent.
- **Resolve / diff** (the batch form): match the fresh scan against the stored
  children by inode — reuse, rename, create, or delete — then NameResolve the
  affected groups. `lookupChild`/`getOrCreateChild` go away.
- `deleteCNID`/`moveCNID` maintain the three indexes above.
- A group's naming is a read-modify-write; larger operations are handed to the
  mapper as a `std::function` so the transaction shape is backend-neutral
  (Decision 9).

### `ItemCache` / `LocalVolume` (`itemcache.*`, `localvolume.cpp`)

- `ItemCache::tryResolve(fs::path)`: **stat-first** — stat each component, resolve
  `(dev, ino)` via the mapper, and never enumerate an ancestor. *(Phase 1 used a
  `(parID, posixName)` lookup; superseded.)*
- `ItemCache::child(...)`: the lookup funnel — ensure the directory is Listed and
  valid, then select (see *Stage model*).
- `getInfoCommon`'s directory branch: use **Count** (`countChildren`), which keeps
  only a tally.
- `LocalVolume::nativePathToFSSpec` / `resolve(...)`: no ancestor enumeration; the
  name for the FSSpec comes from NameResolve of that one entry's group.
- `resolve(name)`: the parent is Listed (or the name's group NameResolved), then
  the name is looked up via the mapper's `(parID, macName)` index.
- `ensureListed` (L0 stamp check, L1 re-list); an LRU by total items over all
  `Item`s.

### `Item` / `DirectoryItem` / `FileItem` (`item.h`, `item.cpp`)

- `Item`: identity (`parID`, `cnid`, `path`, optional `macName`); instantiable
  without a format; owns no other `Item`.
- `DirectoryItem`: the two stages — `count_`/`stamp_` (Counted) and the
  `(macNamePrefix, posixName)`-sorted descriptor list with a per-descriptor `CNID`
  (Listed) — plus `resolve(index, includeDirectories)`; keep the HFS-compatible
  count semantics (files + dirs).
- `FileItem`: holds the optional `FileRep` and builds it lazily via
  `ensureMaterialized()` (called from `open`/`openRF`/`getInfo`/`setInfo`/
  `deleteItem`/`moveItem`). *(done — Phase 3)*

### `ItemFactory` (per-format files)

- Split "kind" (cheap: regular file vs directory, no probing) from "which fork
  layout" (the gauntlet: magic reads / sidecar stats / xattrs). *(done — Phase 3:
  `ItemCache::makeItem` makes the kind decision; `ItemFactory::createRepForFile`
  runs the gauntlet from `ItemCache::materialize`. `DirectoryItemFactory` is gone.)*
- Give reps the naming rule (MacBinary `.bin` stripping) instead of hard-coding
  the host filename. *(done — Phase 4 sub-step 1: `FileRep::nameSource`.)*

## Phased plan

### Phase 0 — Test harness and baseline — DONE

Built the native harness:

- `tests/localvolume_ops.cpp` + `tests/localvolume_test_util.h`, added to
  `NATIVE_TEST_SOURCES`.
- `src/file/localvolume/stats.h` counters, wired into `itemcache.cpp` /
  `localvolume.cpp` (`directoryIterations`, `entriesSeen`, `itemsConstructed`,
  `factoryProbes`); `resetLocalVolumeStats()` for tests.
- A `DISABLED_` large-directory benchmark.

Baseline (empty-file directory, `EXECUTOR_LV_BENCH_N=N`): listing N files costs
1 directory scan, N items constructed, and **6N** factory probes. At N=20000 the
listing did not finish within 5 minutes — F4 in practice.

### Phase 0 findings

- **`PBCatMove` `ioNewName` semantics.** `pb->ioNewName` is the new *item name*,
  not the destination directory (destination is `ioNewDirID`, confirmed against
  `C_FSpCatMove`, `src/file/fileHighlevel.cpp:351`); `LocalVolume::PBCatMove`
  follows this, and `ItemCache::moveItem` takes an optional `newName`. Covered by
  `MoveAndRename` and `CatMoveRenamesWithinDirectory`.
- **The HFS backend shares the `PBCatMove` shape** — `hfsPBCatMove`
  (`src/hfs/hfsHier.cpp:266-270`) feeds `ioNewName` to `ROMlib_findvcbandfile`.
  Out of scope here.
- **Name disambiguation is stable across deletion**: the mapper reuses mappings
  by path. Covered by `DisambiguatedNamesStableAcrossDelete`.

### Phase 1 — Path resolution without ancestor enumeration (F1) — DONE

`SimpleCNIDMapper` stores per-directory children as `(host filename → cnid)`
(the `(parID, posixName)` lookup + diff index); added `lookupChild` /
`getOrCreateChild`; `ItemCache::tryResolve(path)` walks components through them
instead of `cacheDirectory` on each ancestor. Guard:
`ResolvePathDoesNotEnumerate` (zero directory scans).

### Phase 2 — Count without enumerating (F3) — DONE

`ItemCache::countChildren` (readdir + hidden filter; no CNID/item/factory work),
used for `ioDrNmFls` as the **Count** stage, distinct from Enumerate. Guard:
`ListingDoesNotMaterializeSubdirectories`. Count's validation-stamp caching is
Phase 5.

### Phase 3 — Object model + deferred materialization — DONE

Split the format concern out of `Item` into `FileRep` (Variant B) and defer the
format gauntlet from Enumerate to Materialize. Deliverable: scanning a directory
no longer probes every file's format. **Risk:** wide but mechanical; touched
`item.{h,cpp}`, `itemcache.{h,cpp}`, `localvolume.cpp`, and every format backend.

- **DONE (sub-step 1):** the `FileRep` seam (`open`/`openRF`/`getInfo`/`setInfo`
  plus `remove`/`move`) and the Plain backend.
- **DONE (sub-step 2):** all remaining backends migrated to reps —
  `AppleDoubleRep`, `AppleSingleRep`, `BasiliskRep`, `MacBinaryRep`, `MacRep`.
  `FileItem::deleteItem`/`moveItem` remain the Item-level API but delegate the
  host-file mutation to the rep, so the format decision can be deferred.
- **DONE (sub-step 3, the payoff):** Enumerate makes only the cheap kind
  decision (`ItemCache::makeItem`) and instantiates identity `Item`s; the factory
  gauntlet moved behind `ItemCache::materialize` / `FileItem::ensureMaterialized`.
  `DirectoryItemFactory` was removed and `ItemFactory` now exposes
  `createRepForFile`. Verified with the benchmark: 100 entries →
  `directoryIterations=1, itemsConstructed=100, factoryProbes=500` (probes happen
  only when an entry is actually accessed, not during the scan).

### Phase 4 — Resolve and group-scoped naming

Decide an item's `macName` separately from its identity, at the right time and
at the right granularity. Four sub-steps:

1. **DONE — rep naming rule.** `FileRep::nameSource()` lets the rep decide the
   displayed name (MacBinary drops `.bin`), applied within the existing eager,
   whole-directory naming pass. Covered by `MacBinaryStripsBinExtension` and
   `SidecarNameWinsOverMacBinary`.
2. **Model and responsibility split for lazy Resolve.** Let a name be
   *undecided*; move naming out of the mapper's enumeration into an explicit
   whole-directory **Resolve**. Resolve is still triggered at the end of
   Enumerate, so observable behaviour is unchanged — this step only prepares the
   structure.
3. **Whole-directory lazy Resolve.** Trigger Resolve where names are actually
   needed, not at Enumerate, so that Enumerate alone (count, path-keyed identity)
   never decides names.
4. **Group-scoped Resolve.** Narrow a Resolve to the `macNamePrefix` group that
   can actually conflict, instead of the whole directory.

**Risk:** correctness of lazy / undecided names.

### Phase 5 — Caching redesign — DONE

Replace the LRU + 1 s timeout with validation-based staleness and a memory-only
LRU (see *Caching*). Sub-steps:

1. **DONE — directory stamps + `ensureEnumerated`.** `(mtime, size)` from a raw
   `stat`; L0 validate / L1 re-enumerate; the TTL is gone.
   `HostChangeVisibleWithoutInvalidation` is enabled.
2. **DONE — memory-bound LRU** by total items (default 500 K, root pinned);
   `EvictionPreservesIdentity`.
3. **DONE — Count caching** (`count_` + stamp); `CountScansOnceWhileValid`.
4. **DONE — per-item rep revalidation** on `(mtime, size)` change;
   `RepStalenessRebuilds`.
5. **DONE — reconciliation correctness:** kind changes (`KindChangeRebuildsItem`),
   the read-error policy, external rename = delete + create
   (`ExternalRenameIsDeleteCreate`).

### Phase 6 — CNID persistence, transactions, multi-instance sharing

Re-enable `LMDBCNIDMapper`: persist the `cnid -> record` table and the
`(parID, posixName) -> cnid` index, and make Resolve a **transactional
read-modify-write** (Decision 9). LMDB's single-writer model then lets two
Executor instances share CNID and naming decisions for the same volume.

### Phase 7 — Inode-based rename tracking

Use the stored `(dev, inode)` to keep an item's CNID when its host file is
renamed outside the FileManager (currently an external rename looks like
delete + create). Nothing above depends on this.

## Testing

### Existing infrastructure

- `tests/files.cpp` — dual-mode (native + Retro68); Mac ABI only, no Executor
  internals; `FileTest` creates/deletes one file in the default directory.
- `tests/files_internal.cpp` — native-only; uses `nativePathToFSSpec` /
  `cmdlinePathToFSSpec`.
- `tests/localvolume_macbinary.cpp` — native-only; backend functions, no
  FileManager.
- `tests/main_executor.cpp` — global environment: initialises Executor,
  `ROMlib_fileinit()` (mounts `/`), creates a temp dir opened as the default WD.
- Build split: `NATIVE_TEST_SOURCES` vs `TEST_SOURCES` in `tests/CMakeLists.txt`;
  expected failures are labelled `xfail` in `tests/test-properties.cmake`.
- Suites: `ctest --test-dir build`; the Retro68 Mac-app suite is separate.

### New native test module

`tests/localvolume_ops.cpp` drives the FileManager against the mounted-`/`
`LocalVolume` and interleaves host (`std::filesystem`) operations. Helpers in
`tests/localvolume_test_util.h`:

- `LocalVolumeFixture` — per test, asks the FileManager to create a unique
  directory under `ExecutorTestTempDir`, then builds a host tree inside it. All
  helpers take an explicit `(vRefNum, dirID)`.
- Host helpers: `hostFile`, `hostBytes`, `hostDir`, `hostList`, `hostRead`.
- FileManager helpers: `fmGet`, `fmMakeDir`, `fmCreateFile`, `fmDelete`,
  `fmRename`, `fmMove`, `fmList`, `fmListFiles`, `fmChildCount`.

### Coverage

- **Listing**: empty/non-empty; `fmList` vs `fmListFiles` vs `fmChildCount`;
  `parID`/`dirID` and CNID uniqueness; hidden sidecars excluded from listing and
  count; host trees populated before first enumeration are visible; FileManager
  mutations reflected on the host.
- **Creation / deletion**: both directions; new directory CNID returned;
  `dupFNErr` / `fBsyErr` / `fnfErr`.
- **Move / rename**: `PBHRename` and `PBCatMove` (incl. move-with-rename and
  rename-within-directory) keep the CNID; source drops, destination gains; counts
  update; collisions → `dupFNErr`.
- **CNIDs and name disambiguation**: CNID survives rename → move; a
  *truncation*-collision set (host names >31 bytes sharing a 30-byte prefix) so
  the tests do not depend on a case-sensitive host FS; deleting any member leaves
  survivors' names/CNIDs unchanged; moving a disambiguated member with
  `ioNewName = nil` preserves the POSIX name, the disambiguated Mac name, and the
  CNID; case-only ambiguity is covered once and `GTEST_SKIP()`ped on a
  case-insensitive host FS.
- **Path resolution (F1)**: `ResolvePathDoesNotEnumerate` — a path resolve
  performs zero directory scans.
- **Count (F3)**: `ListingDoesNotMaterializeSubdirectories` — listing a directory
  with two subdirectories constructs only those two items; counting a
  subdirectory does not enumerate it.
- **Instrumentation**: a directory is enumerated once and reused
  (`ListingEnumeratesOnceWhileCached`), and the `countScans` counter backs
  `CountScansOnceWhileValid`.
- **Validation (Phase 5)**: an external add/remove/rename inside an enumerated
  directory is visible on the next access; unchanged names keep their CNID and
  `macName`; a repeated listing validates but does not re-enumerate (stats);
  eviction under the item budget, then re-access, preserves CNIDs/names; an
  in-place edit of a MacBinary header is reflected by `getInfo`; a file↔directory
  kind change is handled.
- **Forward-looking** (`DISABLED_`): the large-directory benchmark (run with
  `--gtest_also_run_disabled_tests`). The validation and naming tests above are
  enabled.

### Benchmarks

Not CTest-gated: a micro-benchmark over a synthetic large directory to guard
against regressing F1/F3.

## Out of scope

- Changing how the volume root is chosen (`/`) or mounting behaviour.
- Re-enabling the LMDB mapper, except insofar as it affects Phase 6.
- HFS backend behaviour.

## Future Ideas

- **Smarter invalidation after delete/rename/move.** Those operations update the
  CNID mapping and flush the whole parent directory in the `ItemCache`.
  Invalidating only the affected descriptors/groups would avoid re-enumerating
  the directory.
- **Split Classify from Materialize.** Deciding an item's rep and building it are
  the same step today; separating them would let cheap decisions (and more
  encodings) be added without pre-paying construction cost.

## Discarded alternatives

- **Fused `Item` with in-place promotion.** Keep one class per format and promote a
  cheap placeholder to the concrete class on first access (generalizing
  `upgradeItem`). Smallest change, but it fuses identity and format and mutates an
  item's dynamic type under callers holding a `shared_ptr`. Rejected: identity and
  format must be separable (Decision 10).
- **A rooted tree of realized `Item`s.** Keep parent→child ownership in
  `DirectoryItem` and prune leaves to bound memory. Rejected: the mapper already
  expresses parenthood, a uniform LRU over all `Item`s is simpler than tree
  pruning, and stepping down a path must not drag a directory listing along
  (Decision 15).

## References

- `src/file/localvolume/itemcache.cpp` — `tryResolve(path)`, `cacheDirectory`,
  `countChildren`, LRU.
- `src/file/localvolume/localvolume.cpp` — `nativePathToFSSpec`, `getInfoCommon`,
  `createRepForFile`, `resolve`, `MountLocalVolumes`.
- `src/file/localvolume/item.cpp` / `item.h` — `DirectoryItem::populateCache`,
  `resolve`, `countItems`.
- `src/file/localvolume/simplecnidmapper.cpp` /
  `lmdbcnidmapper.cpp` — mapping, `toMacRomanFilename`.
- `src/file/localvolume/macbinary.cpp` — `isMacBinaryFile` (the `.bin` check).
- `src/hfs/hfsBtree.cpp` — `dirVal` / `ioDrNmFls` semantics.
- `src/util/macstrings.cpp` — `toMacRomanFilename`.
- `docs/subsystems/local-volume.md`, `docs/subsystems/file-manager.md`,
  `docs/ai/2026-10-04-macbinary-localvolume-backend.md`.
