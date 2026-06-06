# Memory Manager

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

The Memory Manager implements the classic Mac OS heap allocator, providing `NewHandle`,
`NewPtr`, `DisposeHandle`, `DisposePtr`, `HLock`, `HUnlock`, `SetHandleSize`, and the
full complement of zone-based allocation traps. It manages a flat region of the guest
address space divided into a system zone (`SysZone`) and an application zone
(`ApplZone`), with an optional temporary memory zone. All structures reside in the
guest address space and are accessed through `GUEST<T>` wrappers.

## Key Concepts

**Zones**: A Mac heap zone (`THz`, pointer to a `Zone` struct) is a contiguous block of
guest memory managed as a free list. The Memory Manager maintains two primary zones:
`LM(SysZone)` for OS-level allocations and `LM(ApplZone)` for the application.
`LM(TheZone)` names the zone to allocate from; `TheZoneGuard` (an RAII helper in
`mman.h`) saves and restores `TheZone` around a block that needs to allocate in a
specific zone.

**Handles vs. Pointers**: A `Handle` is a `Ptr*` — a pointer to a master pointer that
in turn points to the actual block. The level of indirection allows the Memory Manager
to relocate blocks during compaction without invalidating handles held by application
code. A `Ptr` is a direct pointer to a non-relocatable block.

**`block_header_t`**: Every allocation is preceded by an 8-byte (minimum) header in the
guest address space. Fields include `flags` (USE bits: free/nonrelocatable/relocatable),
`master_ptr_flags`, `size_correction` (alignment adjustment), and `size`. The USE
field encodes whether the block is free (`FREE`), a non-relocatable pointer block, or a
relocatable handle block.

**Size parameters**:
- `ROMlib_applzone_size` (default ~8 MB): total application zone size.
- `ROMlib_syszone_size`: system zone size.
- `ROMlib_stack_size`: 68K stack size allocated within the guest address space.
These can be overridden via command-line flags before startup.

**Memory compaction**: When a zone has insufficient contiguous free space, the Memory
Manager walks all relocatable blocks and moves them to consolidate free space, updating
master pointers. Non-relocatable blocks and locked handles (`LOCKBIT`) cannot be moved.

## Source Files

| Path | Description |
|------|-------------|
| `src/mman/mman.cpp` | Core allocator: `NewHandle`, `NewPtr`, `DisposeHandle`, etc. |
| `src/mman/mmansubr.cpp` | Zone walking, compaction, block splitting helpers |
| `src/mman/mman.h` | Public API, `TheZoneGuard`, `HASSIGN_*` macros |
| `src/mman/mman_private.h` | `block_header_t`, USE/LOCK/PURGE bit accessors |
| `src/mman/memsize.h` | Default zone size constants |
| `src/mman/tempmem.cpp` | Temporary memory (`TempNewHandle`, `TempNewPtr`) |
| `src/mman/tempalloc.h` | Temporary allocation declarations |

## Important Data Structures

- **`block_header_t`** (guest struct): per-block header. `USE(block)` returns 0
  (free), 1 (non-relocatable), or 2 (relocatable). `PSIZE(block)` is the physical size
  including the header.
- **`THz`** (`Zone*` in Mac headers): zone header at the base of each heap zone;
  contains free list, rover pointer, and zone boundaries.
- **`TheZoneGuard`**: stack-allocated RAII guard that sets `LM(TheZone)` to a given
  zone on construction and restores the previous value on destruction.

## Key Functions / Traps

| Symbol | Trap | Description |
|--------|------|-------------|
| `C_NewHandle(size)` | `_NewHandle` | Allocate a relocatable block |
| `C_DisposeHandle(h)` | `_DisposeHandle` | Free a handle |
| `C_NewPtr(size)` | `_NewPtr` | Allocate a non-relocatable block |
| `C_DisposePtr(p)` | `_DisposePtr` | Free a pointer block |
| `C_HLock(h)` | `_HLock` | Lock a handle against relocation |
| `C_HUnlock(h)` | `_HUnlock` | Unlock a handle |
| `C_SetHandleSize(h, size)` | `_SetHandleSize` | Resize a handle |
| `C_CompactMem(need)` | `_CompactMem` | Compact the current zone |
| `hlock_return_orig_state(h)` | — | Lock a handle; return the original lock state |

## Design Notes / Gotchas

- `HANDLE_TO_BLOCK(h)` dereferences `h` to get the master pointer, then subtracts
  `HEADER_SIZE` to reach the `block_header_t`. **Never** compute this offset manually.
- `VALID_ADDRESS(p)` checks that a pointer falls within `[ROMlib_syszone, ROMlib_memtop)`.
  This is used as a sanity check before operating on an address; it does not guarantee
  the block is still allocated.
- Some applications patch `NewHandle` via `SetTrapAddress`. The `SYS_P` and `CLEAR_P`
  macros in `mman.h` inspect the calling trap number to determine whether the
  allocation should come from the system or application zone, working around a known
  application compatibility issue.
- The `_NewHandle_copy_ptr_flags` / `_NewPtr_copy_ptr_flags` family are convenience
  wrappers that allocate and populate a block in one call.
