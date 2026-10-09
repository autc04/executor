# LMDB / lmdbxx version upgrade and `LMDBCNIDMapper` re-enable

Date: 2026-10-10

## What changed

- `LocalVolume` now uses `LMDBCNIDMapper` instead of `SimpleCNIDMapper`
  (`src/file/localvolume/localvolume.cpp`). The persistent CNID map is now the
  default backing store; the `SimpleCNIDMapper` include/comment is left in place.
- The `lmdb` submodule was moved from `mdb.master` @ `22af328` (2019-02-17,
  self-reported version `0.9.70`) to the release tag `LMDB_1.0.2`
  (`c279e047`, 2026-09-08).
- The `lmdbxx` submodule is otherwise unchanged in version, but it now carries
  a local commit `af9f90b` ("prevent double transaction abort") fixing
  `txn::commit()`; see "Crash on first real use" and "lmdbxx" below. It is
  ahead of `origin/master` by one commit and has not been pushed.

## Test results

`ExecutorDirectoryMap` is pointed at a throwaway directory (the tests use the
real `~/.executor/cnidmap` otherwise). Filter:
`-R "FileTest|^Files|LocalVolumeFixture|MacBinaryFileTest"`.

| Setup | Result |
|---|---|
| baseline (`SimpleCNIDMapper`, lmdb @ 22af328) | 60/62 pass; only the 2 expected `xfail`s fail |
| `LMDBCNIDMapper`, lmdb @ 22af328 | same |
| `LMDBCNIDMapper`, lmdb @ LMDB_1.0.2 | same |

No regressions. (`FileTest.SetFInfo_CrDat` and `FileTest.SetFLock` are labelled
`xfail`.)

## Crash on first real use (Browser) — `lmdb++` `txn::commit()` bug

Starting Executor with no application (so it launches Browser) segfaulted in
`mdb_page_get`. The crash reproduced with *both* LMDB `22af328` and `LMDB_1.0.2`,
so it is **not** a 1.0 regression: it is a latent bug exposed by finally enabling
`LMDBCNIDMapper` (Browser recursively enumerates the whole host tree, which fills
the default 1 MB map and drives the `growMapIfNecessary` retry path).

Chain of events:

1. `LMDBCNIDMapper::updateDirectoryContents` fills the map; `mdb_txn_commit()`
   returns `MDB_MAP_FULL`. Its `fail:` label calls `mdb_txn_abort(txn)`, which
   ends the preallocated `env->me_txn0` **without** freeing it
   (`mdb_txn_end()` sets `mode = 0` when `!txn->mt_parent`) and sets
   `MDB_TXN_FINISHED`.
2. `lmdb++`'s `txn::commit()` did `lmdb::txn_commit(_handle); _handle = nullptr;`
   so when `txn_commit` throws, `_handle` stays set.
3. Unwinding destroys the `lmdb::txn`, whose destructor calls `mdb_txn_abort()`
   a **second** time. `mdb_txn_end()` now takes the `MDB_TXN_FINISHED` early-out,
   which skips the `mode = 0` reset that protects `me_txn0`, and therefore
   `free()`s `me_txn0` (`mdb_txn_end`, `if (mode & MDB_END_FREE) free(txn);`).
4. `growMapIfNecessary` catches the map-full error, grows the map, and retries;
   `mdb_txn_begin` reuses the now-dangling `env->me_txn0`, whose first field
   (`mt_parent`) has been overwritten by the allocator. The next
   `mdb_page_get` walks the bogus `mt_parent` dirty-list and crashes.

Fix (root cause, in the fork): make `txn::commit()` exception-safe by releasing
the handle before the possibly-throwing call:

```cpp
void commit() {
    MDB_txn* const handle = _handle;
    _handle = nullptr;
    lmdb::txn_commit(handle);
}
```

This is a genuine `lmdb++` bug (present in upstream `bendiken/lmdbxx` and in the
fork) and would bite any caller that grows-and-retries on `MDB_MAP_FULL`.

## Upstream versions (as of 2026-10-10)

### LMDB (`github.com/LMDB/lmdb`)

There are now two maintained release lines:

| Line | Latest release | Date | Version macro |
|---|---|---|---|
| 0.9.x (`mdb.RE/0.9`) | `LMDB_0.9.36` | 2026-08-06 | 0.9.36 |
| 1.0.x (`mdb.RE/1.0`) | `LMDB_1.0.2` | 2026-09-08 | 1.0.2 |

Development branches `mdb.master` (0.9.70) and `mdb.master3` (0.9.90) both
received commits on 2026-10-09; the two `mdb.RE` branches are the release
maintenance branches.

**We pinned `LMDB_1.0.2` (newest release).** The public API is *additions-only*
relative to 0.9 (`mdb_env_set_checksum`, `mdb_env_set_encrypt`,
`mdb_env_incr_*`, `mdb_txn_prepare`, `mdb_env_set_pagesize`, `mdb_size_t`,
new error codes/flags); no symbol was removed, and the default `mdb_size_t` is
`size_t`. `lmdb/libraries/liblmdb/{lmdb.h,mdb.c,midl.c,midl.h}` still build
standalone as `src/CMakeLists.txt`/`CMakeLists.txt` compile them (the new
`crypto.c`/`chacha8.c`/`module.c` files are optional and not needed for the
core library).

**Caveat — on-disk format break.** LMDB 1.0 changed the file format; 0.9 and
1.0 files are mutually incompatible (`libraries/liblmdb/upgrading.doc`).
Consequences for Executor:

- An existing `~/.executor/cnidmap/data.mdb` written by a 0.9 build cannot be
  opened by 1.0: `mdb_env_open()` returns `MDB_VERSION_MISMATCH`, which surfaces
  as an uncaught `lmdb::error` when the volume is mounted.
- The CNID map is a regenerable cache (`docs/subsystems/local-volume.md` already
  treats deleting it as acceptable, at the cost of stale saved FSSpecs), so the
  fix is to delete the old `cnidmap` directory (or migrate with a 0.9
  `mdb_dump` + 1.0 `mdb_load`). Executor currently has no migration/rebuild
  handling for this.

If a drop-in upgrade without the format break is preferred, pin `LMDB_0.9.36`
instead; it is the same on-disk format and API as before.

### lmdbxx (`github.com/autc04/lmdbxx`, fork of `github.com/bendiken/lmdbxx`)

- Upstream (`bendiken/lmdbxx`) is **unmaintained**: last commit `0b43ca8`,
  2016-02-29.
- The fork (`autc04/lmdbxx`) `master` is `33f271a` (2018-10-08), **two commits
  ahead** of upstream (`bd7b412`, `33f271a`), and is exactly the commit already
  pinned. There is nothing to merge or update.
- `lmdb++.h` compiles cleanly against the LMDB 1.0.2 header, so **lmdbxx does
  not need to be abandoned** in favour of the C API.
- A one-function fix was applied to the fork's `lmdb++.h` (`txn::commit()`
  exception safety, see above), committed locally as `af9f90b`. It is ahead of
  `origin/master` by one commit and needs to be pushed to `autc04/lmdbxx` to
  persist.

## References

- `src/file/localvolume/localvolume.cpp` — mapper selection.
- `src/file/localvolume/lmdbcnidmapper.cpp` / `.h` — LMDB mapper.
- LMDB `libraries/liblmdb/upgrading.doc`, `CHANGES`.
