# Mixed Mode & 68K/PPC Interoperability

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

Mixed Mode Manager allows 68K applications to call PPC code and vice versa, using a
`RoutineDescriptor` structure that identifies the calling convention and ISA of a
function. Executor implements this using the `MIXED_MODE_TRAP` (`0xAAFE`) to switch
between the syn68k 68K emulator and the PowerCore PPC interpreter.

## Key Concepts

**`RoutineDescriptor`**: a guest-memory struct created by `NewRoutineDescriptor`. It
begins with a `goMixedModeTrap` instruction (`MIXED_MODE_TRAP`, a 68K word), so that
calling it from 68K code triggers the mode switch. The rest of the struct records the
`ProcInfoType` (argument/return layout), `ISAType` (68K or PPC), and the actual
function pointer.

**`ProcInfoType`**: a 32-bit bitmask that encodes the number of parameters, the size
of each parameter (byte/word/long), and the size of the return value. The mode-switch
code uses this to marshal arguments between the 68K stack/registers and the PPC
register file.

**`MIXED_MODE_TRAP`**: the 68K word written into `goMixedModeTrap`. When syn68k
executes this word, `alinehandler` dispatches to the Mixed Mode trap handler, which
reads the `RoutineDescriptor` from just below the trap word, inspects the `ISA` field,
and calls either `executePPC` or remains on the 68K path.

**Argument marshalling** (`mixed_mode.cpp`): the `ModeSwitch` function reads arguments
from the 68K stack (guided by `ProcInfoType`) and writes them into PPC registers
`r3`–`r10`, then calls `executePPC(procDescriptor)`. On return, the PPC return value
in `r3` is written back to the 68K stack or register as appropriate.

**`PowerPCFrame`**: the PPC stack frame layout expected by CFM-compiled code. Fields
include `backChain`, `saveLR`, `saveRTOC` (table-of-contents pointer), and eight
parameter slots. Mixed Mode sets up this frame on the PPC stack before calling
`executePPC`.

**`sizeOnStack` / `readsized` / `writesized`**: local helpers that abstract 1-, 2-,
and 4-byte argument reads/writes from the 68K stack, guided by the `argsize` field
extracted from `ProcInfoType`.

## Source Files

| Path | Description |
|------|-------------|
| `src/mixed_mode.cpp` | `NewRoutineDescriptor`, `DisposeRoutineDescriptor`, `ModeSwitch` |
| `multiversal/MixedMode.h` | Mac ABI types: `RoutineDescriptor`, `ProcInfoType`, `ISAType` |
| `src/base/cpu.h` | `executePPC()`, `CPUMode` |
| `PowerCore/include/PowerCore.h` | PPC register file accessed during marshalling |

## Important Data Structures

- **`RoutineDescriptor`** (Mac ABI, guest memory):
  - `goMixedModeTrap` — 16-bit trap word; makes the descriptor callable from 68K.
  - `version` — always `kRoutineDescriptorVersion`.
  - `routineCount` — number of `RoutineRecord` entries (usually 1).
  - `routineRecords[0].procInfo` — `ProcInfoType` encoding argument layout.
  - `routineRecords[0].ISA` — `kM68kISA` or `kPowerPCISA`.
  - `routineRecords[0].procDescriptor` — pointer to the actual function.
- **`PowerPCFrame`** (local struct in `mixed_mode.cpp`): PPC ABI stack frame with
  `backChain`, `saveCR`, `saveLR`, `reserved1/2`, `saveRTOC`, `parameters[8]`.

## Key Functions / Traps

| Symbol | Trap | Description |
|--------|------|-------------|
| `C_NewRoutineDescriptor(proc, info, isa)` | `_NewRoutineDescriptor` | Allocate and fill a `RoutineDescriptor` |
| `C_DisposeRoutineDescriptor(rdp)` | `_DisposeRoutineDescriptor` | Free a routine descriptor |
| `C_NewFatRoutineDescriptor(m68k, ppc, info)` | `_NewFatRoutineDescriptor` | Create a descriptor with both ISA implementations |
| `C_SaveMixedModeState` / `C_RestoreMixedModeState` | — | Save/restore mode state (stub) |
| `ModeSwitch` (internal) | — | Core 68K→PPC argument marshalling and jump |

## Design Notes / Gotchas

- **`NewFatRoutineDescriptor` is unimplemented**: it calls `warning_unimplemented` and
  aborts. Fat Binary descriptors are constructed by the CFM loader in `cfm.cpp`, not
  through this API. Applications that call `NewFatRoutineDescriptor` directly will
  crash.
- **`SaveMixedModeState` / `RestoreMixedModeState` are stubs**: they return `paramErr`.
  No known application required these in practice.
- **`ROMlib_destroy_blocks` on the descriptor**: after allocating a `RoutineDescriptor`,
  `NewRoutineDescriptor` calls `ROMlib_destroy_blocks` on the `goMixedModeTrap` field
  so syn68k cannot cache a stale translation of the trap word location.
- **`CPUMode`**: the global `currentCPUMode` is set to `CPUMode::ppc` before
  `executePPC` is called and restored to `CPUMode::m68k` when PPC code returns. Code
  that checks this global must only do so from the emulator thread.
- Mixed Mode is only exercised when a PPC or Fat Binary application is run. Pure 68K
  applications never trigger a mode switch.
