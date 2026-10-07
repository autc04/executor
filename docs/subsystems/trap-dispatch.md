# Trap Dispatch & Calling Conventions

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

Every Mac OS API call made by an application is expressed as a 68K `A-line` trap
instruction (opcode `0xA000`–`0xAFFF`). The trap dispatch subsystem catches each such
instruction, looks up the corresponding C++ implementation, marshals arguments from
the 68K register file or stack, calls the implementation, and writes return values back.
This layer is the fundamental mechanism by which Executor replaces the Mac ROM.

The marshalling code is **generated** by `multiversal` from `multiversal/defs/*.yaml`.
Only a small, hand-written runtime layer is compiled from source; see
[Generated vs. hand-written](#generated-vs-hand-written) below.

## Key Concepts

**A-line handler**: When syn68k encounters an unrecognised A-line opcode it calls
`alinehandler` (`src/base/dispatcher.cpp`). This function reads the trap number from
the opcode, checks whether it is patched (the guest may have replaced the
implementation with its own), and invokes the appropriate handler.

**Trap number space**: The 12-bit field of an A-line instruction splits into:
- Bit 11 (`TOOLBIT = 0x0800`): set → Toolbox trap; clear → OS trap.
- Bits 9–10: used by some traps as selector bits (`TrapBits`).
- Bits 0–8: primary trap index.

**`GeneratedEntrypoint`** (`src/base/trap-entry.{h,cpp}`): the non-template base for
every trap object. It holds two plain function pointers — a straight-line 68K entry
(`syn68k_addr_t (*)(syn68k_addr_t)`) and a straight-line PowerPC entry
(`uint32_t (*)(PowerCore&)`) — plus the trap number and, for dispatcher sub-traps, the
dispatcher and selector. Its `init()` installs the 68K callback via
`::callback_install`, fills `tooltraptable`/`ostraptable`, registers the PPC entry with
`builtinlibs::addPPCEntrypoint`, and (when applicable) calls
`dispatcher->addSelector()`. A trap object only exists once a trap number is known;
`NOTRAP_FUNCTION`s use trap number 0 and therefore never touch the trap table.

**Generated wrapper objects**: For each trap the generator emits a small
`Executor::trapentries::Wrapper_<Trap>` class deriving from `GeneratedEntrypoint`. It adds
the typed `operator()` (direct call, or the guest trap table when patched) and
`operator&` returning the typed `UPP`. The public object — `Executor::<Trap>`, or
`Executor::stub_<Trap>` for the `REGISTER_TRAP2`-style traps — is the same name
hand-written code has always used. The module header declares the class and an `extern`
object; the object is defined in the generated `trap_entries/<Module>.cpp`.

**`GeneratedDispatcherTrap`** (declared in `src/base/traps.h`, defined in
`src/base/trap-entry.cpp`): handles traps that dispatch on a sub-selector (e.g.
`_OSDispatch`). The selector convention is *data* (a `SelectorKind` plus a mask) passed
to the constructor, so no template is instantiated. It reads the selector, looks it up in
the inherited `unordered_map<uint32_t, SelectorEntry>`, and invokes the matching handler.

**`DeferredInit`**: Every `Entrypoint` registers itself on a linked list at static
construction time; `DeferredInit::initAll()` walks it during `traps::init()`. Do not
manipulate this list after startup.

**`trapentries::ReferenceAllEntries()`**: `traps::init()` calls this before
`DeferredInit::initAll()` so that the linker keeps the generated per-trap objects (the
`trap_entries/*.cpp` translation units reference nothing else). The same trick keeps the
`trap_instances/*.cpp` objects (`ReferenceAllTraps`).

**Calling conventions** (`namespace callconv` in `src/base/functions.h`):

| Convention | Usage |
|------------|-------|
| `callconv::Pascal` | Parameters right-to-left on the 68K stack; return value on stack (most Toolbox traps) |
| `callconv::Register<Ret(Args...), locs...>` | Each argument/return mapped to a specific register (most OS traps) |
| `callconv::CCall` | Standard C calling convention; used for internal stubs |
| `callconv::Raw` | Direct syn68k handler; used only when the implementation needs raw CPU state access |

**Module registration pattern** (`src/base/api-module.h`): Each `.cpp` file that still
needs macro-based trap definitions sets `#define INSTANTIATE_TRAPS_<MODULE_NAME> 1`
before including its header. `api-module.h` uses this to switch between `extern`
declarations (header-only inclusion) and full definitions plus template instantiations.
The generated `trap_instances/<Module>.cpp` still does this for the handful of traps the
generator does not convert itself (see below).

## Generated vs. hand-written

`multiversal/executor.rb` turns every `- function:` / `- dispatcher:` entry in
`multiversal/defs/*.yaml` into:

| Output | Content |
|--------|---------|
| `api/<Module>.h` | Types, `C_<Trap>` declarations, and the `Wrapper_<Trap>` class + `extern` object per trap |
| `trap_entries/<Module>.cpp` | Straight-line 68K and PowerPC entrypoints, wrapper methods, per-type logging printers, and the objects |
| `structdump/<Module>.cpp` | `describeStruct()` per type (used by `--logtraps` and the debugger) plus the type registry |
| `trap_instances/<Module>.cpp` | `#define INSTANTIATE_TRAPS_<Module>` for the remaining macro-defined traps |

Hand-written `src/` code still uses the macro surface in `src/base/traps.h` for traps it
publishes itself: `PASCAL_FUNCTION_PTR` / `REGISTER_FUNCTION_PTR` / `CCALL_FUNCTION_PTR`
(device-manager and driver callbacks that take `UPP`s), `RAW_68K_FUNCTION` /
`RAW_68K_TRAP` (raw-CPU-state handlers), and the verbatim YAML blocks that spell out
`NOTRAP_FUNCTION2` (`GetGrayRgn`) or `PASCAL_SUBTRAP` (`GetFrontProcess`). Those are the
only users of `WrappedFunction`, `TrapFunction` and `SubTrapFunction`. Everything else
in the trap headers is generated; the generator `raise`s if it meets a trap shape it
cannot convert, rather than silently falling back to a macro.

## Source Files

| Path | Description |
|------|-------------|
| `src/base/dispatcher.cpp` | `alinehandler`; debug trap history |
| `src/base/dispatcher.h` | Declaration of `alinehandler` |
| `src/base/trap-entry.h` | `GeneratedEntrypoint`, `Entry68KFn`/`EntryPPCFn` |
| `src/base/trap-entry.cpp` | `GeneratedEntrypoint::init()`, `GeneratedDispatcherTrap` |
| `src/base/traps.h` | `Entrypoint`, `GenericDispatcherTrap`, `GeneratedDispatcherTrap`, `WrappedFunction`/`TrapFunction`/`SubTrapFunction`, `SelectorKind` |
| `src/base/traps.impl.h` | Template implementations (included by translation units) |
| `src/base/functions.h` | `callconv` namespace, descriptor types (`Out`, `TrapBit`, …), `UPP<>`, `ProcPtr` |
| `src/base/functions.impl.h` | `callfrom68K`/`callfromPPC`/`callto68K` invokers (still used by `*_FUNCTION_PTR`) |
| `src/base/api-module.h` | Per-module trap declaration/definition switching |
| `src/base/logging.{h,cpp}` | `--logtraps`: runtime gate, filter, value formatting, data-driven trap call/return |
| `src/base/structdump.h` | `TypeDesc` + type registry used by `describeStruct` |
| `src/base/emustubs.cpp` | Low-level 68K stubs and glue |
| `src/base/patches.cpp` | Support for `GetTrapAddress` / `SetTrapAddress` patching |
| `multiversal/executor.rb` | Generates all of the above |

## Important Data Structures / Classes

- **`Entrypoint`**: base class for everything dispatchable; carries a name, optional
  library name, and a breakpoint flag used by the debugger.
- **`GeneratedEntrypoint`**: one static-lifetime object per trap; holds the generated
  entry function pointers and the guest function pointer (`guestFP`) installed at startup.
- **`GeneratedDispatcherTrap`**: owns the `std::unordered_map` from selector value to
  `SelectorEntry`; sub-traps register themselves with `addSelector()`.
- **`UPP<Ret(Args...), CallConv>`**: a typed wrapper around a `void*` pointer that
  represents a callable in the guest address space.

## Key Functions

| Symbol | Description |
|--------|-------------|
| `alinehandler(pc, ignored)` | Entry point for all A-line traps |
| `DeferredInit::initAll()` | Register all traps at startup |
| `GeneratedEntrypoint::init()` | Install the 68K callback, PPC entry and trap-table slot |
| `GeneratedEntrypoint::handle68K` | Breakpoint check, then the generated entry |
| `GeneratedDispatcherTrap::addSelector()` | Add a sub-trap handler at a given selector value |
| `logging::logTrapCallData()` | Format one logged trap call (data-driven) |

## Logging

`--logtraps` prints every OS/toolbox call and return. Logging is a **runtime** feature:
the generated entrypoints always contain the logging calls, gated by
`logging::enabled()`.

To keep the per-trap cost down, the entrypoints do not use per-trap templates. Each
module has one generated printer per distinct argument/return type
(`logArg_<n>(const void*)`, calling `logging::logValue`), and each trap has a small
`static const logging::LogArgFn` table. The entrypoint passes pointers to its typed
locals plus that table to the single non-template
`logging::logTrapCallData` / `logTrapValReturnData` / `logTrapVoidReturnData`. Logged
output is therefore identical to the old `logTrapCall(name, args...)` for the same
values, and `--logtraps-filter` still applies.

## Design Notes / Gotchas

- **Patching**: the generated wrapper's `operator()` checks `isPatched()` before calling
  the implementation directly. If an application has replaced the trap via
  `SetTrapAddress`, the call is routed through the guest trap table so the application's
  patch is honoured. `isPatched()` is always false for a trap number of 0 (i.e.
  `NOTRAP_FUNCTION`s).
- **Naming convention**: the C++ implementation of a trap named `Foo` is `C_Foo`; the
  entry object itself is `Foo` (or `stub_Foo` when it wraps another public function, as
  `REGISTER_TRAP2` and `NOTRAP_FUNCTION2` do). Raw 68K handler variants are `RAW_Foo`.
- **Breakpoints**: setting `Entrypoint::breakpoint = true` causes every invocation to
  call into the debugger before dispatching, without any extra overhead in the hot path.
- **Adding an API**: add it to the YAML, not to the headers; see `AGENTS.md`.
