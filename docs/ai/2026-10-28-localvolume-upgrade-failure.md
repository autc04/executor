# LocalVolume format upgrades: failure handling

Date: 2026-10-28

## Problem

`LocalVolume::upgradeItem` upgraded a plain rep (triggered by `setInfo`, `write`
or `setEOF` throwing `UpgradeRequiredException`) in the wrong order: it first
dropped `fcbx.access` on every open FCB for the item, then called
`betterFactory->createFile()`. Creation could fail silently —
`PlainDataFork(path, create_t)` kept `fd = -1`, the `AppleSingleDoubleFile`
create constructors ignored `write` results, `MacResourceFork` ignored both
`open` and `fsetxattr` — so `rematerialize()` selected `PlainRep` again and the
caller's retry threw `UpgradeRequiredException` a second time. Because that
exception derived *privately* from `std::runtime_error`, `handleExceptions`
could not catch it, so it escaped the trap boundary and terminated the emulator.
Meanwhile the FCBs for the file had lost their access objects and would have
dereferenced null on the next read/write.

## Fix

- Constructors and I/O no longer fake success: `PlainDataFork`'s permission and
  create constructors throw `OSErrorException` when `open` fails,
  `PlainDataFork::write` throws instead of returning `(size_t)-1`, the
  `AppleSingleDoubleFile` create constructors check their writes, and the Mac
  and Basilisk creators check their host calls. `hostError(errno)` maps
  `ENOSPC`/`EDQUOT`/`EFBIG` to `dskFulErr`, `EROFS` to `wPrErr`,
  `EACCES`/`EPERM` to `permErr`, `ENOENT` to `fnfErr`, and the rest to `ioErr`.
  A partially created sidecar is removed.
- `AppleDoubleItemFactory::createFile` keeps opening the primary (without
  truncation) as a deliberate write-access test: a read-only file must not
  acquire a resource fork. The Mac backend already behaves that way, because
  its resource fork is an xattr on the primary.
- `upgradeItem` now (1) creates the storage, (2) asks the same factory's
  `createRepForFile` to confirm the new marker is there, (3) stashes the old
  fork objects without destroying them, (4) swaps the rep and checks it is no
  longer plain, (5) opens all replacement forks, (6) commits. Any failure
  restores the old forks and propagates an `OSErrorException`; the guest sees an
  error (e.g. `ioErr`) instead of terminate. Keeping the old forks alive across
  the swap is safe because the only upgrade source is `PlainRep`, whose forks
  (`PlainDataFork`, `EmptyFork`) are self-contained.
- `UpgradeRequiredException` now derives publicly from `std::runtime_error`, and
  each retry site converts a second occurrence to `OSErrorException(ioErr)`, so
  it can never escape the trap boundary.

## Tests

`tests/localvolume_ops.cpp`:

- `FailedUpgradeReportsErrorAndLeavesPlainRep` — a directory named `%doc` makes
  the sidecar `open` fail with `EISDIR`; `fmSetType` returns `ioErr`, the type
  stays the plain default, and the directory is left alone. (Non-Apple only:
  on macOS the upgrade target uses xattrs.)
- `FailedUpgradeKeepsOpenFcbUsable` — the same failure with an open read/write
  FCB: the refnum can still be read and closed afterwards.

Full native suite: `ctest --test-dir build -LE xfail` — 221/221 pass.
