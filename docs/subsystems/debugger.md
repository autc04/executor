# Debugger & MPW Tool Support

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

Executor ships an interactive machine-level debugger built on top of `cxmon` (a
debugger originally from the Basilisk II project) and exposes it through the
`base::Debugger` interface. Separately, the MPW (Macintosh Programmer's Workshop) tool
support layer implements the subset of MPW-specific traps that command-line Mac tools
use for I/O, so that MPW tools can run under Executor.

---

## Debugger (`cxmon` + `src/debug/`, `src/base/debugger.h`)

### Key Concepts

**`base::Debugger`**: a singleton abstract interface defined in `src/base/debugger.h`.
The cxmon back-end (`src/debug/mon_debugger.cpp`) subclasses it. The instance pointer
`base::Debugger::instance` is checked in hot paths (trap dispatch, PPC execute loop)
so the overhead is a single null-pointer test when no debugger is attached.

**`InitMonDebugger()`**: called at startup if the `-debugger` command-line flag is
present. It instantiates the cxmon-based debugger and assigns it to
`base::Debugger::instance`.

**Breakpoints**: `Entrypoint::breakpoint` (in `src/base/traps.h`) is a per-trap flag.
When set, every invocation of that trap calls `checkBreak68K` or `checkBreakPPC`, which
drop into the debugger. Breakpoints at arbitrary code addresses are managed through
`PowerCore::getNextBreakpoint` (PPC) and syn68k's callback mechanism (68K).

**`initProcess`**: called by `beginexecutingat` when the debugger is active. It gives
the debugger the application start address, allowing it to set initial breakpoints
before the first instruction executes.

**`dump_recent_traps(n)`** (`src/base/dispatcher.cpp`): a debug helper that prints the
last `n` traps executed (by trap number and name), sorted by recency. Active only in
non-NDEBUG builds. The rolling trap history is stored in `debugtable[]`.

**Trap watchpoints** (`trap_watch`): record a snapshot of a memory region before each
trap and compare it after, reporting any change. Only available in debug builds.

**`cxmon`**: the GPL v2+ debugger library in the `cxmon/` submodule. Provides a command
shell with disassembly, memory dump, breakpoint management, and expression evaluation
for 68K addresses. Linking cxmon makes the resulting binary GPL-licensed.

### Source Files

| Path | Description |
|------|-------------|
| `src/base/debugger.h` | `base::Debugger` abstract interface |
| `src/base/debugger.cpp` | Base implementation helpers |
| `src/debug/mon_debugger.h` | `InitMonDebugger()` declaration |
| `src/debug/mon_debugger.cpp` | cxmon integration: `Debugger` subclass |
| `src/base/dispatcher.cpp` | `dump_recent_traps`, trap history table |
| `cxmon/src/` | cxmon source (disassembler, monitor shell) |

---

## MPW Tool Support (`src/mpw/`)

### Key Concepts

Classic Mac OS MPW tools are command-line programs that use a set of non-standard traps
for file I/O, environment variable access, and inter-process communication. Executor
implements these so that MPW-compiled tools can run without modification.

**`fcntl` / `ioctl` emulation**: MPW tools call low-level file operations using
selector-dispatched traps. The selectors (defined in `mpw.h`) include:
- `kF_OPEN` / `kF_DELETE` / `kF_RENAME`: basic file operations.
- `kF_GTABINFO` / `kF_STABINFO` etc.: editor metadata (tab width, font, print record).
- `kFIOLSEEK`, `kFIOFNAME`, `kFIOSETEOF`: low-level I/O control.

**Open flags**: `kO_RDONLY`, `kO_WRONLY`, `kO_RDWR`, `kO_CREAT`, `kO_TRUNC`,
`kO_RSRC` (open the resource fork), `kO_APPEND`, `kO_BINARY`.

**Seek origins**: `kSEEK_SET` (0), `kSEEK_CUR` (1), `kSEEK_END` (2).

**`mpw_errno`**: MPW defines its own errno values (`kEPERM`, `kENOENT`, `kEIO`, etc.)
that are distinct from the host system errno values. MPW trap implementations map Mac
OS errors to `mpw_errno` codes.

**Module name**: `#define MODULE_NAME mpw` — MPW trap objects are registered through
the standard `api-module.h` mechanism.

### Source Files

| Path | Description |
|------|-------------|
| `src/mpw/mpw.h` | MPW constants: `fcntl` selectors, open flags, seek modes, `mpw_errno` |
| `src/mpw/mpw.cpp` | MPW trap implementations |

---

## Design Notes / Gotchas

- **GPL licensing**: cxmon is GPL v2+. Builds that include cxmon (`-DEXECUTOR_INCLUDE_CXMON=ON`)
  produce a GPL-licensed binary. Release builds intended for distribution under a
  different licence must exclude cxmon at configure time.
- **Debugger overhead in release**: `base::Debugger::instance` is checked on every
  trap dispatch. Keep it a simple pointer comparison; do not add logic to the null-check
  path.
- **`dump_recent_traps` is non-reentrant**: it accesses `debugtable[]` and `debugnumber`
  without locking. Call it only from the emulator thread in single-threaded debug
  contexts.
- MPW tool support covers the subset of MPW traps needed by typical command-line tools.
  Advanced MPW features (Projector, Make, source control) are not implemented and will
  return `unimplementedTrap` or `paramErr`.
