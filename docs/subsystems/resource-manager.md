# Resource Manager

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

The Resource Manager implements the Mac OS resource fork API: `GetResource`,
`GetNamedResource`, `Get1Resource`, `AddResource`, `WriteResource`, `ReleaseResource`,
`OpenResFile`, `CloseResFile`, and related traps. Resources are typed, identified data
blobs (e.g., icons, strings, menus, code segments) stored in the resource fork of a
file. Executor also uses the Resource Manager internally to load its own built-in
System file (bundled via CMakeRC).

## Key Concepts

**Resource map**: the in-memory representation of one resource fork's directory. A
`resmap` struct contains the offsets to the type list and name list within the resource
map. The type list (`typref` array) enumerates each `ResType` present and the count and
offset of its resources. Each resource is described by a `resref` record holding the
resource ID, name offset, attributes, data offset, and a cached `Handle`.

**Resource chain**: Executor maintains a chain of open resource maps. `GetResource`
walks this chain from front to back, returning the first match. `Get1Resource` only
searches the front-most map (the current application's resource fork). New maps are
prepended to the chain when a resource file is opened and removed when it is closed.

**`resmaphand`**: a `Handle` to a `resmap`. The resource manager stores the current
chain in low-memory global `LM(TopMapHndl)` and the map count in `LM(CurMap)`.

**`resref`**: the per-resource record in the map. `ratr` holds resource attributes
(loaded, purgeable, locked, etc.). `doff` is a 3-byte big-endian offset into the
resource data section. `rhand` is the cached `Handle`; it is null until the resource
has been loaded.

**`ROMlib_mgetres`**: the core load function. Given a map and a `resref`, it reads the
resource data from the file fork into a new Handle and caches it in `resref::rhand`.

**`ROMlib_typidtop`**: searches the chain for a `(ResType, ID)` pair and returns the
owning map and `resref`. This is the hot path for `GetResource`.

## Source Files

| Path | Description |
|------|-------------|
| `src/res/resource.h` | Internal types: `reshead`, `resmap`, `typref`, `resref` |
| `src/res/resGet.cpp` | `GetResource`, `Get1Resource`, `GetIndResource` |
| `src/res/resGetinfo.cpp` | `GetResInfo`, `GetResAttrs`, `SizeRsrc` |
| `src/res/resGettype.cpp` | `CountResources`, `GetIndType`, `CountTypes` |
| `src/res/resOpen.cpp` | `OpenResFile`, `CloseResFile`, `CreateResFile` |
| `src/res/resInit.cpp` | Resource Manager initialisation, System file loading |
| `src/res/resMod.cpp` | `AddResource`, `RemoveResource`, `WriteResource` |
| `src/res/resMisc.cpp` | `ReleaseResource`, `DetachResource`, `SetResInfo` |
| `src/res/resSetcur.cpp` | `UseResFile`, `CurResFile`, `HomeResFile` |
| `src/res/resPartial.cpp` | Partial resource loading (`GetPartialResource`) |
| `src/res/resIMIV.cpp` | IM IV resource additions |

## Important Data Structures

- **`reshead`** (guest struct): `{rdatoff, rmapoff, datlen, maplen}` — the four
  offsets at the start of every resource fork.
- **`resmap`** (guest struct): `{reshead copy, nextmap handle, refNum, attrs, typoff, namoff}`.
- **`typref`** (guest struct): `{rtyp, nres, rloff}` — one entry in the type list.
- **`resref`** (guest struct): `{rid, noff, ratr, doff[3], rhand}` — one resource
  within a type.
- **`empty_resource_template_t`**: used to create a blank (empty) resource fork.

## Key Functions / Traps

| Symbol | Trap | Description |
|--------|------|-------------|
| `C_GetResource(type, id)` | `_GetResource` | Load and return a resource by type+ID |
| `C_Get1Resource(type, id)` | `_Get1Resource` | Search only the top map |
| `C_GetNamedResource(type, name)` | `_GetNamedResource` | Search by type+name |
| `C_AddResource(h, type, id, name)` | `_AddResource` | Register a handle as a resource |
| `C_WriteResource(h)` | `_WriteResource` | Flush a resource to disk |
| `C_ReleaseResource(h)` | `_ReleaseResource` | Purge the in-memory data |
| `C_OpenResFile(name)` | `_OpenResFile` | Open a file's resource fork |
| `C_CloseResFile(refNum)` | `_CloseResFile` | Close a resource file |
| `ROMlib_typidtop(type, id, map, resref)` | — | Core chain search |
| `ROMlib_mgetres(map, rr)` | — | Load resource data into a Handle |

## Design Notes / Gotchas

- The `ULTIMA_III_HACK` preprocessor flag (`resource.h`) enables a workaround for a
  specific application that violates the resource manager contract. It is on by default
  and has no user-visible effect on other applications.
- `system_file_version_skew_p` is set when Executor's bundled System file has a
  different version than the application expects. The Resource Manager logs a warning
  but continues.
- The 3-byte `doff` field in `resref` is a packed big-endian offset. Do not treat it
  as a standard `GUEST<uint32_t>` — there are dedicated accessor macros that handle
  the 24-bit read.
- Resource handles may be purgeable (`resPurgeable` attribute). Always check that
  `*h != nullptr` after calling `GetResource` if the handle has ever been made
  purgeable and may have been purged.
