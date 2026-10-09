# LMDB / lmdbxx version upgrade and `LMDBCNIDMapper` re-enable

Date: 2026-10-10

## What changed

- `LocalVolume` now uses `LMDBCNIDMapper` instead of `SimpleCNIDMapper`
  (`src/file/localvolume/localvolume.cpp`). The persistent CNID map is now the
  default backing store; the `SimpleCNIDMapper` include/comment is left in place.
- The `lmdb` submodule was moved from `mdb.master` @ `22af328` (2019-02-17,
  self-reported version `0.9.70`) to the release tag `LMDB_1.0.2`
  (`c279e047`, 2026-09-08).
- The `lmdbxx` submodule is unchanged; see "lmdbxx" below.

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

## References

- `src/file/localvolume/localvolume.cpp` — mapper selection.
- `src/file/localvolume/lmdbcnidmapper.cpp` / `.h` — LMDB mapper.
- LMDB `libraries/liblmdb/upgrading.doc`, `CHANGES`.
