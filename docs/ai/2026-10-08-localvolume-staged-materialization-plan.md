# Plan: staged materialization for `LocalVolume`

> **AI-generated.** This document was produced with the assistance of an AI
> language model and may contain inaccuracies. It is a *plan*, not a record of
> implemented behaviour.

- **Date:** 2026-10-08
- **Status:** Phases 0–5 were implemented under the *older* object model (see the phase headings). The link-free design above is the target; Phases 6–10 reach it and are DONE. Phase 11 is implemented through 11c (the LMDB backend is the default); Phase 11d is preliminary and not agreed, and Phases 12–13 are not implemented.
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

This plan separates the stages that have been conflated — Count, List,
NameResolve and Materialize. They are optional, orthogonal **states of an
object**, not a pipeline:

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
`CNIDMapper`, and realized `Item`s are kept alive only by the realization LRU
or an external reference.
The file-format concern is **split out of `Item`** (into a `FileRep`), so an
`Item` can exist without knowing its format, and **a `CNID`'s identity is the
host file's inode**, not its name. The headline fixes are that **path
resolution must not list any ancestor** (at most the target's immediate parent,
to name it) and that **reporting a count must not enumerate or materialize the
directory's contents**.

## Findings (motivation)

### F1. Path resolution walks and fully caches every ancestor

`Executor::cmdlinePathToFSSpec` routes a host path (no `:`) to
`Executor::nativePathToFSSpec` (`src/file/fileMisc.cpp:245-251`), which calls
`LocalVolume::nativePathToFSSpec` (`src/file/localvolume/localvolume.cpp:867`).
That canonicalises the path (following the `result` symlink into `/nix/store/…`)
and calls `ItemCache::realizeByHostPath(fs::path)` (`itemcache.cpp:169`).

`realizeByHostPath(path)` walks the path component by component from the volume root and
calls `cacheDirectory(dir)` on **every ancestor**. Each `cacheDirectory`
(`itemcache.cpp:42`) does:

- one `directory_iterator` over the directory;
- the batch CNID diff — then `SimpleCNIDMapper::mapDirectoryContents`, now
  `ItemCache::mapDirectoryContents` — sorts all entries and runs the diff;
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

Names are chosen by `ItemCache`'s naming policy from the host filename during
batch enumeration (the policy formerly lived in the two mappers), with `#N`
suffixed by `toMacRomanFilename(name, index)` for collisions. `ItemCache::cacheDirectory`
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

and the CNID store's policy is batch-shaped
(`ItemCache::mapDirectoryContents(dirID, allEntries)`) because batch mode is
*possible* only as a consequence of always caching a directory whole. **Whole-directory batching is an artifact** — assigning a single
entry a CNID needs only its parent id and its host path. But **batching by shared
`macNamePrefix` is required**: disambiguators must be assigned consistently for
the whole group, and assignment must query the store for the disambiguators it
has already handed out. The store keeps each binding's `posixName` and identity
and reuses them across enumerations; the enumeration
dependency lives in `ItemCache::realizeByHostPath(fs::path)` (`itemcache.cpp:169`),
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
3. **[revised] Identity follows the host file, not the host name.** A `CNID`
   follows the *file*: a **same-directory external rename** keeps it, and a
   delete-and-recreate at the same name gets a new one. (A cross-directory host
   move is seen as delete + create, so it does not; a FileManager-initiated move
   keeps the CNID through an explicit `moveCNID`.) CNIDResolve assigns the `CNID`
   (and its `parID`) at **CNIDResolve**; the `macName` is decided separately, at
   **NameResolve**, and is therefore optional until then. `macName` never
   determines the `CNID`.
4. **Naming avoids known conflicts, and is batch-only for now.** Deciding a name
   assigns a `macName` that does not collide with any name already assigned in
   that directory. A "resolve my name" request triggers a `NameResolve` of the
   whole `macNamePrefix` group it falls in — deterministic, and simpler because
   the group is decided at once. Single-item (order-dependent, faster) name
   resolution is a possible later phase.
5. **[revised] `macNamePrefix` is a conservative grouping key** (see *Names*).
   Over-grouping costs only laziness; under-grouping would be a bug. The group is
   the contiguous run of equal prefixes in a prefix-sorted descriptor list, found
   by binary search (no separate group index).
6. *(removed.)*
7. **Caching is validation-based**, not timer-based (see *Caching*).
8. **[revised] Re-enumeration must not renumber existing entries.** The reconciliation
   match key is the host inode (with the kind), not the path, so a delete-and-recreate
   at an existing name is a *new* entry by design (Decision 3) — while a
   same-directory rename keeps its CNID and adopts the new name (Phase 10).
9. **[revised] Every CNID-store read-modify-write is one transaction.** The
   per-directory diff, get-or-create (`resolveChildCNID`), `nameGroup`, and
   `deleteCNID`/`moveCNID` each read and write inside a single write transaction
   (`ItemCache` policy over `CNIDMapper::withTransaction`),
   so concurrent writers converge (Phase 11). No host I/O runs inside one. The
   naming batch is the prefix group, not the whole directory.
10. **Identity is split from format.** `Item` may be instantiated without knowing
    its file format; the format lives in a `FileRep` (see *Object model*).
11. **[revised] Catalog order is `macNamePrefix`-major, host path as the
    tie-break.** A `Listed` directory's descriptors are sorted by
    `(macNamePrefix, posixName)`, and that is the listing order. (Was: host-path
    sort order.)
12. **delete/rename/move update the CNID store and flush the whole directory.**
    `deleteCNID`/`moveCNID` keep the CNID mapping correct; the `ItemCache` flush
    stays directory-wide. Finer-grained invalidation is deferred (*Future Ideas*).
13. **[revised] Count and List are separate directory stages.** Count keeps only
    a tally (no descriptors, no mapper work); List stores the descriptor list and
    yields the count for free (see *Stage model*). Counts vastly outnumber
    Listings (`ioDrNmFls` is requested for every directory a listing shows), and
    List costs memory, so the two must not be conflated.
14. **[revised] Staleness is validation-based, in three scopes** (see *Caching*):
    directory contents use `(mtime, size)` from one raw `stat`; identity uses
    `(dev, ino)`; a file representation re-tests the probe trace its factory left
    (re-checking candidate existence, trusting the content probes). External
    changes are reconciled at access, not on a timer.
15. **Items are link-free.** No `Item` owns another; a `DirectoryItem` stores a
    descriptor list, not child `Item`s. Identity lives in the CNID store, and
    realized `Item`s are retained only by the LRU or an external reference (see
    *Object model*).
16. **AppleDouble companion ownership follows the winning rep.** Both `%name`
    and `._name` are hidden by name, but only the scheme selected during
    classification is part of the item. Delete and move touch only that scheme;
    a companion in the other scheme is left untouched. This is deliberate until
    a concrete reason to treat both names as owned appears. Directories are not
    rep-classified, so their own companions are handled separately by name.

## Names

### `macNamePrefix`

While listing a directory we know a grouping key, **not** the name:

```
macNamePrefix(posixName) = truncate(30 - maxDisambiguatorDigits)(
                             stripAll(
                               first31(toMacRomanFilename(posixName, 0))))

stripAll(x) | endsWith ".bin" = stripAll(stripBin(x))
            | endsWithDisambiguator = stripAll(stripDisambiguator(x))
            | otherwise = x
```

- `toMacRomanFilename(posixName, 0)` — the same host→Mac filename conversion
  used for the final name (see *The final name*), with no disambiguator suffix.
  It maps `:` to `/` and falls back to raw bytes (high-bit bytes → `?`) when the
  host name is not representable, so the grouping key is a function of the *Mac
  name*. Do **not** use the raw `toMacRoman()` character mapping here: it would
  group a host `a:b` under `A:B` while its Mac name is `a/b`, and `child("a/b")`
  would compute a different key. The conversion's own 31-byte cap/ellipsis does
  not affect the prefix: the final truncation to 27 drops the ellipsis at byte
  30. (`first31` is therefore redundant with the current conversion, but
  harmless.)
- `stripBin` — drop a trailing `.bin`, so a MacBinary `foo.bin` groups with a
  plain `foo`.
- `stripDisambiguator` — drop a trailing `#<digits>`.
- `maxDisambiguatorDigits = 3`; truncate to `30 - 3 = 27` bytes to leave room for
  a generated `#999`.

In the code this composition is `hostGroupPrefix(posixName)`; `macNamePrefix`
is the second stage, applied to a Mac string. The prefix is computed at
enumeration from the same host→Mac conversion that produces the final name, and
stored on each descriptor. It is **case-folded** (the same MacRoman upper-casing
the mapper's name index uses), so
`Foo` and `foo` are one group — grouping must not be case-sensitive while the name
index is not. A `Listed` directory keeps its descriptors sorted by the folded
prefix then `posixName`, so a group is exactly the contiguous range of descriptors
with equal folded prefix, found by binary search — no separate group index.
Entries in a group are the ones that can collide. Because a name's group is a
function of the name itself, two names from different groups can never be equal,
so a group's collision set is exactly its own members (already-decided ones plus
those decided in the current batch) — naming stays linear in the directory size.

### The final name

The final Mac name is the raw name or the `.bin`-stripped name, chosen by the
winning rep: MacBinary → stripped; every other rep → raw. The rule belongs to
the rep, not a standalone extension check: a `foo.bin` with a `._foo.bin`
companion is AppleDouble and keeps `foo.bin`. The mapper's `macName` field holds
the chosen name.

The `macNamePrefix` grouping key is **lexical** (it always strips a trailing
`.bin`), so List needs no format knowledge of any file. Deciding the *final* name
does: for a `.bin` entry the winning rep decides whether `.bin` survives, so
**NameResolve materializes the `.bin` entries of its group** — the only entries
whose name can change. Probing `.bin` files is the expensive part of naming and is
deliberately kept out of List: a `Listed` directory has descriptors but has not
probed any file, and nothing is Materialized merely because its parent was listed.

## Stage model

These are optional, orthogonal **states** of an object — not a pipeline, with no
implied ordering except where noted: List implies Count (and, for every child it
describes, CNIDResolved), and Realize is not a state at all (see below).

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
without its format being known. The per-entry form of `CNIDResolve` is what lets
path resolution walk an ancestor without listing or naming it; List runs the
same step for every child as part of its diff.

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
`(posixName, kind, macNamePrefix, cnid)` per visible child (sidecar names filtered
by name) — plus a validation stamp. The reconciliation diff assigns each child's
`CNID` (see *Reconciliation*). The descriptors are sorted by
`(macNamePrefix, posixName)`. The count is `descriptors.size()`: List implies
Count. No child `Item` is created or retained.

### CNIDResolve (Item)

Give an entry a `CNID` (and its `parID`) by looking it up in the CNID store
through `ItemCache` — keyed by
its binding `(parID, posixName)`, with the record's `(dev, ino)` identity
validating it — or creating a record on a miss. List performs it for every
visible child as part of its reconciliation diff. The same per-entry primitive
(`ItemCache::resolveChildCNID`) is what path resolution uses, component by
component, so a walk needs only the entry's parent and its host identity, not the
parent's listing, and can descend without enumerating.

### NameResolve / Resolve (a directory's group)

Decide `macName`s for one `macNamePrefix` group — the contiguous run of equal
prefixes in a `Listed` directory's descriptor list — assigning each a name that
does not collide with names already assigned in that directory. This is the
atomic naming batch (Decision 9). A whole-directory Resolve is the special case
of doing it for every group. Naming is **batch-only** (Decision 4): a request
names the group it falls in, never a single entry.

"I want this entry's name" is funnelled through a single directory-side entry
point that owns both the lookup and the state change — it ensures the directory
is `Listed` (so every child's `CNID` is assigned) and decides the group's names —
so no caller drives those steps itself. Any read of an `Item`'s `macName` goes
through it, so a caller never observes an undecided name. That entry point lives
on `DirectoryItem`, which owns the Count/List/NameResolve machinery.

### Materialize (FileItem)

Construct the `FileRep` (the format concern). See *Classify*.

### Classify

For now Classify is just another name for Materialize: we decide the rep by
building it. The two words are kept distinct so they *could* be split later
(*Future Ideas*).

### Realize (a cache event, not a state)

Turning a store record into a live `Item` object is not a stage. It is a cache
event driven by the realization LRU (see *Caching*): any record can be realized on
demand and dropped arbitrarily, because a realized `Item` is a pure function of
its store record plus live host state.

## Object model

**Items are link-free.** No `Item` owns another `Item`: a `DirectoryItem` stores
descriptors, not children, and the parent→child relation lives entirely in the
`CNIDMapper`. A realized `Item` is held only by the realization LRU or by an
external reference (see *Caching*).

- **`Item`** — identity: `parID`, `cnid`, `path`, and `macName`, plus the identity
  ops (`deleteItem`, `moveItem`). `cnid` is assigned at **CNIDResolve**; `macName`
  is decided separately at **NameResolve** and is therefore *optional* until then
  (the authoritative copy lives in the store record). The record stores no path:
  it holds `parID` + `posixName`, and the path is derived from the `parID` chain.
  `deleteItem`/`moveItem` stay the public API here but delegate the host-file
  mutation to the rep, so a file's format need not be known until it is Materialized.
- **`DirectoryItem : Item`** — the two directory stages and their stamps: an
  optional `count_` (+ stamp), and an optional **descriptor list** sorted by
  `(macNamePrefix, posixName)` with a per-descriptor `CNID` (assigned by List's
  reconciliation diff). No child `Item`s.
- **`FileItem : Item`** — holds an optional `FileRep`.
- **`FileRep`** — the format concern: `open`, `openRF`, `getInfo`, `setInfo`, and
  the host-file mutation `remove`/`move` (default = a single host file; the
  sidecar formats AppleDouble and Basilisk override). Concrete `PlainRep`,
  `AppleDoubleRep`, `AppleSingleRep`, `BasiliskRep`, `MacBinaryRep`, `MacRep`.
  Built at Materialize.

`ItemCache::items_` (`cnid -> weak_ptr<Item>`) is the realization registry, backed
by an LRU that owns the realized `Item`s (files *and* directories). So "find an
already-realized child" is answered there, not by the parent. `upgradeItem`
becomes a `FileRep` swap: the upgrade targets (AppleDouble, Mac) only *add* their
storage beside the existing data fork, so the host file keeps its inode — and the
item its CNID.

## Caching

Three jobs are conflated in the current LRU + 1 s timeout (`itemcache.cpp:26`):
realizing items, bounding memory, and staying fresh. Split them.

### What a DirectoryItem caches

`DirectoryItem` carries two optional pieces of state, a separate validation
stamp for each, and a per-descriptor CNID:

```cpp
std::optional<int> count_;                  // Counted: a tally only
FileStamp countStamp_;                      // the stamp count_ is valid for
std::vector<Descriptor> descriptors_;       // Listed: sorted by (macNamePrefix, posixName)
                                            //   posixName, kind, macNamePrefix, cnid
bool listed_ = false;
FileStamp stamp_;                           // (mtime, size) at the last scan
```

- `listed_` implies the count (`descriptors_.size()`), which is then the answer
  Count gives; `count_` is ignored while listed.
- `count_` alone means Counted but not Listed.
- `descriptors_` is valid iff `stat(dir) == stamp_`; a bare `count_` is valid iff
  `stat(dir) == countStamp_`. A listing validation failure rebuilds the listing
  (and its per-descriptor CNIDs), so a stale listing is never served merely
  because a fresh count was taken. A *count* is the one answer that a Listed
  directory serves without revalidating — deliberately, see *Listing staleness*.

### The three staleness scopes

All three checks are validation-at-access; none is timer-based.

| Check | Question | Compared | Scope | On failure |
|---|---|---|---|---|
| **Listing** | Are `descriptors_`/`count_` current? | `(mtime, size)` of the directory | one directory | drop both payloads, rescan, run the diff |
| **Binding** | Does CNID *c* still denote the same file at `path(parID, posixName)`? | `(dev, ino)` + kind of that path | one CNID binding | flush the parent's listing, re-list, update the live `Item`, retry once |
| **Rep** | Is the materialized representation current? | the facts the gauntlet probed (candidate existence re-tested, content probes trusted) | one `FileItem`'s rep | drop the rep, re-run the gauntlet |

- A **binding** failure *escalates* to a listing revalidation of the parent: "gone"
  and "moved" are distinguishable only by re-listing, so the escalation is the
  whole rename-recovery mechanism.
- A **listing** revalidation *resolves* binding state (and decides names): the diff
  is the only writer of bindings.
- **Rep** staleness is orthogonal: it validates the format layer against the host
  files, never reads or writes a binding, and never escalates.
- The `(parID, macName)` and posix-name maps are maintained by the diff, not
  validated on read; a hit is a hint, still subject to the binding check. "Is the
  name decided?" is a decidedness flag, not staleness.

### Listing staleness

`cacheDirectory` becomes `DirectoryItem::ensureListed():

```
if listed_ and stat(dir) == stamp_: return   # L0, O(1)
relist(dir)                                  # L1
```

- **L0 (O(1))**: `stat` the directory itself; if the stamp is unchanged, the
  descriptors and count are all still valid.
- **L1**: scan the host directory and re-run the mapper diff (reusing CNIDs by
  binding, assigning new ones, `deleteCNID`ing children no longer present). The
  scan is two passes over one vector of names: the `readdir` runs to completion
  first, and only then is each name `stat`ed. That keeps the directory stream
  from spanning the per-entry stats, which matters for a directory needing more
  than one `getdents` fill — a single fill is one atomic snapshot, so the risk
  lives entirely in the span between fills.

The stamp is `(mtime, size)` taken from **one raw `stat`**, read *before* the scan
and stored with the result: the listing is keyed to the state it started from, so
a change arriving while we scan makes the next access re-list rather than be
served as valid. (Do not read it after the scan instead: that would claim
validity for a state whose entries we may not have observed.) Do **not** use
`fs::last_write_time()` for the stamp at all: this project's `fs` is
Boost.Filesystem, whose `last_write_time()` returns `std::time_t` — one-second
resolution, which would miss same-second changes and make the validation test
flaky.

Counting caches the same way, with one deliberate exception: `Count(dir)` returns
`descriptors_.size()` whenever the directory is Listed, a cached `count_` if its
stamp is valid, else rescans. The listed fast path does **not** revalidate the
listing, because a stamp mismatch there would turn a request for a number into a
full re-List — `readdir`, a per-entry `stat`, the diff, the descriptor rebuild —
which is precisely the cost Count exists to avoid. (A `stat` per directory is not
the objection; the unbounded work it can trigger is.) Nor would revalidating buy
much: `ioDrNmFls` is a snapshot by nature (see above), so the count reported for a
Listed directory is "as of the last List of that directory", the same bound the
rest of the caching uses. A merely-Counted directory has no descriptors to fall
back on, so its tally is validated against `countStamp_` and re-read once stale.

### Binding staleness

A directory stamp only runs on a List. The lookups that deliberately bypass a List
— `CNID -> Item` and path resolution — validate the *binding* directly: derive the
record's path from its `parID` chain plus `posixName`, `stat` it, and compare the
identity and the kind. Match → valid, whatever `(mtime, size)` say. A binding check
validates an existing binding only, never discovers a new entry, so List remains
the bulk path; the `(parID, macName)` index is likewise only as fresh as the last
List or NameResolve, and a hit from it is still validated here.

Identity is `(dev, ino)` when the host supplies a usable inode, otherwise a name
key `(parID, posixName)`. There is no other form — in particular no `nlink` case —
because a CNID denotes a **binding**, not an inode: one inode may have several
bindings (hard links), each with its own CNID.

Identity is stored **in the record**, not in a separate index: a record is
`{ parID, posixName, macName?, identity, kind }`, and a staleness check
always starts from a CNID, so it already has the identity in hand. The record's
`identity` is written when the binding is created (and when a rename re-points
it). A match does not re-stamp it, with one exception: a record whose stored
identity is unusable is re-stamped from a name+kind match once the host supplies
a usable one (Phase 11b's re-adoption). A usable identity is never replaced by
an unusable one, and every identity comes from the same `stat`. The mapper keeps
only two parent-keyed maps:

- `parID -> { posixName -> cnid }` — the directory's **child set** *and* the
  `(parID, posixName) -> cnid` lookup (this replaces a separate child list);
- `parID -> { macName (case-folded) -> cnid }` — name lookup, populated at
  NameResolve.

There is deliberately **no inode-keyed index**. An inode→bindings lookup is only
useful for cross-directory move detection, which cannot be done efficiently and is
not attempted, and recording an inode "wherever it happens to be seen" would
misfire in the presence of hard links. Within one directory the inode pairing is
needed only while re-listing that directory, so it is built from the scan itself (a
transient map, O(n+m)) rather than kept as an index.

When no inode is usable (`d_ino == 0`, a filesystem without inodes, or Windows'
`GetFileAttributesEx`, which exposes none and needs a handle-based variant,
`GetFileInformationByHandle`), the posix-name map *is* the identity: move detection
is off, and there is no false move. The same mode is selectable per volume when
the host's inodes cannot be trusted across a restart or between mounts (Phases
11b/11c), and it is what the diff already does for records whose `identity` is
invalid.

A binding failure is **not** resolved in place: the file may have moved, so nothing
is dropped or deleted — the parent's listing is flushed, the following diff
re-establishes the binding (recovering a same-directory rename, or deleting a
genuinely gone entry), and the access retries once. A live `Item` (an open FCB's,
say) is *updated* to the new location rather than rebuilt, so an open file survives
a host rename.

A reused inode (a file deleted and a new one allocated the same inode before
re-enumeration) will match in the diff and reuse the CNID, provided the kind is
unchanged; a name-keyed diff has the same class of race. A **kind change at a
name is never a match**, even when the host reused the inode — the entry is a new
item. It is accepted that a *delete-and-recreate at a new name whose inode was
recycled* is read as a rename.

There is deliberately **no per-entry `(filename, mtime)` level**: per-entry
metadata is not cached — `Item::getInfo` stats live, and fork/Finder-info reads
hit the rep live — so the only per-entry staleness is the materialized rep, below.

### Reconciliation of external changes

Reconciliation *is* L1: a host change is observed on the next access to the
affected directory, with no FileManager operation. For each stored child `c` of
the directory being re-listed, let `occ(c)` be the scanned entries whose identity
matches `c`:

- some occurrence in `occ(c)` matches `posixName` **and** kind → unchanged: keep
  the CNID and the decided `macName`;
- else exactly one occurrence → same-directory **rename**: keep the CNID, adopt the
  new `posixName`, clear the `macName` (it is re-derived), update the binding;
- else no occurrence → gone: `deleteCNID(c)`, recursively;
- else (several occurrences, none matching the old name) → `deleteCNID(c)`; each
  occurrence becomes a new entry.

Every scanned entry not consumed by a match is new: List creates a record (a fresh
`CNID`) for it, so List assigns identity to the whole visible child set. A
same-inode record in another directory is never a match here — that is a move,
seen as delete + create (a new CNID). Reconciliation is **per directory**;
cross-directory rename detection is deliberately not attempted (Phase 10).

The rules are order-independent and cover the regular, hard-link and no-inode cases
uniformly. With a usable inode, the scan's entries are paired with the directory's
stored children by `(dev, ino)` — one child for a regular file, one per hard-linked
name; that pairing is scratch built from the scan (see *Binding staleness*), not an
index. With no usable inode the match is name-keyed, so there is no move detection
and no false move.

Consequences:

- A **same-directory external rename keeps the CNID** (Decision 3). A
  delete-and-recreate at the same name, a **kind change** at a name, a rename that
  would leave the inode with several names at once, or a move to another directory is
  a new CNID.
- A renamed entry's **live `Item`s are updated in place** (`path` re-derived,
  `macName` cleared), not rebuilt, so an open FCB survives a host rename. A record's
  path is always **derived from the `parID` chain + `posixName`**, never stored, so
  renaming a directory changes no descendant record.
- **Path resolution** descends by stat + `(dev, ino)` (see *Stage model*,
  CNIDResolve) and lists no ancestor; a stale binding is validated or recreated.
- **Read errors**: a scan that fails must **not** be cached as a valid (empty)
  listing — the directory is left invalid so the next access retries. A requested
  directory that cannot be read reports `ioErr` — the listing funnel
  (`DirectoryItem::child`) and `Count` alike. In a path walk, a component that does not
  exist is `fnfErr`, and one that exists but cannot be `stat`ed is `ioErr`;
  `nativePathToFSSpec` maps both to `nullopt`.
- The staleness bound is "as of the last access". Changes racing the guest's
  other traps are inherently un-transactional (HFS is not transactional either).

Only *naming* changes are made cross-instance through LMDB without a host change,
and so are invisible to the stamp. Every other mapper mutation — create, delete,
move — happens because a host entry appeared, disappeared or was renamed (or
because a FileManager operation changed it), so the stamp or the binding check
already invalidates any answer derived from the old state. Coherence therefore
needs no generation counter: the mapper is read through, and each query answers
from current state inside a transaction LMDB serializes.

An answer that *is* held in memory is held only under a validity rule, never
across one: a listed directory's descriptors carry the CNIDs of the listing they
were scanned for (stamp-validated) and no names, and an `Item`'s decided
`macName` is cached only because naming is **sticky per binding** — `nameGroup`
never reassigns a name it has handed out, and a rename clears the decided name on
both sides (the record's `macName` and the `Item`'s copy, via `relocate`). A
`(parID, macName) -> cnid` result kept across listings, for example, would need
invalidating; the answer is not to cache it. If a mapper cache is ever added, key
it to the database's own read-transaction snapshot, not a bespoke counter.

Identity stability across re-enumeration (CNIDs and decided names survive for an
unchanged entry) is provided by the mapper's inode match, pinned by
`DisambiguatedNamesStableAcrossDelete`, `MoveKeepsDisambiguatedNameAndPosixName`
and `CnidStableAcrossRenameThenMove` — *not* an open question.

### Rep staleness

A rep caches parsed state (`MacBinaryFile::header_`,
`AppleSingleDoubleFile::descriptors`) and can outlive the listing that built it.
A rep is a function of **all** the host files classification consults — not just
the primary one and not just the ones the winning rep reads: AppleDouble and
Basilisk read `._*`/`%*` and `.rsrc`/`.finf`, and xattr-backed metadata likewise.
The **presence or absence of a candidate is itself part of the result** — a Plain
file becomes AppleDouble once its `._foo` appears — so validation must span
**every candidate the factories probed**, not just the primary path.

There is no `(exists, mtime, size)` stamp. Instead the gauntlet leaves a **probe
trace**: a list of the **facts** it relied on, written in a small fixed vocabulary
— `IsRegularFile(Path)`, `NoRegularFile(Path)`, `IsUnchanged(Path)`,
`AttributeExists(Path, AttrName)`, `AttributeDoesntExist(Path, AttrName)`. The
observations are pure data: **how** each fact is re-obtained is *policy*, a handler
kept apart from the vocabulary (`FileItem::probeHolds`), never a behaviour hung on
the observation. **Absent candidates are recorded** (`NoRegularFile`), or a sidecar
appearing later would go unnoticed. When any fact no longer holds the trace fails
and the **whole gauntlet re-runs from the top** — an earlier factory may now win.

Revalidation is **by fact**, not by timestamp:

- `IsRegularFile`/`NoRegularFile` re-`stat` the path, and the `Attribute*` facts
  re-check the xattr. These catch the realistic external changes: another process
  (a second Executor, or Basilisk) *adding* or *removing* a sidecar — an upgrade, or
  a deleted sidecar.
- `IsUnchanged` is **ignored as a whole**: it records that a *content* check (a
  magic number) decided the format; its handler is only a change-check on the
  file's (mtime, size), and the policy discards that verdict rather than re-reading
  the content. So a file rewritten *in place* to a different container format while
  Executor runs leaves a stale rep. That is a deliberate, bounded error: the
  operations an external program realistically performs on a mounted directory are
  sidecar add/delete (covered), and a root is single-writer (LMDB). It is cleaner to
  *deliberately not act* on the change than to record a dependency we would not
  check.

The regular-file fact is asserted **once**, by the gauntlet itself before it runs
any factory: a factory may assume its subject is a regular file, so no factory
repeats the check, and the TOCTOU window between the check and a factory's read
only turns into a reportable read error.

The factories build the trace themselves as they probe (`createRepForFile` takes a
`ProbeTrace&`), so there is no separate `FileRep::dependencies()` accessor: the
operative set is the trace, not the paths a rep later reads.

At the start of `open`/`openRF`/`getInfo`/`deleteItem`/`moveItem`,
`ensureMaterialized` re-tests the trace. Do not revalidate a rep whose fork is
currently open — the guest holds it, so the change surfaces on reopen.

A **path change** (`relocate`, i.e. a rename or move) marks the trace stale
outright: the candidate set is derived from the path, and a rename preserves
`(mtime, size)`, so a timestamp stamp would be structurally blind to it. The
staleness is acted on the next time the item is materialized with no fork open, so
no live parsed object is orphaned.

Trace completeness is not enforced by an architecture: a probe object that
factories must route their reads through was considered and rejected — a factory
can always call `fs::is_regular_file` directly, so routing cannot be enforced.

### Memory bound

An LRU over realized `Item`s — files *and* directories — bounds memory; eviction
drops the LRU's strong reference (the item then dies unless externally
referenced), and is safe because re-realization rebuilds from the mapper. The
queue is ordered by **use**: an item is re-touched when it is handed out, and a
directory again when it is re-listed or re-counted. Its budget is a **weight**
(`Item::cacheWeight()`): one unit per realized item, and a listed directory's
descriptor count, so one huge directory (e.g. `/nix/store`) outweighs many small
items. Default: **500,000 units**, exposed as `localVolumeCacheBudget()`.

**The root is not pinned.** The root path is a plain member, and the root item
(CNID 2) is realized on demand from its mapper record like any other: it is warmed
at construction and thereafter retained only by the LRU or an external reference.

Evicting an item drops the LRU's reference only; an item no one else holds dies
with it, payload and all, while one a consumer still holds keeps its payload (its
listing or its `FileRep`). The mapper record survives either way, so a later List
rebuilds it with the same CNIDs. An open FCB **owns** its
`FileItem` through a strong reference in the FCB table, released when the FCB
closes; while it is open, an external delete or rename of the host file does not
affect the open handle (the OS keeps the fd on the inode). This removes the
1 s / 20-directory policy entirely.

## Component changes (sketch)

### `CNIDMapper` (`cnidmapper.h`)

Backing store, per volume. It is deliberately a dumb value store: records, the
two parent-keyed indexes, and one transaction seam. Reconciliation, naming,
get-or-create, recursive delete and move *policy* live in `ItemCache`; the
backend never decides what a record should mean:

- **record** per CNID: `{ parID, posixName, macName?, identity, kind }`. The
  host **path is derived** from the `parID` chain and `posixName`, never stored, so
  renaming a directory invalidates no record. `identity` is `(dev, ino)` or a name
  key; there is no `nlink` (see *Binding staleness*). `macName` is optional until
  NameResolve.
- `parID -> { posixName -> cnid }` — the directory's child set and the
  `(parID, posixName) -> cnid` lookup.
- `parID -> { macName case-folded -> cnid }` — name lookup, populated at NameResolve.

There is no inode-keyed index: the inode pairing is done from the scan while
re-listing a directory (see *Binding staleness*).

Operations:

- primitives: `get`, `getChild`, `getChildren`, `getName`; `put`, `erase`,
  `putChild`, `eraseChild`, `putName`, `eraseName`, `allocate`.
- one transaction seam, `withTransaction(std::function<void()>)`: the in-memory
  backend runs the callback directly, while LMDB opens a single write transaction
  and retries on map growth or `MDB_MAP_RESIZED`. The callback is re-runnable, so
  a retry starts from the state before the attempt (Decision 9).
- there is no policy here: no diff, no get-or-create, no naming, no recursive
  delete and no move. `ItemCache` composes those from the primitives inside one
  `withTransaction` call.

### `FileStamp` (`filestamp.h`, `filestamp.cpp`)

The raw-stat primitive the three staleness scopes share. `FileStamp` is
`(mtime, size)`; `FileIdentity` is `(device, inode)`, with a `valid` flag that is
false when the host exposes no usable inode; `PathStat` is all of that plus the
kind, from a single `statPath` call. Deliberately not built from
`fs::last_write_time()`, whose one-second resolution would miss same-second
changes (see *Listing staleness*).

### `ItemCache` / `LocalVolume` (`itemcache.*`, `localvolume.cpp`)

- `ItemCache::realizeByHostPath(fs::path)`: **stat-first** — stat each component,
  CNIDResolve it (`resolveChildCNID`) and realize it; **no ancestor is ever
  listed**, and the walk does not name. The target's Mac name is decided lazily,
  on read (`Item::name()` → `ensureNamed` → `DirectoryItem::resolveName`). *(Phase 1
  used a `(parID, posixName)` lookup; superseded.)*
- `getInfoCommon`'s directory branch: use **Count**
  (`DirectoryItem::countVisible`), which keeps only a tally.
- `LocalVolume::nativePathToFSSpec` / `resolve(...)`: no ancestor enumeration; the
  name for the FSSpec comes from NameResolve of that one entry's group.
- `resolve(name)`: the parent is Listed (or the name's group NameResolved), then
  the name is looked up via the mapper's `(parID, macName)` index.
- the CNID store policy: `mapDirectoryContents` (the reconciliation diff),
  `getOrCreateChild`, `nameGroup`, `deleteCNID`, `moveCNID`, and
  `lookupCNID`/`lookupPosixName`/`cnidForName`. Each compound operation runs its
  body inside one backend `withTransaction` call; `DirectoryItem` asks
  `ItemCache` and never touches the backend.
- an LRU over all `Item`s, by cache weight; realization and the backend/factory
  seams the Count/List/NameResolve stages drive.

### `Item` / `DirectoryItem` / `FileItem` (`item.h`, `item.cpp`)

- `Item`: identity (`parID`, `cnid`, `path`, optional `macName`); instantiable
  without a format; owns no other `Item`.
- `DirectoryItem`: the two stages — `count_`/`stamp_` (Counted) and the
  `(macNamePrefix, posixName)`-sorted descriptor list with a per-descriptor `CNID`
  (Listed) — plus the Count/List/NameResolve machinery that owns them
  (`countVisible`, `ensureListed`, `resolveGroup`/`resolveName`) and the lookup
  funnel (`child`); keep the HFS-compatible count semantics (files + dirs).
- `FileItem`: holds the optional `FileRep` and builds it lazily via
  `ensureMaterialized()` (called from `open`/`openRF`/`getInfo`/`setInfo`/
  `deleteItem`/`moveItem`). *(done — Phase 3)*

### `ItemFactory` (per-format files)

- Split "kind" (cheap: regular file vs directory, no probing) from "which fork
  layout" (the gauntlet: magic reads / sidecar stats / xattrs). *(done — Phase 3:
  `ItemCache::makeItem` makes the kind decision; `ItemFactory::createRepForFile`
  runs the gauntlet from `FileItem::ensureMaterialized`. `DirectoryItemFactory` is
  gone.)*
- Give reps the naming rule (MacBinary `.bin` stripping) instead of hard-coding
  the host filename. *(done — Phase 4: `FileRep::nameSource`.)*

## Access and invalidation flows

*(Realization-only validation is deliberately provisional: good enough for the
next stages, to be revisited.)*

Every reference **by CNID** resolves through **`realizeByCNID(cnid)`**, which
fuses the binding check of *Caching* with `ItemCache`'s realization. A listing
realizes from its stamp-validated descriptors via `realizeDescriptor` (see *By
parID + index*) and so does not repeat the binding check:

```
rec  = mapper.lookupCNID(cnid)
path = derivePath(rec)                      # parID chain + posixName
st   = statPath(path)
if st missing, identity or kind mismatch:                 # binding staleness
    flush the parent's listing; retry once                # Phase 10: recovery
item = items_[cnid].lock(), else realize from rec + st and register
return item
```

The binding check deletes nothing: it flushes the parent so the diff can reconcile
(rename or delete) and the access retries. Identity is validated at *realization*;
an already-realized `items_` entry is trusted (the "as of last access" bound). If the
item is open, the diff **updates it in place** rather than replacing it.

Parts: `ItemCache` answers `lookupCNID` / `lookupPosixName` / `cnidForName` /
`mapDirectoryContents` / `nameGroup` / `deleteCNID` / `moveCNID` by composing the
backend's value primitives inside `withTransaction`; `ItemCache` also owns
`items_`, realization, and the identity/kind policy of `resolveChildCNID`; and
`DirectoryItem::child(name)` is the single directory-side lookup funnel (below).

**By CNID** (`resolveDir(dirID)`, `realizeByCNID(CNID)`, FCB revalidation): the steps
above. The record gives the parent, so a binding failure flushes it and retries; a
same-directory rename is recovered even for an access by CNID, while a genuine
delete still fails.

> We may later prefer a *live* `Item` over a mapper lookup, since a
database-backed mapper (LMDB) may be slower than the in-memory object. Left as
written for now; benchmark when the backend matters.

**By parID + macName** (`resolve(name)`, `DirectoryItem::child(name)`):

```
1. cnid = mapper.cnidForName(parID, macName)
2. if cnid: item = realizeByCNID(cnid); ok -> return        # fast path
3. parent = realizeByCNID(parID) as DirectoryItem; else dirNFErr
4. cnid = parent.child(macName):                               # the funnel
     ensureListed()
     prefix = prefixOf(macName)        # truncate27(stripDisambiguator(...))
     resolveGroup(prefix)              # NameResolve the group
     mapper.cnidForName(parID, macName)
5. if !cnid -> fnfErr
6. return realizeByCNID(cnid)
```

**By full posix path** (`realizeByHostPath(path)`, `nativePathToFSSpec`):

```
1. canonicalise
2. dir = root (cnid 2)
3. for each component c but the last:
     st   = statPath(dir.path / c); if missing -> fail
     cnid = cache.resolveChildCNID(dir.cnid, c, st)       # identity-or-create policy
     dir  = realizeByCNID(cnid); must be a DirectoryItem else ENOTDIR
4. last component c:
     st   = statPath(dir.path / c); if missing -> fail
     cnid = cache.resolveChildCNID(dir.cnid, c, st)
     return realizeByCNID(cnid)                                 # not listed or named
```

The walk never lists or names; the target's Mac name is decided lazily, when a
caller reads `Item::name()`.

**By parID + index** (a listing):

```
1. dir = realizeByCNID(parID) as DirectoryItem
2. ensureListed(dir)
3. realizeDescriptor(dir, descriptors_[index])
                                             # files-only: filter by kind first;
                                             # the listing stamp validated the
                                             # descriptor's binding; its name is
                                             # decided lazily on read
```

### The directory-side funnels

Two directory-side entry points each own their stage's state change, so no caller
drives the stages itself:

- **Lookup** — `DirectoryItem::child(name)` / `child(index, …)`: ensure the
  directory is `Listed`, then select and realize the entry. A name lookup also
  NameResolves the target's `macNamePrefix` group; an index lookup leaves the
  name undecided until it is read (see *By parID + index*). This is how
  `resolve`, `resolveRelative` and `resolveForInfo` reach a child.
- **Naming** — `Item::name()` → `ItemCache::ensureNamed` → `DirectoryItem::resolveName(item)`
  (NameResolve the group holding `item`) and `DirectoryItem::resolveGroup(prefix)`
  (the batch itself): every read of an `Item`'s `macName` goes through here, so a
  caller never observes an undecided name.

Both live on `DirectoryItem`, which reaches the CNID policy and the factory
through `ItemCache`'s policy methods / `itemFactory()`, and the realizer through
`ItemCache`'s realize seams (`cacheItem`, `findRealizedItem`,
`realizeDescriptor`).

### Invalidation

| Event | Action |
|---|---|
| create / dir-create | host op, then `DirectoryItem::clearCache()` (clear `listed_`/`count_`, re-cache); the next access re-lists → diff → new record |
| delete | `ItemCache::deleteCNID(cnid)`, drop `items_[cnid]`, flush the parent |
| rename / move (FileManager) | `ItemCache::moveCNID(cnid, newParID, newName)`, flush the old and new parent listings |
| external change | next `ensureListed` (stamp) → re-list → the binding diff (rename/delete/create) + decide the affected name groups; or a binding check fails → flush the parent → re-list → recover |

### Responsibility summary

- **Identity** (CNID/inode lookup, realize, validate, flush): `ItemCache` +
  `CNIDMapper`.
- **Listing staleness & naming** (`ensureListed`, `resolveGroup`/`resolveName`):
  `DirectoryItem` drives the stages, calling `ItemCache` policy methods and the
  factory through `ItemCache`. The per-directory reconciliation diff is
  `ItemCache::mapDirectoryContents`; `DirectoryItem::listDirectory` builds the
  scan and installs the result.
- **Index listing**: `DirectoryItem::descriptors_` + `ItemCache` realizing
  (`realizeDescriptor`).
- `ItemCache` owns the atomic read-modify-write operations (the diff,
  `nameGroup`, `deleteCNID`, `moveCNID`); `CNIDMapper` owns only the
  records/indexes and the transaction seam, and realization/staleness policy
  stays in `ItemCache`/`DirectoryItem`.

### Filesystem contracts

- **Symlinks are followed transparently and are not items**: a symlink to a file is
  that file (whichever rep its target wins), a symlink to a directory is that
  directory, and identity is the target's inode. A link whose target escapes the
  volume root is **not** hidden: it is listed and followed like any other entry
  (relevant only when the root is not `/`, e.g. a Windows drive).

## Phased plan

Phases marked *(based on older design)* were implemented against the earlier
name-keyed, all-or-nothing object model. The code exists and mostly still works,
but each must be re-examined and reshaped for the link-free design. New phases
that take the implementation from here to the target design follow them.

### Phase 0 — Test harness and baseline — DONE (based on older design)

Built the native harness:

- `tests/localvolume_ops.cpp` + `tests/localvolume_test_util.h`, added to
  `NATIVE_TEST_SOURCES`.
- `src/file/localvolume/stats.h` counters, wired into `item.cpp` /
  `itemcache.cpp` / `localvolume.cpp` (`directoryIterations`, `entriesSeen`,
  `itemsConstructed`, `factoryProbes`, and later `countScans`, `nameProbes`);
  `resetLocalVolumeStats()` for tests.
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
- **Name disambiguation is stable across deletion**: the store reuses a binding's
  record (and therefore its CNID) while the host binding exists. Covered by
  `DisambiguatedNamesStableAcrossDelete`.

### Phase 1 — Path resolution without ancestor enumeration (F1) — DONE (based on older design)

The CNID store keeps per-directory children as `(host filename → cnid)`
(the `(parID, posixName)` lookup + diff index); `ItemCache` gained
`lookupPosixName` / `getOrCreateChild`; `ItemCache::realizeByHostPath(path)` walks components through them
instead of `cacheDirectory` on each ancestor. Guard:
`ResolvePathListsAtMostTheTargetParent` (no ancestor scan; at most the target's
immediate parent, to name it).

### Phase 2 — Count without enumerating (F3) — DONE (based on older design)

`DirectoryItem::countVisible` (readdir + hidden filter; no CNID/item/factory
work), used for `ioDrNmFls` as the **Count** stage, distinct from Enumerate.
Guard: `ListingDoesNotMaterializeSubdirectories`. Count's validation-stamp
caching is Phase 5.

### Phase 3 — Object model + deferred materialization — DONE (based on older design)

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
  gauntlet moved behind `FileItem::ensureMaterialized`.
  `DirectoryItemFactory` was removed and `ItemFactory` now exposes
  `createRepForFile`. Verified with the benchmark: 100 entries →
  `directoryIterations=1, itemsConstructed=100, factoryProbes=500` (probes happen
  only when an entry is actually accessed, not during the scan).

### Phase 4 — Rep naming rule — DONE (based on older design; superseded by Phase 9)

- **DONE** — `FileRep::nameSource()` lets the rep decide the displayed name
  (MacBinary drops `.bin`), applied within the then-current eager, whole-directory
  naming pass. Covered by `MacBinaryStripsBinExtension` and
  `SidecarNameWinsOverMacBinary`.

The phase's former name-resolution sub-steps are superseded by Phase 9.

### Phase 5 — Caching redesign — DONE (based on older design)

Replace the LRU + 1 s timeout with validation-based staleness and a memory-only
LRU (see *Caching*). Sub-steps:

1. **DONE — directory stamps + `ensureEnumerated`.** `(mtime, size)` from a raw
   `stat`; L0 validate / L1 re-enumerate; the TTL is gone.
   `HostChangeVisibleWithoutInvalidation` is enabled.
2. **DONE — memory-bound LRU** by total items (default 500 K; the root was
   still pinned at this stage — Phase 7 removes the pinning);
   `EvictionPreservesIdentity`.
3. **DONE — Count caching** (`count_` + stamp); `CountScansOnceWhileValid`.
4. **DONE — per-item rep revalidation** by re-testing the gauntlet's probe trace
   (candidate existence); `RepStalenessTrustsContent`,
   `RepStalenessFollowsCandidates`.
5. **DONE — reconciliation correctness:** kind changes (`KindChangeRebuildsItem`),
   the read-error policy, a cross-directory host move = delete + create
   (`CrossDirectoryHostMoveIsDeleteCreate`).

### Phase 6 — Tests for the target behaviour (written first) — DONE

Pin what the FileManager must do, independent of the implementation, and mark the
ones that cannot pass yet `xfail` / `DISABLED_` (see *Testing*): a same-directory
host-side rename keeps the CNID (Phase 10); listing order is `macNamePrefix`-major
(Phase 7); unrelated groups stay undecided after a name lookup (Phase 9); a
hard-linked name is distinct from its sibling (Phase 10). Later phases add their
own tests too.

### Phase 7 — Link-free object model — DONE

Make a `DirectoryItem` own descriptors, not `Item`s. **The mapper keeps its
current shape** (eager, name-keyed naming); only `DirectoryItem` / `ItemCache`
change.

- `DirectoryItem` stores `descriptors_` sorted by `(macNamePrefix, posixName)`
  (per-descriptor `cnid`), plus `listed_` / `stamp_`; drop `contents_` /
  `files_` / `contents_by_name_`.
- `resolve(index)` walks descriptors and realizes entries on demand.
- `ItemCache` keeps `items_` weak and adds an LRU over *all* `Item`s, with **no
  root pinning** (see *Caching*); `ensureEnumerated` → `ensureListed`; the
  `child()` funnel stays.

**Deliverable:** no child `Item` is retained by its parent. **Risk:** touches
every listing/metadata call site and the `dynamic_cast` sites.

### Phase 8 — Identity keyed by CNID — DONE

- The mapper record gains an explicit `(dev, ino)`; children are the CNID set
  `parID -> {cnid}` (the name key goes away). (The per-entry `mtime` proposed
  here was later dropped; per-entry metadata is not cached.)
- Identity is looked up by inode — `(dev, ino) -> cnid` — and `realizeByHostPath(path)`
  stats each component instead of `lookupChild` / `getOrCreateChild` (which go
  away). The identity-or-create step is one mapper call (`resolveChildCNID`), so it can
  become a single transaction later. *(Phase 10 replaces the inode index with the
  posix-name map; see *Binding staleness*.)*
- **Validation, not recovery:** on access, if the record's path no longer exists or
  its `(dev, ino)` no longer matches, flush the `Item` and the mapper entry. A moved
  item therefore gets a **new** CNID; rename preservation comes later (Phase 10,
  which replaces the eager mapper delete with a parent flush and a retry).
- **A kind change is a new item**, even when the host reused the inode; the diff
  and path lookup require the kind to match as well as the inode.
- **An encoding upgrade swaps the `FileRep`, not the file.** The upgrade targets
  (AppleDouble, Mac) only *add* their storage beside the existing data fork, so
  `upgradeItem` rewrites nothing and the host file keeps its inode — and the item
  its CNID.
- **Hard links are out of scope** (Phase 10).

**Risk:** inode fidelity (`d_ino == 0`) and Windows (no inode from the cheap stat).

### Phase 9 — CNIDResolve / NameResolve split (batch-only) — DONE

- `macName` becomes optional on the `Item` and the record; add the
  `(parID, macName case-folded) -> cnid` index.
- `mapper.nameGroup` decides a whole `macNamePrefix` group at once (Decision 4);
  the `resolveName` funnel owns both the lookup and the state change, and every
  name read goes through it (`Item::name()`).
- Remove eager naming from List / diff; the `.bin` probe lives only in NameResolve.

**Risk:** the undecided-name obligation spans every name reader.

### Phase 10 — Same-directory rename recovery, and hard links — DONE

**Bindings.** A CNID denotes the pair `(parID, posixName)` at which an inode lives,
not the inode alone, so one inode may hold several bindings (hard links), each with
its own CNID. Identity is `(dev, ino)` (or a name key when no inode is usable) and
`nlink` is dropped; the record stores `posixName` and the path is derived from the
`parID` chain, so a directory rename invalidates no record. There is no inode-keyed
index (see *Binding staleness*); the posix-name map doubles as the child set.

**Rename recovery.** The per-directory diff matches a stored child `c` against the
scanned entries with the same identity, per *Reconciliation*: one matching
name+kind → unchanged; exactly one occurrence at a new name of the same kind →
rename (keep the CNID, adopt the `posixName`, clear the `macName`); no occurrence →
delete; several occurrences with no old-name match → delete and re-create each. A
same-inode record in another directory is never a match, so cross-directory moves
stay delete + create.

**Recovery on access.** The binding check no longer deletes a record: on failure it
flushes the parent's listing, the diff reconciles, and the access retries once
(recursing up while an ancestor's own binding is stale). A live `Item` (an open
FCB's) is updated in place, and `macName` is cleared only when the `posixName`
changed. One consequence: a name lookup must re-check the name *after* realizing,
since realizing may have recovered a rename that moved the binding away from the
requested name.

**Accepted:** a recycled inode makes a delete-and-recreate at a *new* name look like
a rename, and inode reuse at the same name still reads as unchanged (see *Binding
staleness*).

### Phase 11 — CNID persistence, transactions, multi-instance sharing — DONE through 11c

Rewrite `LMDBCNIDMapper` against the current interface (now as the primitive
`CNIDMapper`; policy lives in `ItemCache`). The dormant implementation
predated the link-free, identity-validated design and could not be re-enabled as
is: it stored a path rather than an identity, dropped kind, matched by sorted
path, named every new entry eagerly, and had `deleteMapping` stubbed. LMDB's
single writer then lets two Executor instances share CNID and naming decisions
for the same volume.

- **Record and indexes.** Persist `{parID, posixName, macName?, identity, isDir}`
  with no stored path, plus the `(parID, posixName) -> cnid` and
  `(parID, folded macName) -> cnid` indexes. `identity` is a value on the record,
  not an index or a key: it is consulted only by the binding check and the diff's
  inode pairing (see *Binding staleness*). Scope the environment per volume
  (root path or volume identity) and store a schema version; the current code
  opens one global environment and leaves its `config` DB unused.
- **Allocator.** Persist a monotonic `nextCNID` in the config DB. Deriving
  `max + 1` reuses the highest CNID after it is deleted, which can make a stale
  guest CNID denote a different file.
- **Transactions.** Each read-modify-write — the diff, get-or-create, `nameGroup`,
  `deleteCNID`, `moveCNID` — runs in one LMDB write transaction opened by the
  backend's `withTransaction` seam, and no host I/O runs inside one (Decision 9).
  CNID allocation happens inside the transaction. The host scan stays outside;
  the diff re-reads the stored child set inside.
- **Naming.** Keep Phase 9's rule: names are decided only in `nameGroup`, never
  eagerly by the diff. `NameRequest` carries the binding (`posixName`) its
  `nameSource` was derived from, and `nameGroup` drops a request whose record no
  longer has that name, because another instance may have renamed it after the
  listing.
- **Coherence.** LMDB's single writer serializes the transactions; reads are
  read-through and no mapper answer is cached across a validation boundary, so no
  generation counter is needed (see *Caching*).

**Identity is a verification value, not a key.** The persisted keys are the CNID
and the `(parID, posixName)` binding; `identity` is recorded on the record and
consulted only by the binding check and the diff's inode pairing (see *Binding
staleness*). Persistence therefore does not depend on identity stability: if
validation is wrong the maps are still intact, and what actually resets CNIDs and
names is the *policy* that treats an identity mismatch as "the binding changed."
The sub-phases below decide that policy.

### Phase 11a — Disregard the problem — DONE

Assume the volume is on a block-backed filesystem with stable device numbering
(ext4, btrfs, xfs). Persist and validate identities as today. On an identity
namespace change — tmpfs, overlayfs, device renumbering, NFS — the first re-list
fails both diff passes and deletes and recreates every record, so persisted CNIDs
and names are lost. Document that limitation and do not mitigate it; this is the
default. The LMDB backend is enabled, and `ItemCache` validates `(dev, ino)` for
both backends.

### Phase 11b — Global switch to disable identity validation — DONE

`--dont-use-inodes` sets `Executor::dontUseInodes`, which makes `statPath()`
report no usable identity — exactly the inode-0 path (`FileIdentity::valid ==
false`: `sameIdentity` returns true, `filestamp.h:41-48`). The diff then matches by
`(posixName, kind)` only, the `byInode` rename pairing is off, and the binding
check degrades to existence, kind and name. Records written while the flag is on
store `identity.valid = false`; when the flag is removed, the next name+kind
match re-stamps the live identity (a usable identity is never overwritten with an
unusable one).

Consequences: no rename recovery (a same-directory rename reads as delete +
create), and a same-name replacement is not detected. CNIDs and names survive
identity-namespace changes because they are keyed by name. The switch is
per-process, so instances sharing one LMDB must agree on it; otherwise the
validating instance still deletes and recreates on a namespace change. Tests that
pin rename recovery or identity invalidation must be skipped or constrained under
the flag.

### Phase 11c — Per-filesystem heuristic — DONE

A `LocalVolume` is a path subtree, not a filesystem: it can span several
`st_dev` values (nested mounts, bind mounts, a bind-mounted tree under a
container's overlay root). The policy is therefore per filesystem instance, not
per volume. `statPath()` classifies a device once, on first sight, using
`statfs` on the path that produced the stat, and caches the verdict per thread.
On Linux it trusts the block-backed local types (ext2/3/4, xfs, btrfs, f2fs,
reiserfs, nilfs, ocfs2, bcachefs) and treats everything else — tmpfs, overlayfs,
NFS, FUSE, unknown types, and a failed `statfs` — as name+kind only.
`--dont-use-inodes` overrides the classification. Windows already reports no
identity; other non-Linux platforms default to validation on. Tests whose subject
needs inode identity call `requireInodeIdentity()`, which asks `statPath` about
the test tree's own device, so a bind-mounted ext4 test tree runs even when the
container root is overlayfs.

A heuristic, not a proof: it says nothing about device renumbering, btrfs
subvolume id quirks, "same path, different volume," or a device number reused
after a remount within one process. Keep the manual switch for exceptions.

### Phase 11d — Identity epochs (preliminary; not agreed)

Persist an identity epoch (the "generation" idea) with the volume: the `st_dev`
observed at open, plus a volume identity. When the epoch changes, reconcile by
`(posixName, kind)` instead of deleting — keep each CNID and `macName`, re-stamp
`identity`, then resume validation. This preserves CNIDs and names across a
namespace change at the cost of reading a rename made while no instance was
running as delete + create. It is unrelated to the mapper-cache generation
discounted under *Caching*.

Preliminary and not agreed; do not implement without a decision. Open questions:

- What volume identity distinguishes "the same data seen through a new namespace"
  from "different data now at the same root path"? Root path plus schema is weak;
  a volume UUID or a hidden marker is robust but invasive. Without one, a rebase
  can carry CNIDs onto unrelated files that share names.
- Multi-instance: instances must see the same identity namespace, or agree on the
  epoch, or they will keep re-stamping each other. Two mounts of the same data
  (host vs container, two overlays) do not share `st_dev`/`st_ino`.
- tmpfs: after a fresh mount the contents are new, so a rebase can match only what
  still exists; skipping persistence for such volumes is probably better.
- Whether a rebase also repairs records with an invalid or missing identity, or
  only records whose epoch differs.

### Phase 12 — Single-item name resolution

Today a name request resolves the whole `macNamePrefix` group. Add, as an
optimization, single-item (order-dependent) assignment for a name whose group is
otherwise unknown, keeping full-group `NameResolve` as the fallback.

### Phase 13 — Drop Boost.Filesystem in favour of platform APIs

`boost::filesystem` is a lowest-common-denominator wrapper, and the design now
needs detail it does not expose. Replace it with platform-specific code:

- directory scans that read the inode and kind straight from `readdir`
  (`d_ino`/`d_type`) or `FindFirstFile`, so listing no longer runs a `stat` per
  entry to learn its identity;
- a Windows `statPath` that exposes identity (`GetFileInformationByHandle`),
  instead of today's path-keyed fallback;
- optionally, a real change signal for directory staleness
  (inotify/FSEvents/`ReadDirectoryChangesW`) to replace the `(mtime, size)` stamp.

Keep `boost::filesystem` behind the `fs` alias as the fallback for platforms
without a native path.

## Testing

### Existing infrastructure

- `tests/files.cpp` — dual-mode (native + Retro68); Mac ABI only, no Executor
  internals; `FileTest` creates/deletes one file in the default directory.
- `tests/files_internal.cpp` — native-only; uses `nativePathToFSSpec` /
  `cmdlinePathToFSSpec`.
- `tests/localvolume_macbinary.cpp` — native-only; backend functions, no
  FileManager.
- `tests/localvolume_itemcache.cpp` — native-only; drives `ItemCache`'s CNID
  policy through the public realization/listing/move/delete API, using
  `SimpleCNIDMapper` and real host files (diff/rename, identity adoption, naming,
  move, recursive delete).
- `tests/localvolume_cnidmap.cpp` — native-only; the `CNIDMapper` primitive
  contract on both the Simple and LMDB backends, plus LMDB persistence and
  transaction rollback.
- `tests/main_executor.cpp` — global environment: initialises Executor, points
  `ExecutorDirectoryMap` at the temp tree (so CNID state is per-run), calls
  `ROMlib_fileinit()` (mounts `/`), and creates a temp dir opened as the default WD.
- Build split: `NATIVE_TEST_SOURCES` vs `TEST_SOURCES` in `tests/CMakeLists.txt`;
  expected failures are labelled `xfail` in `tests/test-properties.cmake`.
- Suites: `ctest --test-dir build`; the Retro68 Mac-app suite is separate.

### New native test module

`tests/localvolume_ops.cpp` drives the FileManager against the mounted-`/`
`LocalVolume` and interleaves host (`Boost.Filesystem`, via the `fs` alias)
operations. Helpers in
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
  count; host trees populated before the first List are visible; FileManager
  mutations reflected on the host.
- **Creation / deletion**: both directions; new directory CNID returned;
  `dupFNErr` / `fBsyErr` / `fnfErr`.
- **Move / rename (FileManager)**: `PBHRename` and `PBCatMove` (incl.
  move-with-rename and rename-within-directory) keep the CNID; source drops,
  destination gains; counts update; collisions → `dupFNErr`.
- **Identity and rename recovery** (Decision 3; Phase 10):
  - a **host-side** rename of a listed child **within its directory** keeps its
    CNID, adopts the new `posixName`, and re-derives its Mac name; a move to another
    directory is delete + create (a new CNID);
  - the rename is recovered even for an **access by CNID** and for an **open FCB**
    (the `Item` is updated in place and a read through it still works);
  - a delete-and-recreate at the same name gets a **new** CNID;
  - a **hard-linked** file keeps distinct CNIDs for its distinct names; renaming one
    name leaves the other alone, even when it lives in another directory, and adding
    a hard link to a listed directory leaves the original name's CNID alone;
  - a name re-used by a new inode is not mistaken for the old entry.
- **Names and disambiguation**: a *truncation*-collision set (host names >31 bytes
  sharing a 30-byte prefix) so the tests do not depend on a case-sensitive host
  FS; deleting any member leaves survivors' names/CNIDs unchanged; moving a
  disambiguated member with `ioNewName = nil` preserves the POSIX name, the
  disambiguated Mac name, and the CNID; case-only ambiguity is covered once and
  `GTEST_SKIP()`ped on a case-insensitive host FS.
- **Listing order** (Decision 11): `fmList` returns `macNamePrefix`-major order,
  host path the tie-break within a group, so a group's members are contiguous.
- **Group-scoped NameResolve**: naming one group leaves other groups' names
  undecided until asked; a name lookup for one entry does not force a
  whole-directory naming pass; a caller never observes an undecided name.
- **Path resolution (F1)**: `ResolvePathListsAtMostTheTargetParent` — resolving the
  *path* scans no ancestor (components resolve by stat + `(dev, ino)`) and its CNID
  matches a later FileManager access to the same entry; producing the `FSSpec`
  *name* lists the target's immediate parent once (`resolveName`).
- **Count (F3)**: `ListingDoesNotMaterializeSubdirectories` — counting a
  subdirectory constructs no `Item`s for its children and keeps no descriptors.
- **Instrumentation**: a directory is Listed once and reused while valid
  (`ListingEnumeratesOnceWhileCached`); the `countScans` counter backs
  `CountScansOnceWhileValid` and `nameProbes` backs `NameResolveIsGroupScoped`.
- **Validation**: an external add/remove inside a Listed directory is visible on the
  next access; unchanged entries keep their CNID and `macName`; a repeated listing
  validates but does not re-list (stats); a **binding** failure flushes the parent and
  retries rather than dropping the CNID; eviction under the cache budget, then
  re-access, preserves CNIDs/names; a file↔directory kind change at a name is a
  new entry, whatever the inode.
- **Rep probe trace**: an in-place content change is *not* re-probed (the content
  checks are trusted), so a plain `.bin` rewritten as MacBinary keeps the plain
  rep; with the primary file untouched, creating or deleting a sidecar
  (`._*`, `%*`, `.rsrc`, `.finf`) is reflected by `getInfo` on the next access —
  including a Plain file *becoming* AppleDouble when its `._*` appears, and
  becoming Plain again when it is removed.
- **Persistence (Phase 11a)**: records and decided names survive destroying and
  reopening the store; the CNID allocator continues rather than reusing an old
  CNID; a re-open diff preserves CNIDs for unchanged entries and recovers a
  same-directory rename by inode; move and recursive delete persist.
- **Identity policy (Phase 11b/11c)**: `--dont-use-inodes` clears
  `PathStat::identity.valid` and makes `sameIdentity()` fall back. Tests whose
  subject requires inode identity call `requireInodeIdentity()` and skip when
  the test tree's filesystem is classified as non-identity.
- **Forward-looking** (`DISABLED_`): the large-directory benchmark (run with
  `--gtest_also_run_disabled_tests`). The validation, naming and Phase 10
  rename/hard-link tests above are enabled.

### Benchmarks

Not CTest-gated: a micro-benchmark over a synthetic large directory to guard
against regressing F1/F3.

## Out of scope

- Changing how the volume root is chosen (`/`) or mounting behaviour.
- HFS backend behaviour.

## Future Ideas

- **Concurrent modification during a scan is currently unguarded.** A scan is not
  atomic: `readdir` may skip or repeat entries when the directory is modified
  while it is being read (POSIX leaves that unspecified), and the two-pass scan
  narrows the window but does not close it — a directory needing several
  `getdents` fills can still tear. Because a listing is *diffed*, a torn scan can
  read as "this entry is gone" and renumber it (delete + create) under a
  concurrent host writer. We accept that today: a scan is not retried, and a
  "gone" verdict is not confirmed. Removing the consequence means confirming a
  missing child over two consecutive scans (or re-checking its binding) before
  `deleteCNID`.
- **Smarter invalidation after delete/rename/move.** Those operations update the
  CNID mapping and flush the whole parent directory in the `ItemCache`.
  Invalidating only the affected descriptors/groups would avoid re-enumerating
  the directory.
- **Split Classify from Materialize.** Deciding an item's rep and building it are
  the same step today; separating them would let cheap decisions (and more
  encodings) be added without pre-paying construction cost.
- **List a group, not the whole directory.** Correctness needs only the target's
  `macNamePrefix` group; listing every descriptor is the simple superset. A lookup
  could readdir and keep only the target's group.

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

- `src/file/localvolume/itemcache.cpp` — `realizeByHostPath(path)`, realization, LRU.
- `src/file/localvolume/filestamp.{h,cpp}` — `statPath`, `FileStamp`, `FileIdentity`
  (the raw stat the three staleness scopes share).
- `src/file/localvolume/localvolume.cpp` — `nativePathToFSSpec`, `getInfoCommon`,
  `createRepForFile`, `resolve`, `MountLocalVolumes`.
- `src/file/localvolume/item.cpp` / `item.h` — the `DirectoryItem` stages
  (`ensureListed`, `countVisible`), `child`, `countItems`.
- `src/file/localvolume/simplecnidmapper.cpp` /
  `lmdbcnidmapper.cpp` — mapping, `toMacRomanFilename`.
- `src/file/localvolume/macbinary.cpp` — `isMacBinaryFile` (the `.bin` check).
- `src/hfs/hfsBtree.cpp` — `dirVal` / `ioDrNmFls` semantics.
- `src/util/macstrings.cpp` — `toMacRomanFilename`.
- `docs/subsystems/local-volume.md`, `docs/subsystems/file-manager.md`,
  `docs/ai/2026-10-04-macbinary-localvolume-backend.md`.

## Implementation review status (2026-10-21)

These are the points from the implementation review that remain open, with the
test coverage that now exists. Tests marked `DISABLED_` are in
`tests/localvolume_ops.cpp`; run them explicitly with
`--gtest_also_run_disabled_tests` (command at the end of this section).

### Still open: design deltas

| Point | Nature | Test case now? |
|---|---|---|

| Windows: `statPath` exposes no identity and does not follow reparse points (Phase 13 is not being done). | Accepted limitation | No — needs Windows CI |

### Still open: other holes

| Point | Nature | Test case now? |
|---|---|---|
| `Item::name()` has no failure signal: it can return an empty name. `nativePathToFSSpec` now rejects that instead of emitting a zero-length FSSpec. | Remaining API limitation | **Yes** — `UnreadableParentHasNoEmptyFSSpecName` (enabled; passes) |
| A per-entry `stat` failure is read as “gone”, the listing is cached as valid, and the child's CNID is deleted. `PathStat` cannot distinguish `ENOENT` from `EACCES`/`EIO`. | Bug | **Yes** — `DISABLED_StatFailureDoesNotRenumberChild` (disabled; fails). The `PathStat` API aspect is not separately tested. |
| Host-path resolution canonicalizes symlinks, so a link whose target escapes the volume root returns `nullopt` instead of being followed as the listing does. | Edge case | No — needs a non-`/` volume fixture |
| Persistent CNID DB has no GC/compaction. There is no evidence this is a practical concern; it remains an open design question, not an observed defect. | Theoretical / open policy | No — expected behavior is undefined until a policy is chosen |

### Closed since the review

- **Doc inconsistencies** (status line, inode-keyed wording, re-stamping, root
  pinning, Phase 8 `mtime`, funnel/`realizeDescriptor` descriptions, “dumb value
  store”, retention wording) and the stale `stats.h` comment — fixed in
  `3448f4c0`.
- **`MDB_MAP_RESIZED` handling** for the read-only lookups and the initialization
  transaction — fixed in `6ac93d10`. No focused regression test: forcing the
  error needs a second process/environment that grows the map.
- **`MoveUpdatesParID`** — `Item::moveItem` now takes the destination parent
  and updates `parID_` after the host move succeeds; `ItemCache::moveItem` passes
  the new parent. `MoveUpdatesParID` is enabled and passes.
- **Host-path hidden filtering** — `realizeByHostPath` now applies
  `ItemFactory::isHidden`, matching the listing. `HiddenSidecarPathHasNoEmptyFSSpecName`
  is enabled and asserts that the sidecar is not addressable.
- **Zero-length FSSpecs** — `nativePathToFSSpec` now returns `nullopt` when the
  item has no decided name. `UnreadableParentHasNoEmptyFSSpecName` is enabled.
  `Item::name()` still has no failure signal (the remaining API limitation above).
- **Directory sidecars** — directory delete now removes hidden children and its
  own `._name`/`%name` companions; directory move moves those companions. The
  four corresponding sidecar tests are enabled and pass. File AppleDouble
  companions follow Decision 16: only the winning scheme is owned.
- **`PBCatMove` FCB state** — open FCBs now get the new parent and name, matching
  `PBHRename`. `CatMoveUpdatesOpenFCB` is enabled and passes.
- **Probe staleness on move** — `Item::moveItem` now calls `pathChanged()`, so a
  path change marks the rep's probe trace stale. No direct test (white-box).
- **Deleted Item LRU retention** — `ItemCache::deleteItem` now drops the item from
  the LRU after the host delete. No direct test (needs an LRU accessor).
- **Empty listed-directory weight** — a listed empty directory now weighs 0
  instead of 1. No test.
- **CNID test layering / encapsulation** — policy tests now use the in-memory
  backend and public `ItemCache`/`DirectoryItem` APIs; the mapper contract,
  persistence and transaction tests are separate. `lookupPosixName` was removed,
  and the methods used only inside `ItemCache` (`getOrCreateChild`, `deleteCNID`,
  `moveCNID`) are private. The methods `DirectoryItem` drives
  (`mapDirectoryContents`, `lookupCNID`, `cnidForName`, `nameGroup`) remain the
  public collaborator seam; the tests do not call them.
- **CNID policy ownership** — `CNIDMapper` remains the value store (primitives
  plus one `withTransaction` seam), and policy lives in `ItemCache`.
  Reconciliation and naming are no longer duplicated across the two backends.
- **Access-by-CNID / binding-retry coverage gap** — closed by
  `DirectoryRenameRecoveredByCnidAccess` (enabled; passes). It also covers the
  previously indirect binding-failure/re-list/retry path.
- **`realizeDescriptor` vs `realizeByCNID`** — closed by aligning the plan text
  with the code: the listing realizes from its stamp-validated descriptor and
  does not repeat the binding check.
- **`toMacRoman` vs `toMacRomanFilename`** — the grouping-key formula above was
  corrected to use `toMacRomanFilename(posixName, 0)`. The code already did
  this; raw `toMacRoman` would under-group `:`/invalid-encoding names and break
  lookup by the displayed name.

The remaining open-point test is in `tests/localvolume_ops.cpp` and can be run
with:

```sh
./build/tests/tests --gtest_also_run_disabled_tests \
  --gtest_filter='*StatFailureDoesNotRenumberChild*'
```
