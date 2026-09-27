---
name: executor-file-manager
description: Use when diagnosing File Manager behaviour in Executor, especially differences between an HFS disk image and a LocalVolume (a mounted host directory) — invalid refnums, missing or wrong file names, wrong directory IDs, or an application that works on one volume type but not the other.
---

# File Manager in Executor (HFS images vs LocalVolumes)

Executor has two file-system backends, and app-visible bugs frequently come from
one of them doing something the other does not:

- **HFS** — a volume inside a disk image.
- **LocalVolume** — a host directory exposed as a Mac volume.

Use the `executor-guest-debugging` skill for the mechanics (`--logtraps`,
`--debug trapfailure`, trap/address breakpoints, stepped traces).

## Reproducing against each backend

```
# LocalVolume (host directory)
./build/Executor\ 2000.app/Contents/MacOS/Executor\ 2000 --headless "/path/to/Dir/App"

# HFS image
MacVolumes="/path/to/Disk.image" \
  ./build/Executor\ 2000.app/Contents/MacOS/Executor\ 2000 --headless "Volume:App"
```

If a bug reproduces on one backend but not the other, treat it as a backend
divergence and compare the two code paths (below).

## Model

- **FCBs** live in one shared table, `ROMlib_fcblocks` (`src/file/file.h`).
  Refnums encode position: `refnum = 2 + index * 94` (the FCB stride is
  `FSFCBLen == 94`), and `ROMlib_refnumtofcbp()` validates a refnum.
- **Dispatch** (`src/hfs/hfsXbar.cpp`) routes each trap to a backend by
  inspecting the FCB's VCB (`getFileVolume`, `hfsfil`/`hfsvol`). A LocalVolume
  is identified by the `volume` pointer on its VCB; HFS by a non-zero `vcbCTRef`.
- **Names and IDs**: files carry a catalog node id (CNID); `ioDirID` / `ioFCBParID`
  and similar fields appear throughout the parameter blocks.
- **Parameter blocks** (generated, see `build/src/api/FileMgr.h`): `IOParam`,
  `FileParam` / `HParamBlockRec`, `CInfoPBRec`, `FCBPBRec`, `WDPBRec`.

## Where errors come from

| Error | Meaning | Source |
|---|---|---|
| −51 `rfNumErr` | refnum does not name an open FCB | `ROMlib_refnumtofcbp`; `pbtofcbp`, `hfsPBClose`/`hfsPBFlushFile`, `PRNTOFPERR`, `PBGetFCBInfo` |
| −50 `paramErr` | LocalVolume's own refnum check rejected it | `LocalVolume::getFCBX` |
| −35 `nsvErr` | no backend matched the refnum | dispatcher (`hfsXbar.cpp`) |

For −51 in particular, `--debug trapfailure` prints the exact `file:LINE` that
produced it, which usually identifies the site immediately.

## Comparing the two backends

The backends implement the same traps separately, so a field that one fills may
be missing in the other. When a trap's result differs, compare field by field:

- **Open path** — `LocalVolume::openCommon` vs HFS `PBOpenHelper`: every FCB
  field HFS sets (`fcbCName`, `fcbDirID`, `fcbFlNum`, fork/flags bits, …) must
  be set here too, or later `PBGetFCBInfo` / `GetFCBInfo` results differ.
- **Info path** — `LocalVolume::getInfoCommon` vs HFS `PBFInfoHelper` /
  `cathelper`: type/creator, sizes, dates, directory id, openness attribute bits.
- **Flag composition** — HFS ORs some catalog flags (e.g. inherited Finder-flag
  bits) that LocalVolume may omit.

## Traps worth tracing

`PBHGetFInfo`, `PBGetFInfo`, `PBGetCatInfo`, `PBGetFCBInfo`,
`PBOpen` / `PBHOpen` / `…DF` / `…RF`, `HOpenResFile`, `PBGetVInfo`,
`PBGetWDInfo`.

## Docs

`docs/subsystems/file-manager.md`, `local-volume.md`, `hfs.md`.
