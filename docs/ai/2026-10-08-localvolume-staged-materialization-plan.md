# Plan: staged materialization for `LocalVolume`

> **AI-generated.** This document was produced with the assistance of an AI
> language model and may contain inaccuracies. It is a *plan*, not a record of
> implemented behaviour.

- **Date:** 2026-10-08
- **Status:** Phases 0–3 done (object model + deferred materialization); Phases 4–7 planned
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
| DirectoryItem | **Enumerated** | its children have been instantiated |
| FileItem | Instantiated | identity only |
| FileItem | **Materialized** | it has a `FileRep` |
| FileItem | **Resolved** | it has a `CNID` and a `macName` |

and it **splits the file-format concern out of `Item`** (into a `FileRep`), so an
`Item` can exist without knowing its format. The headline fixes are that **path
resolution must not enumerate any directory** and that **reporting a count must
not realize the directory's contents**.

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
has already handed out. The mapper already stores each mapping's host path and
reuses it across enumerations, so identity itself is path-keyed; the enumeration
dependency lives in `ItemCache::tryResolve(fs::path)` (`itemcache.cpp:169`),
which has no path → CNID index and must enumerate each ancestor to find the child.

## Decisions

1. **Content-derived names are dead.** No name is ever read from an
   AppleDouble/AppleSingle/MacBinary header. All names are transformations of the
   host basename.
2. **Sidecars are always hidden by name**, independent of whether they are
   valid. `._`/`%` (AppleDouble) and `.rsrc`/`.finf` (Basilisk) are filtered
   purely from `readdir` output; no validity check is involved.
3. **Identity is path-determined and name-independent.** Identity is keyed by
   `(parID, posixName)`; a mapping is reused whenever the same entry is seen
   again. `macName` does not determine the CNID and is **optional/undecided**
   until Resolve.
4. **Name resolution is lazy and group-scoped.** Final names are decided when a
   member of a `macNamePrefix` group is named, not eagerly.
5. **`macNamePrefix` is a conservative grouping key** (see *Names*).
   Over-grouping costs only laziness; under-grouping would be a bug.
6. **Disambiguation is assigned in catalog order**, making it deterministic; a
   later iteration persists the numbers (Phase 6). (Catalog order is defined by
   Decision 11.)
7. **Caching is validation-based**, not timer-based (see *Caching*).
8. **Re-enumeration must not renumber or rename existing entries.** The mapper
   already has this property (reuse by path); the staged design must preserve it.
9. **Resolving names is a transaction.** Assigning `macName`/`CNID` is one atomic
   read-modify-write, scoped to the whole directory first and then narrowed to the
   `macNamePrefix` group (Phase 4, sub-steps 1–2), so concurrent writers converge
   and numbers stay stable.
10. **Identity is split from format.** `Item` may be instantiated without knowing
    its file format; the format lives in a `FileRep` (see *Object model*).
11. **Catalog order is host-path sort order, for now.** The mapper assigns in the
    order the host lists the directory (path-sorted). This can look unexpected to
    a Mac user; ordering by Mac name would instead force Resolve a little earlier.
    Revisit if the native order proves objectionable.
12. **delete/rename/move update the mapper and flush the whole directory.**
    `deleteCNID`/`moveCNID` keep the CNID mapping correct; the `ItemCache` flush
    stays directory-wide. Finer-grained invalidation is deferred (*Future Ideas*).

## Names

### `macNamePrefix`

At a file's Instantiate we know a grouping key, **not** the name:

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

Entries with equal `macNamePrefix` are the ones that can collide (the **group**).

### The final name

The final Mac name is the raw name or the `.bin`-stripped name, chosen by the
winning rep: MacBinary → stripped; every other rep → raw. The rule belongs to
the rep, not a standalone extension check: a `foo.bin` with a `._foo.bin`
companion is AppleDouble and keeps `foo.bin`. The mapper's `macName` field holds
the chosen name and is **optional** until Resolve decides it.

## Stage model

These are optional, orthogonal **states** of an object — not a pipeline, and with
no implied ordering between them. The one exception is `.bin` files, below.

| Object | State | Meaning |
|---|---|---|
| DirectoryItem | Instantiated | identity only |
| DirectoryItem | **Enumerated** | its children have been instantiated (one readdir; sidecar names filtered) |
| FileItem | Instantiated | identity only; `(parID, posixName, macNamePrefix)` known |
| FileItem | **Materialized** | it has a `FileRep` |
| FileItem | **Resolved** | it has a `CNID` and a `macName` |

Enumerate is both the count and the child instantiation: the number of visible
descriptors answers `ioDrNmFls`, and the descriptors become child `FileItem`s.
`ioDrNmFls` is a snapshot by nature (HFS keeps a stored valence; the host
directory is live), so a cheap readdir count is no less correct than counting the
same live directory eagerly — document the staleness bound rather than pretending
to exactness.

Materialized and Resolved are independent: a file can be **Resolved without being
Materialized**. The exception is a `.bin` file, whose final name depends on the
winning rep, so it must be Materialized (Classify) before it can be Resolved.

Materialize does not read file data — it establishes the item's representation
(which rep, where the forks live, Finder metadata), and does not imply reading a
whole file.

### Enumerate (DirectoryItem)

Read the host directory once and store intermediate **descriptors** `(host path,
posixName, kind, macNamePrefix)` for the visible children (sidecar names filtered
by name), then instantiate child `FileItem`s from them. The descriptors are
retained so a following operation reuses the scan.

### Materialize (FileItem)

Construct the `FileRep` (the format concern). See *Classify*.

### Classify

For now Classify is just another name for Materialize: we decide the rep by
building it. The two words are kept distinct so they *could* be split later
(*Future Ideas*).

### Resolve (FileItem, or a DirectoryItem's children)

`Resolve` assigns an item's `macName` and `CNID`. It delegates to the parent
`DirectoryItem`, which:

1. **Classifies every `.bin` child** (a `.bin` child's final base name is only
   known once its winning rep is known), then
2. **Resolves the directory's children** — first cut: all of them; later, only
   the group sharing the item's `macNamePrefix` —

in **one CNID-mapper transaction** (Decision 9).

Path resolution Resolves the components along the path individually; a collision
can only involve same-`macNamePrefix` siblings, which in the common case is a
singleton group.

## Object model

The format concern is **split out of `Item`** so an `Item` can exist without
knowing its file format (Variant B; see *Discarded alternatives*):

- **`Item`** — identity only: `parID`, `cnid`, `path`, `name` (optional until
  Resolve), plus the identity ops (`deleteItem`, `moveItem`). Instantiable at
  Instantiate. `deleteItem`/`moveItem` stay the public API here, but delegate the
  host-file mutation to the rep (see below) so that a file's format need not be
  known until it is Materialized.
- **`DirectoryItem : Item`** — the children (descriptors, then child `Item`s) and
  the directory stages.
- **`FileItem : Item`** — holds an optional `FileRep`.
- **`FileRep`** — the format concern: `open`, `openRF`, `getInfo`, `setInfo`, and
  the host-file mutation `remove`/`move` (default = a single host file; the
  sidecar formats AppleDouble and Basilisk override). Concrete `PlainRep`,
  `AppleDoubleRep`, `AppleSingleRep`, `BasiliskRep`, `MacBinaryRep`, `MacRep`.
  Built at Materialize.

`ItemCache::items_` (`cnid -> weak_ptr<Item>`) stays the realization cache, so
"find an already-realized child" is answered there, not by the parent.
`upgradeItem` becomes a `FileRep` swap (it still rewrites the host file into the
richer encoding).

## Caching

Split the two jobs the current LRU + 1 s timeout (`itemcache.cpp:26`) conflates:

- **Memory bound** stays an LRU over realized items / enumerated directories.
  Eviction is now a pure memory concern.
- **Staleness** becomes explicit validation, so correctness no longer depends on
  timing:
  - **Membership**: `stat` the directory itself; if its mtime is unchanged since
    Enumerate, the descriptors, `macNamePrefix` groups, and count are still valid —
    O(1).
  - **Per-entry**: escalate to re-reading the directory's `(filename, mtime)`
    list only when the directory mtime moved; this catches add/remove/rename and
    in-place edits (which directory mtime alone does not).
  - **Materialized item**: on access, `stat` the item; if `(mtime, size)` differ
    from what was materialized, drop its `FileRep` (back to Instantiate).

This removes the 1 s / 20-directory policy entirely.

## Component changes (sketch)

### `CNIDMapper` (`cnidmapper.h`)

Backing store, per volume:

- `cnid -> { parID, posixName, macName?, mtime, (dev, inode) }` — the item
  record. Replace the cached `fs::directory_entry` with a plain path/filename and
  an explicit `mtime` snapshot. `macName` is optional until Resolve (Decision 3).
  `(dev, inode)` is stored now for future rename tracking (Phase 7).
- `(parID, posixName) -> cnid` — the load-bearing index. It is also the
  directory content/diff index (deletions = stored children not seen in the host
  scan). Replaces the current path-sorted `directories_` vector.
- `(parID, macName) -> cnid` — **deferred** (needs per-file staleness rules);
  until then, lookup by Mac name forces enumeration via the existing `tryResolve`.

Operations:

- `getOrCreate(parID, posixName)` — assigns `CNID`; `macName` may be undecided.
- Batch re-enumeration reconciles against `(parID, posixName)`, reusing CNIDs and
  keeping decided `macName`s (Decision 8).
- **Resolve** decides `macName` (and, when needed, allocates the CNID) as one
  transaction over the group (Decision 9).

### `ItemCache` / `LocalVolume` (`itemcache.*`, `localvolume.cpp`)

- `ItemCache::tryResolve(fs::path)`: resolve via CNID only; do not enumerate
  ancestors. *(done — Phase 1)*
- `getInfoCommon`'s directory branch: use Enumerate's count instead of
  `cacheDirectory` + `countItems()`. *(done — Phase 2)*
- `LocalVolume::nativePathToFSSpec` / `resolve(...)`: no ancestor enumeration;
  the name for the FSSpec comes from Resolve of that one entry.
- `resolve(name)` (`localvolume.cpp:170`): name lookup is no longer a prebuilt
  map. Resolve the parent directory (or the name's group), then look up the name.

### `Item` / `DirectoryItem` / `FileItem` (`item.h`, `item.cpp`)

- `Item`: identity + optional name; instantiable without a format.
- `DirectoryItem`: `countChildren()` (the count half of Enumerate), descriptor
  storage + child instantiation, `resolve(index, includeDirectories)` materializing
  only the requested child; keep the HFS-compatible count semantics (files + dirs).
- `FileItem`: holds the optional `FileRep` and builds it lazily via
  `ensureMaterialized()` (called from `open`/`openRF`/`getInfo`/`setInfo`/
  `deleteItem`/`moveItem`). *(done — Phase 3)*

### `ItemFactory` (per-format files)

- Split "kind" (cheap: regular file vs directory, no probing) from "which fork
  layout" (the gauntlet: magic reads / sidecar stats / xattrs). *(done — Phase 3:
  `ItemCache::makeItem` makes the kind decision; `ItemFactory::createRepForFile`
  runs the gauntlet from `ItemCache::materialize`. `DirectoryItemFactory` is gone.)*
- Give reps the naming rule (MacBinary `.bin` stripping) instead of hard-coding
  the host filename.

## Open questions

1. **External modification.** Reconciliation happens at validation time
   (*Caching*), not only at the next full cache.

*Not* open: CNID and name stability across re-enumeration is provided by the
mapper's match-by-path reuse, pinned by `DisambiguatedNamesStableAcrossDelete`,
`MoveKeepsDisambiguatedNameAndPosixName` and `CnidStableAcrossRenameThenMove`.
Phase 6 is persistence/sharing; Phase 7 is inode-based rename tracking.

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

### Phase 2 — Enumerate: count without materialization (F3) — DONE

`ItemCache::countChildren` (readdir + hidden filter, no CNID/item/factory work)
used for `ioDrNmFls` (the count half of Enumerate). Guard:
`ListingDoesNotMaterializeSubdirectories`.

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

Implement `macNamePrefix` grouping, transactional Resolve, catalog-order
disambiguation, and the rep naming rule (MacBinary `.bin` stripping). Two
sub-steps, each independently testable:

1. **Whole-directory Resolve** — one transaction over the directory (the simplest
   correct scope; Decision 9).
2. **Group-scoped Resolve** — narrow the transaction to the `macNamePrefix` group
   that can actually conflict.

**Risk:** correctness of lazy name assignment.

### Phase 5 — Caching redesign

Replace the LRU + timeout with the validation scheme; keep an LRU for memory only.

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
- **Enumerate (F3)**: `ListingDoesNotMaterializeSubdirectories` — listing a directory
  with two subdirectories constructs only those two items.
- **Instrumentation**: a directory is enumerated once and reused
  (`ListingEnumeratesOnceWhileCached`).
- **Forward-looking** (`DISABLED_`): host-side changes visible without
  invalidation (*Caching*), MacBinary `.bin` stripping (Phase 4), large-directory
  benchmark.

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

Two object-model variants were considered and dropped in favour of the split
above (Variant B):

- **Variant A — fused `Item` with in-place promotion.** Keep one class per format
  and promote a cheap placeholder to the concrete class on first access
  (generalizing `upgradeItem`). Smallest change, but it keeps identity and format
  fused and mutates an item's dynamic type under callers holding a `shared_ptr`.
- **Variant C — descriptor-first directory.** `DirectoryItem` stores descriptors
  and no `Item` exists until Materialize. Most faithful to the stages, but it
  forces the listing/metadata API (`resolve`, `getInfoCommon`) onto descriptors
  and rewrites every `dynamic_cast` site.

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
