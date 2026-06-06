# Trap Dispatch & Calling Conventions

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

Every Mac OS API call made by an application is expressed as a 68K `A-line` trap
instruction (opcode `0xA000`–`0xAFFF`). The trap dispatch subsystem catches each such
instruction, looks up the corresponding C++ implementation, marshals arguments from
the 68K register file or stack, calls the implementation, and writes return values back.
This layer is the fundamental mechanism by which Executor replaces the Mac ROM.

## Key Concepts

**A-line handler**: When syn68k encounters an unrecognised A-line opcode it calls
`alinehandler` (`src/base/dispatcher.cpp`). This function reads the trap number from
the opcode, checks whether it is patched (the guest may have replaced the
implementation with its own), and invokes the appropriate handler.

**Trap number space**: The 12-bit field of an A-line instruction splits into:
- Bit 11 (`TOOLBIT = 0x0800`): set → Toolbox trap; clear → OS trap.
- Bits 9–10: used by some traps as selector bits (`TrapBits`).
- Bits 0–8: primary trap index.

**`TrapFunction<F, fptr, trapno, CallConv>`**: The core template class in
`src/base/traps.h`. Each instantiation wraps one C++ implementation function and
registers a syn68k callback at the corresponding trap number. During `init()` it
installs a 68K stub that, when executed, calls back into the C++ function via
`callback_install`. Calling the `TrapFunction` object from C++ code will either call
the implementation directly or route through the trap table if the application has
patched the trap.

**`DispatcherTrap<SelectorConvention>`**: Handles traps that dispatch on a sub-selector
(e.g. `_OSDispatch`). It reads the selector using the specified convention, looks it up
in an `unordered_map<uint32_t, SelectorEntry>`, and invokes the matching handler.

**`DeferredInit`**: All `TrapFunction` and `DispatcherTrap` objects are registered at
process startup by `DeferredInit::initAll()`, which walks a linked list built at static
construction time. Do not manipulate this list after startup.

**Calling conventions** (`namespace callconv` in `src/base/functions.h`):

| Convention | Usage |
|------------|-------|
| `callconv::Pascal` | Parameters right-to-left on the 68K stack; return value on stack (most Toolbox traps) |
| `callconv::Register<Ret(Args...), locs...>` | Each argument/return mapped to a specific register (most OS traps) |
| `callconv::CCall` | Standard C calling convention; used for internal stubs |
| `callconv::Raw` | Direct syn68k handler; used only when the implementation needs raw CPU state access |

**Module registration pattern** (`src/base/api-module.h`): Each `.cpp` file that
implements traps sets `#define INSTANTIATE_TRAPS_<MODULE_NAME> 1` before including its
header. `api-module.h` uses this to switch between `extern` declarations (header-only
inclusion) and full `TrapFunction` definitions and template instantiations.

## Source Files

| Path | Description |
|------|-------------|
| `src/base/dispatcher.cpp` | `alinehandler`; debug trap history |
| `src/base/dispatcher.h` | Declaration of `alinehandler` |
| `src/base/traps.h` | `TrapFunction`, `DispatcherTrap`, `Entrypoint`, selector types |
| `src/base/traps.impl.h` | Template implementations (included by translation units) |
| `src/base/functions.h` | `callconv` namespace, `UPP<>`, `ProcPtr` |
| `src/base/functions.impl.h` | UPP call-through implementations |
| `src/base/api-module.h` | Per-module trap declaration/definition switching |
| `src/base/emustubs.cpp` | Low-level 68K stubs and glue |
| `src/base/patches.cpp` | Support for `GetTrapAddress` / `SetTrapAddress` patching |

## Important Data Structures / Classes

- **`Entrypoint`**: base class for everything dispatchable; carries a name, optional
  export-to-library flag, and a breakpoint flag used by the debugger.
- **`TrapFunction<F,fptr,trapno,CallConv>`**: a static-lifetime object per trap; holds
  the guest function pointer (`guestFP`) installed at startup.
- **`DispatcherTrap<SelectorConvention>`**: owns a `std::unordered_map` from selector
  value to `SelectorEntry`.
- **`UPP<Ret(Args...), CallConv>`**: a typed wrapper around a `void*` pointer that
  represents a callable in the guest address space.

## Key Functions

| Symbol | Description |
|--------|-------------|
| `alinehandler(pc, ignored)` | Entry point for all A-line traps |
| `DeferredInit::initAll()` | Register all traps at startup |
| `TrapFunction::init()` | Install the 68K stub and register the C++ callback |
| `DispatcherTrap::addSelector()` | Add a sub-trap handler at a given selector value |

## Design Notes / Gotchas

- **Patching**: `TrapFunction::operator()` checks `isPatched()` before calling
  `fptr` directly. If an application has replaced the trap via `SetTrapAddress`, the
  call is routed through the guest trap table so the application's patch is honoured.
- **Naming convention**: the C++ implementation of a trap named `Foo` is `C_Foo`; the
  `TrapFunction` object itself is `Foo`. Raw 68K handler variants are `RAW_Foo`.
- **Breakpoints**: setting `Entrypoint::breakpoint = true` causes every invocation to
  call into the debugger before dispatching, without any extra overhead in the hot path.
