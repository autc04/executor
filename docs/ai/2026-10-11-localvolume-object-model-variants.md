# LocalVolume: two object-model / cache variants, for comparison

Status: **exploration only** — no decision taken. Companion to
`2026-10-08-localvolume-staged-materialization-plan.md` (the *main plan*), which
fixes the *stages* but leaves the **object graph** and the **cache** unspecified:
where the parent→child relation lives, what keeps an `Item` alive, and what
"eviction" means.

> **Naming.** These are unrelated to the "Variant A / Variant C" in the main
> plan's *Discarded alternatives* — those were about fusing vs. splitting the
> *format* concern. To avoid confusion they are called here **Variant A —
> link-free Items** (the mapper owns the graph) and **Variant B — rooted tree,
> leaf-pruned** (the `Item` graph owns the tree). They are variants of the same
> size as the main plan's object model: picking one reshapes Phase 4 onward.

---

## 1. Shared ground

Both variants inherit from the main plan and are not differentiated by these:

- **Identity is path-keyed** in `CNIDMapper`, keyed by `(parID, posixName)`;
  re-seeing an entry reuses its CNID and name (Decisions 3, 8).
- **Name disambiguation is batch-required by group.** A name must avoid conflicts
  with every sibling that can map onto the same Mac name, so a `macNamePrefix`
  group must be considered together (Decisions 4, 5). Resolve is transactional
  (Decision 9).
- **Count, Enumerate, Materialize** stay distinct (the main plan's stage model);
  `FileRep` stays the format seam (Phase 3).
- **Validation stamps** `(mtime, size)` from a raw `stat` stay the staleness
  mechanism (Phase 5).
- **Path resolution must not enumerate ancestors.** At most the *immediate*
  parent is scanned; the ancestors above it are not.
- **CNIDResolve is separated from NameResolve** (§2). Deciding an entry's CNID
  needs only its `(parID, posixName)`; deciding its `macName` needs its
  `macNamePrefix` group. Keeping them apart is what lets an ancestor be addressed
  and descended *without* listing or naming it, so it is **required by both
  variants**, not a differentiator. (The mapper's tree is rooted, so if identity
  creation entailed naming, descending it would drag a listing of every parent
  behind it.)
- **Sidecars are hidden by name; content-derived names are dead** (Decisions 1, 2).
- Retention is by **strong `shared_ptr`**; `ItemCache::items_` is a
  `cnid -> weak_ptr<Item>` registry, never a strong owner.

## 2. Vocabulary

The main plan's stage names are kept; two are added for the CNIDResolve /
NameResolve split, which both variants need (§1).

| Term | Meaning |
|---|---|
| **Instantiated** | an `Item` object exists (in `items_` and/or owned by the tree). |
| **CNIDResolved** | the entry has a `CNID` in the mapper. |
| **Named / Resolved** | the entry has a decided `macName`. |
| **Counted** | a directory's visible-child tally is known (one `readdir`; no children). |
| **Enumerated** | *all* of a directory's children are Instantiated. |
| **Materialized** | a `FileItem` has its `FileRep` (Phase 3). |

A **descriptor** is a retained per-entry `readdir` result:
`(posixName, kind, macNamePrefix)`, plus an optional **lazily populated CNID**
once the entry is CNIDResolved. Descriptors are kept sorted by
`(macNamePrefix, posixName)` — the group key first — which makes a
`macNamePrefix` group a contiguous range (found by binary search) and defines the
listing order. A descriptor is not an `Item` (no object), not a
`CNIDMapper::Mapping` (no identity ownership), and not an `fs::directory_entry`
(no cached stat). A directory stores descriptors when it wants a listing it can
reconstruct — and each entry realize — on demand, without holding child `Item`s.

The main plan currently folds CNIDResolve into Instantiate and makes `macName`
mandatory (Decisions 3, 4: "assigned together"). **Both variants split them**
(§1): an entry can exist CNIDResolved but not yet Named. The variants differ only
in *where* the name lives (the mapper record vs. the `Item`) and *who* triggers
NameResolve. The single axis that separates A and B is **where parenthood lives**.

---

## 3. Variant A — link-free Items

**Idea.** Delete the parent→child links between `Item`s. The relationship is
already expressed by `CNIDMapper` (parID chains) plus the weak `items_` registry,
so a `DirectoryItem` does not need to own its children.

### 3.1 Object graph and retention

- No `Item` holds another `Item`. `DirectoryItem::contents_/files_/contents_by_name_`
  (strong child vectors) are **removed**.
- A `DirectoryItem`'s enumeration payload is a **descriptor list** (§2), sorted
  by `(macNamePrefix, posixName)`, with a **lazily populated CNID cache** — a
  resolved CNID per descriptor once that entry is CNIDResolved. One ordered list
  serves three jobs: the listing order for `resolve(index)` (the descriptor order
  *is* the catalog order), the `macNamePrefix` group for lazy NameResolve (a
  contiguous range, found by binary search — no separate group index), and the
  CNID source for realized children (no separate ordered child-CNID list).
- **Realization registry**: `items_` (weak) as today; **plus** one LRU of
  `Item`s — files *and* directories — that holds the strong reference.
- An `Item` is alive iff it is in the LRU or externally referenced (an open FCB,
  a local `ItemPtr`). Nothing else retains it.
- **Identity and name lookups delegate to the mapper**, which stores a record per
  CNID plus the indexes it needs: a global `(dev, ino) -> cnid` (identity), a
  per-directory child set `parID -> {cnid}` (reconciliation and names-in-use), and
  `(parID, macName case-folded) -> cnid` (name lookups; populated by NameResolve,
  and a miss triggers NameResolve of the group). There is deliberately **no
  `(parID, posixName) -> cnid` index** (§3.3, §3.8): only path resolution wanted
  one, so it resolves components stat-first instead. `cnid -> Item` realizes from
  the record.

### 3.2 Stages

- Mapper record: **CNIDResolved** (`cnid, parID, posixName`), and — once the
  group has been NameResolved — **Named** (`macName`). The two happen at
  different times.
- `DirectoryItem`: **Counted** (tally only) or **Listed** (descriptor list +
  lazy CNID cache).
- `FileItem`: **Materialized** (rep).
- *Realization is not a stage* — it is a cache event. Any mapping can be turned
  into an `Item` on demand and dropped arbitrarily without consequence, because
  the `Item` is a pure function of the mapper record plus live host state.

So a realized `Item` may be CNIDResolved but not yet Named; reading its name must
trigger NameResolve. That is the same undecided-name obligation as Variant B
(§1), and the reason the split is listed as shared rather than B-specific.

### 3.3 Path resolution (`nativePathToFSSpec` on `/X/Y/z`)

1. Seed at the root Item (CNID 2).
2. For each parent component `X`, `Y`: **stat** `parent/comp` for `(dev, ino)`
   and look it up in the global `(dev, ino) -> cnid` index (**CNIDResolve**, no
   name); create a record under the parent's CNID on a miss. No listing, no
   siblings, no names. Realize the `DirectoryItem` (LRU) as needed.
3. For the last component `z`: the FSSpec needs its `macName`, so make the
   immediate parent's **descriptor list** available (one `readdir`,
   hidden-filtered, grouped by `macNamePrefix`) and run **NameResolve** over
   `z`'s group; realize `z`.
4. No sibling `Item` is realized.

Path resolution is the only consumer of a posix-name-to-child lookup, and it
already stats every component (for existence and kind), so folding the identity
lookup into that same stat costs nothing and removes the need for a
`(parID, posixName)` index (§3.8). Step 2 is also why CNIDResolve must be
separable from NameResolve: without it, resolving a parent would require naming
it, which needs its own parent's listing, and so on — back to enumerating every
ancestor.

### 3.4 Staleness and identity

Three staleness scopes, each answering a different question with its own signal:

| Scope | Question | Signal | Lives in |
|---|---|---|---|
| Directory contents | are `dir`'s children/names current? | dir `(mtime, size)` | `ensureEnumerated` L0 |
| File representation | is a file's `FileRep` current? | file `(mtime, size)` | `FileItem` rep stamp |
| **Identity** | is `(cnid ↔ path)` still the same *file*? | file `(dev, ino)` | realization |

The directory stamp only runs on enumeration, so the lookups that deliberately
bypass it — `CNID -> Item` and path resolution — need the third check. It is
nearly free: realization already stats the path to decide its kind, so widen
that one stat to also return
`(kind, dev, ino)` and compare `(dev, ino)` and the kind against the mapper
record. The mapper stays a dumb store returning `(dev, ino)`; the comparison and
the policy live in `ItemCache`. The outcome is *valid* (regardless of
`(mtime, size)`), *gone* (`ENOENT`, the name vanished), or *replaced* (a
different file now holds the name). A mapping check can only validate an existing
binding, never discover a new entry, so enumeration remains the bulk path and the
miss fallback; the check is what lets a *hit* be served without enumerating.

**Decision: identity is keyed by inode, not by name.** A `CNID` follows the
*file*:

- A rename — host-side or through the mapper — keeps the `CNID`. Host-side renames
  are recovered from the `(dev, ino) -> cnid` index (§3.8); until that recovery
  path is wired up, a binding whose path vanished is treated as a delete.
- A delete-and-recreate at the same name is a **new** `CNID`, since the inode
  differs. This supersedes the main plan's current name-keyed rule, where a kind
  change at one name reuses the `(parID, posixName)` identity and only rebuilds
  the `Item` type.
- **Hard links** are the caveat: one inode can carry several names, so
  `(dev, ino)` is not injective and two Mac entries would share a CNID. Give
  inode identity only to single-link files; for `nlink > 1` key identity on
  `(dev, ino, posixName)` (distinct per name, no rename tracking). That is why
  `nlink` is stored beside `(dev, ino)`. This is not hypothetical for the target
  volume: `/nix/store` can contain hard-linked files.
- **Inode fidelity and reuse.** Some filesystems/mounts supply no usable inode
  (`d_ino == 0`); identity then needs a name-keyed fallback. And a *reused* inode
  (a file deleted and a new one allocated the same inode before re-enumeration)
  will be matched by the diff and reuse the CNID. A name-keyed diff has the same
  class of race on name reuse.

### 3.5 Eviction

- Uniform LRU over all `Item`s. Eviction = drop the LRU's strong ref; the `Item`
  dies if nothing else holds it (`noteItemFreed` clears the weak entry).
- **O(1), no parent coupling.** Evicting a file never touches its parent; a
  `DirectoryItem`'s listing simply goes away with it.
- Root is a normal cached `Item`; "pin the root" is at most a heuristic (the
  volume anchor / `resolveDir(2)`), not a structural need.

### 3.6 Consequences for call sites

- `resolve(index)` walks the descriptor list (its order is the listing order)
  and realizes entries on demand — the listing costs descriptors, not `Item`s.
  The listing order is `macNamePrefix`-major with host path as the tie-break;
  this revises the main plan's Decision 11 (host-path sort order).
- `dir->tryResolve(name)`: mapper `(parID, macName) -> cnid` (requires the index
  the main plan deferred), else fall back to a listing. No `contents_by_name_`.
- `upgradeItem`: swap the rep; the `Item` is not owned by a parent, so nothing
  else changes.
- delete/rename/move: update the mapper (`deleteCNID`/`moveCNID`); invalidate the
  affected listings and any LRU/registry entries. No child vectors to maintain.

### 3.7 Pros / cons / risks

- **Pros.** Very small retention story; eviction is trivial and O(1); memory is
  bounded by the LRU, not by directory shape; F1 is cheapest (no sibling
  realization); maps cleanly onto LMDB (the mapper is the only durable
  structure); one place decides names (the mapper), so identity/name consistency
  is local; the mapper does not index the host-name space.
- **Cons / risks.**
  - Needs the `(parID, macName case-folded) -> cnid` index **and** a staleness
    rule for it (the main plan deferred this precisely because a name lookup can
    hit a renamed host file), plus population/removal on NameResolve and on
    delete/rename/move.
  - The descriptor list must be *ordered* and cheap to re-derive for
    `resolve(index)`, and its lazily populated CNID cache must be invalidated
    consistently with the stamp.
  - NameResolve must run over a group without realizing sibling `Item`s — the
    mapper needs a group entry point (given the group's host names plus
    already-known names, assign names).
  - Undecided names exist between CNIDResolve and NameResolve, so every name
    reader must trigger NameResolve — the same obligation as B.
  - Two caches (LRU + weak registry) plus the mapper: the invariant "an `Item` is
    always re-derivable" must hold, so no per-`Item` state may become load-bearing
    (today only `FileRep` and directory stamps are, and both are reclaimable).
  - The inode is now load-bearing for identity, validation (§3.4), path
    resolution, and the reconciliation diff. On POSIX `readdir`/`stat` supply it
    for free; on Windows `GetFileAttributesEx` does not, so both the stat helper
    and the directory scan need a handle-based variant there.

### 3.8 `CNIDMapper` as a dumb value store

Resolving and staleness are owned by `DirectoryItem` (and, for reps, `Item`),
**not** by the mapper. The mapper stays a value store. It holds:

- **record** per CNID: `{ parID, posixName, macName?, mtime, (dev, ino), nlink }`
  (`macName` optional until NameResolve; `nlink` for the hard-link rule of §3.4).
- **`(dev, ino) -> cnid`** (global): identity, for stat-first path resolution
  (§3.3) and, later, rename recovery.
- **`parID -> {cnid}`** (child set): reconciliation and names-in-use.
- **`(parID, macName case-folded) -> cnid`**: name lookups.

It deliberately does **not** store `(parID, posixName) -> cnid`: the only caller
that wanted a host-name-to-CNID lookup is path resolution, which is stat-first
instead. The consequence is that `lookupChild` / `getOrCreateChild` leave the
interface — there is no name-keyed child lookup — and the reconciliation diff is
re-keyed to the inode.

Two further consequences:

- Swapping backends (`SimpleCNIDMapper` ↔ `LMDBCNIDMapper`) then changes only a
  small value interface instead of duplicating policy in both.
- An operation larger than one lookup — a group NameResolve, a directory diff, a
  move — is driven by the caller and handed to the mapper as a `std::function`
  read-modify-write callback, so the same transaction shape fits both backends.

The mapper interface is **not set in stone**; A assumes it can be redesigned
cleanly rather than grown onto the current `getOrCreateChild` /
`mapDirectoryContents` shape.

---

## 4. Variant B — rooted tree, leaf-pruned

**Idea.** Keep the parent→child strong links (the rooted `Item` tree) and make
the cache **prune leaves from the tree**. Remove `getOrCreateChild`. Split
**CNIDResolve** from **name Resolve** (shared with A, §1) so that converting a
native path neither enumerates ancestors nor names anything prematurely.

### 4.1 Object graph and retention

- `DirectoryItem` keeps `contents_/files_/contents_by_name_` (strong children).
- Retention = the rooted tree: `rootDirItem_` → `contents_` → … ; plus external
  refs (open FCBs).
- Items carry `parID` (CNID), so a child's parent is found via
  `items_[parID].lock()`.

### 4.2 Stages

- `Item` **Instantiated**: identity exists (`parID`, `path`, and `cnid` once
  CNIDResolved) — **`macName` may be undecided**.
- `Item` **Resolved**: `macName` decided (group transaction).
- `DirectoryItem` **Counted** / **Enumerated** (all children Instantiated).
- `FileItem` **Materialized**.

This split is **shared with Variant A** (§1) and revisits the main plan's
Decision 3 ("`CNID` and `macName` are assigned together; neither is optional").
B's difference from A is only *where* the undecided state lives — on the live
`Item` here, versus in the mapper record for A. In both, every reader of the
name must force Resolve; a missed site observes an undecided name. (This is
distinct from the rejected "provisional name", which was *decided but wrong*.)

### 4.3 Path resolution (`nativePathToFSSpec` on `/X/Y/z`)

1. Seed at the root Item.
2. For each parent component `X`, `Y`: **Instantiate** the `DirectoryItem` and
   **CNIDResolve** it (assign its CNID from the mapper); do not enumerate it.
3. **Enumerate** the immediate parent (`Y`): one `readdir`, instantiate *all*
   its children, CNIDResolve each. The target `z` is now a real child `Item`.
4. **Resolve** the group that can conflict with `z` (`macNamePrefix`) — decide
   `macName`s for the group.
5. Return the target `Item`.

`getOrCreateChild` is gone: the target is never created standalone; it comes out
of step 3. Ancestors only need their CNIDs (CNIDResolve), not enumeration.

### 4.4 Eviction

- The LRU **prunes leaves** of the tree: detach a leaf (a `FileItem`, or an
  emptied directory) from its parent's `contents_/files_/contents_by_name_`,
  then drop it from `items_`.
- A directory is pruned only after its children (post-order), so a parent that
  still has children is never a leaf and stays.
- Root is naturally retained (never a leaf unless empty).
- Protection: an `Item` referenced by an open FCB must not be pruned; pruning a
  directory subtree must respect that.
- Memory is bounded by total tree size, charged as today by item count.
- Finding the parent to detach from needs `parID -> Item` (the weak registry),
  which is present because the parent is on the tree.

### 4.5 Consequences for call sites

- `resolve(index)` / `dir->tryResolve(name)`: unchanged shape — the tree is the
  listing; but both must trigger **group Resolve** before a name is read.
- A generous sweep is needed over `Item::name()` readers (listing, `getInfo`,
  `openCommon`/`setFCBName`, `FSSpec` construction) to guarantee Resolve.
- delete/rename/move: update the mapper and detach/attach the child `Item` in the
  parent's vectors; drop the affected directories' enumeration.
- `upgradeItem`: unchanged (rep swap on an owned `Item`).

### 4.6 Pros / cons / risks

- **Pros.** Keeps the current, well-understood structure (the tree *is* the
  directory listing); `resolve(index)` and `contents_by_name_` stay trivial once
  Resolved; F1 is fixed by CNIDResolve-only ancestors; invalidation is scoped to
  the affected directory.
- **Cons / risks.**
  - Undecided names on live `Item`s → every name reader must force Resolve; a
    missed Resolve is a silent wrong-name bug. (Shared with A, but A keeps the
    name in the mapper record rather than on the `Item`.)
  - **Rootedness vs. "don't enumerate ancestors" is in tension.** Step 2
    Instantiate+CNIDResolve creates ancestors that are *not* Enumerated, so they
    cannot be attached as children of their own (un-enumerated) parent without
    either
    **(i)** allowing *sparse* `contents_` (partial, non-`Enumerated` child sets —
    which weakens the all-or-nothing invariant and complicates `resolve(index)`
    and Count), or
    **(ii)** keeping unattached `Item`s in the weak registry (i.e. re-introducing
    Variant A's second store for exactly the path-formed ancestors).
  - Leaf pruning must reach the parent and must not prune open items; error-prone
    around move/rename.
  - Cross-instance (Phase 6): both the mapper and the live tree must be kept
    consistent, so there is more invalidation surface than A.

---

## 5. Comparison

| Aspect | A — link-free | B — rooted tree |
|---|---|---|
| Parent→child relation | `CNIDMapper` | `Item` tree (`contents_`) |
| Child `Item` ownership | none (LRU only) | parent holds children |
| Realization store | weak `items_` + one LRU of all `Item`s | weak `items_` + tree |
| Eviction unit | any `Item`, O(1) | leaf, post-order |
| Root special-casing | heuristic only | structural (tree root) |
| `macName` storage | mapper record + `(parID,macName)` index | on the live `Item` |
| `(parID, macName)->cnid` | required, case-folded (path lookup is stat-first) | not required (`contents_by_name_`) |
| Undecided names | exist (mapper record without a name) | exist (`Item` without a name) |
| F1 immediate parent | descriptor list + group NameResolve (no sibling `Item`s) | full Enumerate (all sibling `Item`s) |
| Directory payload | descriptors sorted by `(prefix, path)` + lazy CNID cache | live `Item` vectors |
| Listing by index | ordered descriptors, realize on demand | `contents_` as today |
| Memory shape | LRU-bounded, O(1) evict | tree-shaped, leaf-pruned |
| `upgradeItem` | rep swap, nothing else | as today |
| Phase 6 (LMDB) fit | clean (mapper is the only durable state) | mapper + tree must agree |
| Main risk | new `(parID,macName)` index + descriptor/CNID-cache staleness | rootedness/partial-enum tension + leaf pruning |
| Revisits Decision 3? | yes (shared) | yes (shared) |

Both variants share the same *cost model* for F1 and F3 (ancestors are never
enumerated; counts come from Count), the same CNIDResolve/NameResolve split
(§1), and the inode-keyed identity decision (§3.4); the table's naming rows
differ only in *where* names live.

---

## 6. Open questions

Shared:

- **Undecided names.** The CNIDResolve/NameResolve split is shared (§1), so both
  variants can hold an entry that is CNIDResolved but not Named. Where exactly is
  "ensure Resolved before reading a name" enforced, and can it be made structural
  rather than a duty on every read site?
- Does the immediate parent always have to be *fully* listed? Correctness only
  demands the target's `macNamePrefix` group be complete; listing the whole
  directory is the simple superset. (A lists descriptors; B Enumerates.)

Per-variant:

- **A.**
  - The mapper's `(parID, macName case-folded)` index: how is it kept stale-safe,
    and is it populated by NameResolve or maintained eagerly?
  - Descriptor list with a lazily populated CNID cache: what invalidates a cached
    CNID when the directory changes?
- **B.**
  - How are path-formed ancestors attached to the tree: sparse `contents_`, or
    an unattached registry (A's second store)?

---

## 7. Relationship to the phased plan

- **Phases 0–3** (test harness, `FileRep`, deferred Materialize) are agnostic and
  stay.
- **Phase 4** is reshaped by the choice, on top of the shared CNIDResolve /
  NameResolve split (§1):
  - A: `DirectoryItem` gains a descriptor list; identity and name lookups
    delegate to a redesigned mapper; Phase 1's `getOrCreateChild`-shaped path is
    replaced by mapper-delegated lookups and a group NameResolve.
  - B: `Resolve` becomes a *tree* operation and Phase 1's `getOrCreateChild` is
    removed.
- **Phase 5** (stamps, Count cache, rep revalidation) is agnostic and stays; only
  the memory-bound step's *unit* differs (uniform LRU vs. leaf pruning).
- **Phase 6** (LMDB, transactions, multi-instance) favours A's shape; under B it
  must also reconcile the live tree.
- **Phase 7** (inode rename tracking) is agnostic.

## 8. Decision criteria (not a recommendation)

- How much new mapper surface is acceptable — A adds a case-folded
  `(parID, macName)` index (and a redesign of the interface); B adds none.
- Where the undecided `macName` lives (mapper record vs. `Item`) and how the
  "ensure Resolved before reading a name" obligation is discharged. (The split
  itself is shared, so this is a placement question, not an A/B one.)
- Whether the tree's listing semantics (`resolve(index)`, `contents_by_name_`)
  are worth keeping versus deriving listings from descriptors.
- How much Phase 6 sharing matters now (A fits it best).
