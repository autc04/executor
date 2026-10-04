# Design: MacBinary backend for LocalVolume

> **AI-generated.** This document was produced with the assistance of an AI
> language model and may contain inaccuracies. It describes the MacBinary
> LocalVolume backend as implemented on 2026-10-04.

- **Date:** 2026-10-04
- **Status:** implemented (2026-10-04)
- **Area:** `src/file/localvolume/`

## Summary

`LocalVolume` exposes a host file with a `.bin` extension that carries a valid
MacBinary header to the guest as a single Mac file with **both** a data fork and
a resource fork (plus Finder metadata). Previously such a file fell through to
the plain fallback and appeared to the guest as a data-fork-only `TEXT` file.

Detection is gated on the `.bin` extension **and** a full header validity check.
The backend is registered **after every sidecar/xattr-based backend**, so that an
AppleDouble pair describing a text file named `foo.bin` is never parsed as
MacBinary.

## Pre-existing behavior

Before this change, `LocalVolume` built an ordered list of `ItemFactory` objects
(`src/file/localvolume/localvolume.cpp:67-77`). The first factory that returned a
non-null `ItemPtr` for a directory entry won. The order was:

```
DirectoryItemFactory
AppleSingleItemFactory     (magic 0x00051600 / 0x00051607)
AppleDoubleItemFactory     (._foo or %foo sidecar)
BasiliskItemFactory        (.rsrc/ and .finf/ sidecars)
MacItemFactory             (__APPLE__ only; Finder-info / resource-fork xattrs)
ExtensionItemFactory       (default fallback; everything else is a plain file)
```

A `.bin` file fell through to `ExtensionItemFactory` and became a
`PlainFileItem` with an empty resource fork (`openRF` returned `EmptyFork`) and a
hard-coded `TEXT`/`ttxt` FInfo that threw `UpgradeRequiredException` on
`setInfo` with any other type.

## Decisions

1. **Mac-visible name = the host filename (`foo.bin`).** This matches
   `AppleSingleFileItem`, which exposes its single host file under its own name
   (including extension). Restoring the filename embedded in the MacBinary
   header remains deferred (see *Deferred*).
   - There is no provision anywhere for a content-derived Mac name. Names are
     chosen by the CNID mappers from the host filename only
     (`SimpleCNIDMapper::mapDirectoryContents`,
     `src/file/localvolume/simplecnidmapper.cpp:59-70`; the LMDB equivalent at
     `src/file/localvolume/lmdbcnidmapper.cpp:223-228`), with `#N` suffixed by
     `toMacRomanFilename(name, index)` (`src/util/macstrings.cpp:204`) for
     collisions. `ItemCache::cacheDirectory`
     (`src/file/localvolume/itemcache.cpp:69-85`) hands the finished name to the
     factory, so a factory cannot rename an item.
2. **Writes preserve all unmanaged header bytes.** The 128-byte header is read
   into a raw buffer; writes start from those raw bytes so unknown fields (the
   `"mBIN"` signature, secondary-header length, computer/OS-type, comment length,
   …) are written back byte-for-byte. The *managed* fields are additionally
   updated:
   - data fork length `[83..86]`
   - resource fork length `[87..90]`
   - unpacked total length `[116..119]` ← set to `dflen + rflen` (big-endian)
   - header CRC `[124..125]` (see decision 3 for when)
3. **Zero CRC on MacBinary I is accepted without verification.** For detection,
   the CRC is verified only when the reported CRC is non-zero or the file is
   MacBinary II/III (extended version at `[122]` is 129 or 130). On write, the
   CRC is recomputed for MacBinary II/III and for MacBinary I files that carry a
   non-zero CRC; an all-zero CRC is left zero.

## Integration

`MacBinaryItemFactory` is registered **immediately before `ExtensionItemFactory`**
in `LocalVolume::LocalVolume` (`src/file/localvolume/localvolume.cpp:67-77`):

```
DirectoryItemFactory
AppleSingleItemFactory
AppleDoubleItemFactory
BasiliskItemFactory
MacItemFactory             (__APPLE__ only)
MacBinaryItemFactory       ← MacBinary backend
ExtensionItemFactory       (default)
```

Consequences:

- `foo.bin` + `._foo.bin` (or `%foo.bin`) is claimed by `AppleDoubleItemFactory`
  first and is never parsed as MacBinary — the case called out in the request.
- A Basilisk `.rsrc/foo.bin` sidecar, or macOS Finder-info / resource-fork
  xattrs, likewise win.
- `defaultItemFactory` and `upgradedItemFactory` remain **unchanged**: MacBinary
  is never chosen for `PBCreate` (`ExtensionItemFactory` stays the default) and
  is not an `upgradeItem` target.
- No `isHidden()` override is needed (MacBinary is the file itself, not a
  sidecar).

## Detection algorithm

The predicate in `macbinary.cpp` is:

```cpp
bool Executor::isMacBinaryFile(const fs::path& p);
```

Steps (extension gate first, then header; follows Deark's detector):

1. The extension equals `.bin` **case-insensitively** (the factory separately
   requires a regular file).
2. Read a 128-byte header into a raw byte buffer; assemble multi-byte fields with
   explicit big-endian helpers (`getU16BE`/`getU32BE`).  The header is host-file
   data, not guest memory, so no `GUEST<>` wrappers are involved.
3. Require `size >= 128`.
4. Structural checks:
   - `h[0] == 0` — the *original version* byte. It is **always 0** for MacBinary
     I, II and III. (The version is stored at offset 122; the common belief that
     byte 0 is `0x81` is wrong.)
   - `1 <= h[1] <= 63` (filename length) and `h[2] != 0`.
   - `h[74] == 0` and `h[82] == 0` (reserved bytes).
   - Filename bytes `[2, 2+h[1])` contain no control characters; a single
     trailing CR is allowed.
5. Extended version: `h[122]==129 && h[123]==129` → MacBinary II;
   `h[122]==130 && (h[123]==129 || h[123]==130)` → MacBinary III; otherwise
   MacBinary I. All three are accepted.
6. MacBinary III signature `"mBIN"` at `[102..105]` → strong accept (some files
   carry it with earlier version numbers).
7. Fork lengths `dflen = u32be(h[83..86])`, `rflen = u32be(h[87..90])`. Size
   sanity, with Deark's tolerances:
   - `128 + dflen <= size` and `128 + dflen + rflen <= size + 4096` (resource
     overruns are common);
   - `minLen = 128 + pad128(dflen) + (rflen ? rflen : 0)`; `goodLen` is true when
     `size == minLen` or `size == pad128(minLen)`.
8. Version-specific:
   - II/III: secondary-header length `u16be(h[120..121])` must be 0; if
     `[102..115]` is non-zero, `goodLen` is required.
   - I: not both forks empty; if `[99..115]` or `[120..123]` is non-zero,
     `goodLen` is required.  (This is Deark's `[99..123]` check with the managed
     unpacked-total-length field `[116..119]` excluded, so that the backend's own
     writes — which always refresh that field — cannot invalidate detection.)
9. CRC: CRC-16/XMODEM (poly `0x1021`, init 0) over bytes `[0..123]`, stored
   big-endian at `[124..125]`. Verified when the reported CRC is non-zero or the
   file is II/III (decision 3); a mismatch rejects the file.

Because the extension gate already narrows candidates hard, these checks accept
all real variants while keeping false positives very low.

## MacBinary header layout (128 bytes)

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 1 | original version | always 0 |
| 1 | 1 | filename length | 1..63 |
| 2 | 63 | filename | not NUL-terminated |
| 65 | 4 | file type | → `FInfo.fdType` |
| 69 | 4 | creator | → `FInfo.fdCreator` |
| 73 | 1 | finder flags (high) | → `FInfo.fdFlags` |
| 74 | 1 | unused / must be 0 | |
| 75 | 2 | window position v | → `FInfo.fdLocation.v` |
| 77 | 2 | window position h | → `FInfo.fdLocation.h` |
| 79 | 2 | window/folder ID | → `FInfo.fdFldr` |
| 81 | 1 | protected flag | |
| 82 | 1 | must be 0 | |
| 83 | 4 | data fork length (BE) | **managed** |
| 87 | 4 | resource fork length (BE) | **managed** |
| 91 | 4 | creation date (Mac epoch) | → `ItemInfo.creationTime` |
| 95 | 4 | modification date (Mac epoch) | → `ItemInfo.modTime` |
| 99 | 2 | Get Info comment length | preserved |
| 101 | 1 | finder flags (low) | II/III only; preserved |
| 102 | 4 | `"mBIN"` signature | III; else unused; preserved |
| 106 | 10 | unused | preserved |
| 116 | 4 | unpacked total length (BE) | **managed** = `dflen + rflen` |
| 120 | 2 | secondary header length | II/III; must be 0; preserved |
| 122 | 1 | extended version | 0 / 129 / 130; preserved |
| 123 | 1 | extended min version | 129 / 130; preserved |
| 124 | 2 | header CRC (BE, XMODEM over `[0..123]`) | **managed** |
| 126 | 2 | reserved (computer type / OS ID) | preserved |

Fork locations are derived, not stored: the data fork starts at 128, the
resource fork at `128 + pad128(dflen)`. Any Get Info comment / trailing padding
follows the resource fork.

## Classes (`macbinary.h` / `macbinary.cpp`)

Mirrors the AppleSingle/AppleDouble split.

- **`MacBinaryItemFactory : ItemFactory`**
  - `createItemForDirEntry()` — runs `isMacBinaryFile()`; returns a
    `MacBinaryFileItem` or `nullptr`.
  - `createFile()` — not overridden (base throws), since this factory is never
    the default or an upgrade target.
- **`MacBinaryFileItem : FileItem`**
  - caches a `weak_ptr<MacBinaryFile>` like
    `AppleDoubleFileItem::openedFile`.
  - `open(perm)` → `MacBinaryFork(file, Fork::data)`.
  - `openRF(perm)` → `MacBinaryFork(file, Fork::resource)`.
  - `getInfo()` / `setInfo()` — decode/encode the Finder fields.
  - inherits `deleteItem()` / `moveItem()` (a single host file; no sidecars).
- **`MacBinaryFile`** — owns the `std::unique_ptr<OpenFile>` (opened with the
  requested permission) and the raw 128-byte header:
  - `getEOF(Fork)`, `read(Fork, off, buf, n)`, `write(Fork, off, buf, n)`,
    `setEOF(Fork, sz)`.
  - `setEOF` patches the header lengths, shifts the other fork (and any trailing
    comment/padding region) in blocks like
    `AppleSingleDoubleFile::setEOFCommon`
    (`src/file/localvolume/appledouble.cpp:223-315`), rewrites the header, and
    recomputes the CRC per decision 3.
  - `readFInfo` / `writeFInfo` patch only the managed metadata fields and
    preserve the rest of the raw header.
- **`MacBinaryFork : OpenFile`** — thin wrapper forwarding to `MacBinaryFile`
  with a fork selector (analogous to `AppleSingleDoubleFork`).

## Metadata mapping

- `FInfo.fdType` ← `[65..68]`; `fdCreator` ← `[69..72]`; `fdFlags` ← `[73]`;
  `fdLocation` ← `[75]`/`[77]`; `fdFldr` ← `[79]`.
- `ItemInfo.creationTime` ← `u32be(91)`, `ItemInfo.modTime` ← `u32be(95)` —
  already Mac-epoch seconds, the same units `ItemInfo` uses. A stored `0`
  ("unknown") falls back to the host timestamp.
- MacBinary carries no `FXInfo`; `info.file.xinfo` stays zero.
- `setInfo` writes the fields above back into the header (plus low flags at
  `[101]` for II/III) and never needs to throw `UpgradeRequiredException`, so a
  MacBinary item **never triggers `upgradeItem`**.

## Precedence example (the motivating case)

```
foo.bin            data fork (text)
._foo.bin          AppleDouble sidecar
```

`AppleDoubleItemFactory` (position 3) sees the sidecar and claims the pair; the
`MacBinaryItemFactory` at position 6 is never consulted. Even if `foo.bin` also
happens to carry a valid MacBinary header, AppleDouble wins.

## Read/write semantics

- `getInfoCommon` (`localvolume.cpp:456-479`) computes fork sizes by calling
  `open(fsRdPerm)->getEOF()` and `openRF(fsRdPerm)->getEOF()`; for MacBinary these
  read `dflen`/`rflen` from the header, including the resource-fork-only case
  (e.g. `dflen == 0`, `rflen == 515`).
- `PBSetEOF` (`localvolume.cpp:766-778`) → `MacBinaryFork::setEOF` →
  `MacBinaryFile::setEOF`: relocates the other fork and trailing data, updates
  the header, recomputes the CRC.
- `PBSetFInfo`/`PBHSetFInfo`/`PBSetCatInfo` → `MacBinaryFileItem::setInfo`.

## Non-goals

- Creating MacBinary files on `PBCreate`/`PBHCreate` (the default factory stays
  `ExtensionItemFactory`).
- Being an `upgradeItem` target.
- Extracting/decoding files to a directory (this is a volume view, not an
  unpacker).

## Deferred

- **Restore the embedded Mac filename** from the header instead of showing
  `foo.bin`. This is *not* a localized backend change: the name would have to be
  produced before the CNID mappers insert it into their `usedNames` set, with a
  host-name-vs-embedded-name precedence rule, 63-byte/31-char handling, and
  rename round-tripping. Revisit separately.

## Implementation notes

- Total length `[116..119]` is always written as `dflen + rflen` (decision 2b),
  including for MacBinary I; see the detector note in step 8 above for why that
  stays self-consistent.
- CRC is verified at detection time whenever the stored CRC is non-zero.  A
  zero CRC is accepted (decision 3).  On write the CRC is recomputed when the
  file is MacBinary II/III or the stored CRC was non-zero.
- `MacBinaryFileItem` shares one `MacBinaryFile` (weak-cached) between the two
  forks, so a resize through one fork is seen by the other.
- The real-fixture test (`MacBinaryDetection.AcceptsRealFixture`) is guarded by
  `EXECUTOR_SOURCE_DIR`, which `tests/CMakeLists.txt` defines for the native
  `tests` target.

## Files changed

| File | Change |
|------|--------|
| `src/file/localvolume/macbinary.h` | defines the factory, item, file and fork classes plus the detection predicate |
| `src/file/localvolume/macbinary.cpp` | implements detection, header parse/CRC and fork I/O |
| `src/file/localvolume/localvolume.cpp` | includes `macbinary.h` and registers the factory before `ExtensionItemFactory` |
| `src/CMakeLists.txt` | adds `macbinary.h`/`macbinary.cpp` to `file_sources` |
| `tests/localvolume_macbinary.cpp` | new native-only tests |
| `tests/CMakeLists.txt` | adds the test source and defines `EXECUTOR_SOURCE_DIR` for the native `tests` target |
| `docs/subsystems/local-volume.md` | adds MacBinary to the format list and source-file table, and a factory-order gotcha |
| `docs/ai/2026-10-04-macbinary-localvolume-backend.md` | this document |

## Tests

`tests/localvolume_macbinary.cpp`, registered in `NATIVE_TEST_SOURCES` in
`tests/CMakeLists.txt`, contains 18 tests:

- Detection accept cases: both forks, data-only, resource-only, MacBinary I with
  a CRC, and the checked-in real fixture `cxmon/utils/suspend.bin` (a MacBinary
  II file with `dflen == 0`, `rflen == 515`, CRC `0x41ad`).
- Detection reject cases, each expected false: wrong extension, `size < 128`,
  bad name length, non-zero reserved byte, inconsistent fork lengths, corrupted
  CRC, both forks empty.
- `MacBinaryFile` reads: data/resource fork contents and EOF, and `readFInfo`.
- `MacBinaryFile` writes: `writeFInfo` round-trip; `setEOF` growth and shrink on
  one fork preserves the other fork and keeps the header/CRC consistent.

No existing `FileTest` cases are affected: they create plain files through the
default factory.

## Validation

Performed on 2026-10-04:

- `cmake --build build -j8` — clean build (all front-ends plus `tests`).
- `ctest --test-dir build -LE xfail` — 133/133 tests passed, including the 18
  MacBinary tests.
- `clang-format` is not available in the dev shell (`NO_CLANG_FORMAT`), so the
  new sources were matched to `src/.clang-format` by hand (Allman braces, 4
  spaces, right-aligned pointers, no tabs or trailing whitespace).

## References

- Deark `modules/macbinary.c` — authoritative detection heuristic and header
  field offsets (original version byte always 0; extended version at 122).
- Retro68 `ResourceFiles/ResourceFile.cc` — read/write of header, CRC-16 table,
  128-byte fork padding.
- `cxmon/utils/suspend.bin` — real MacBinary II fixture in this repository.
- Just Solve the File Format Problem, "MacBinary"; Wikipedia "MacBinary".
