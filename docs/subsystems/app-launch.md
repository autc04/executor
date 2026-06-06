# Application Launch, Segment Loader & Process Manager

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

This subsystem handles loading and starting Mac applications, managing their code
segments, and maintaining the (single) process record that the Process Manager exposes.
It also implements CFM (Code Fragment Manager) loading for PPC/Fat Binary applications.

## Key Concepts

**`launch.cpp`**: the top-level launcher. After the emulator's own initialisation
sequence (QuickDraw, Fonts, Window Manager, etc.), `launch.cpp` calls
`beginexecutingat(startpc)`, which:
1. Initialises the 68K register file to plausible Mac startup values.
2. Calls `execute68K(startpc)` — the main application entry point.
3. On return, calls `C_ExitToShell()` to clean up.

The start PC is the address of the application's `CODE 1` segment entry point, loaded
by the Segment Loader.

**Segment Loader** (`segment.cpp`): implements `_LoadSeg` and `_UnloadSeg`. Mac
68K applications are divided into code segments stored as `CODE` resources. Segment 0
contains the jump table; other segments are loaded on demand via `_LoadSeg`. When
first loaded, a segment's jump table entries are patched with the real addresses and
`_LoadSeg` calls are replaced with `_jmp` instructions.

**`argv_to_appfile`**: converts a command-line path (given to Executor) into a Mac
`AppFile` record by calling `PBGetCatInfo`. This record is presented to the application
via `CountAppFiles` / `GetAppFiles`.

**CFM loader** (`cfm.cpp`): handles Code Fragment Manager containers (PEF format) used
by PPC and Fat Binary applications. It:
1. Reads the PEF section headers.
2. Allocates guest memory and copies / decompresses each section (`load_unpacked_section`).
3. Resolves inter-library symbol references by name.
4. Installs `RoutineDescriptor` trampolines for exported symbols callable from 68K code.

**PEF decompression**: PEF sections can use a simple RLE-like packing scheme (`repeat_block`,
`repeat_zero`, `block_copy`). The loader handles all four PEF pattern opcodes.

**Process Manager** (`process.cpp`): implements the minimal subset of the Process
Manager needed for compatibility. Executor runs a single application at a time;
`GetCurrentProcess` / `GetProcessInformation` return a single `process_info_t` entry.
The `SIZE` resource of the application is consulted to set process mode flags.

## Source Files

| Path | Description |
|------|-------------|
| `src/launch.cpp` | System initialisation, `beginexecutingat`, ExitToShell |
| `src/segment.cpp` | `C_LoadSeg`, `C_UnloadSeg`, application file parsing |
| `src/process.cpp` | `process_create`, `C_GetCurrentProcess`, `C_GetProcessInformation` |
| `src/cfm.cpp` | PEF loader, CFM library resolution, section mapping |
| `multiversal/PEFBinaryFormat.h` | PEF on-disk struct definitions |

## Important Data Structures

- **`section_info_t`**: host-side record of a loaded PEF section: `start` (host ptr),
  `length`, permission flags.
- **`PowerPCFrame`** (in `mixed_mode.cpp`): PPC ABI stack frame layout used when
  transitioning between PPC and 68K code.
- **`process_info_t`**: host-side per-process record: mode, type, signature, size,
  launch ticks, `ProcessSerialNumber`.
- **`AppFile`** (Mac ABI struct): file type, version, volume reference number, and name;
  returned by `GetAppFiles`.

## Key Functions / Traps

| Symbol | Trap | Description |
|--------|------|-------------|
| `beginexecutingat(startpc)` | — | Prime 68K registers and start the application |
| `C_LoadSeg(segNum)` | `_LoadSeg` | Load a `CODE` resource segment on demand |
| `C_UnloadSeg(segNum)` | `_UnloadSeg` | Release a loaded code segment |
| `C_ExitToShell()` | `_ExitToShell` | Terminate the application |
| `process_create(da_p, type, sig)` | — | Register a new process record |
| `C_GetCurrentProcess(psn)` | — | Return the current process serial number |
| `C_NewRoutineDescriptor(proc, info, isa)` | `_NewRoutineDescriptor` | Create a CFM routine descriptor |

## Design Notes / Gotchas

- **Single-process model**: Executor runs exactly one Mac application at a time. Desk
  accessories are modelled as a separate process type but still share the same address
  space and CPU.
- **Jump table patching**: after `_LoadSeg` resolves a segment, the entry in the jump
  table is overwritten with a direct `jmp` to the segment code. This means
  `ROMlib_destroy_blocks` must be called on the jump table range after patching, or
  syn68k will execute the stale `_LoadSeg` stub.
- **Fat Binary selection**: when a Fat Binary (containing both 68K and PPC code) is
  opened, the CFM loader checks the host architecture and prefers the PPC fragment if
  PowerCore is available.
- **`try_to_get_memory`**: the CFM loader currently ignores alignment requests. If an
  application requires strictly aligned PEF sections, this may cause subtle failures.
