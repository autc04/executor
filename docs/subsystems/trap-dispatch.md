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
every trap object. It holds two uniform entry pointers — `Entry68KFn`
(`syn68k_addr_t (*)(syn68k_addr_t, Entrypoint*)`) and `EntryPPCFn`
(`uint32_t (*)(PowerCore&, Entrypoint*)`) — plus the trap number and, for dispatcher
sub-traps, the dispatcher and selector. The generated wrapper constructor passes
non-capturing lambdas that name the trap implementation and call the signature's
generated marshaller. Its `init()` installs the 68K callback via
`::callback_install`, fills `tooltraptable`/`ostraptable`, registers the PPC entry with
`builtinlibs::addPPCEntrypoint`, and (when applicable) calls
`dispatcher->addSelector()`. A trap object only exists once a trap number is known;
`NOTRAP_FUNCTION`s use trap number 0 and therefore never touch the trap table.

**Generated wrapper objects**: For each trap the generator emits a small
`Executor::trapentries::Wrapper_<Trap>` class deriving from `GeneratedEntrypoint`. It adds
the typed `operator()` (direct call, or the guest trap table when patched) and
`operator&` returning the typed `UPP`. The object keeps the name hand-written
code has always used — `Executor::<Trap>`, or `Executor::stub_<Trap>` /
`Executor::stub_<Impl>` for an entry object that wraps another function. The
module header declares the class and an `extern` object; the object is defined in
the generated `trap_entries/<Module>.cpp`.

**`GeneratedDispatcherTrap`** (declared in `src/base/traps.h`, defined in
`src/base/trap-entry.cpp`): handles traps that dispatch on a sub-selector (e.g.
`_OSDispatch`). The selector convention is *data* (a `SelectorKind` plus a mask) passed
to the constructor, so no template is instantiated. It reads the selector, looks it up in
the inherited `unordered_map<uint32_t, SelectorEntry>`, and invokes the stored
`Entry68KFn` with the sub-trap's `Entrypoint*`.

**`DeferredInit`**: Every `Entrypoint` registers itself on a linked list at static
construction time; `DeferredInit::initAll()` walks it during `traps::init()`. Do not
manipulate this list after startup.

**`trapentries::ReferenceAllEntries()`**: `traps::init()` calls this before
`DeferredInit::initAll()` so that the linker keeps the generated per-trap objects (the
`trap_entries/*.cpp` translation units reference nothing else, and a static library would
otherwise drop them).

**Calling conventions** (`namespace callconv` in `src/base/functions.h`):

| Convention | Usage |
|------------|-------|
| `callconv::Pascal` | Parameters right-to-left on the 68K stack; return value on stack (most Toolbox traps) |
| `callconv::Register<Ret(Args...), locs...>` | Each argument/return mapped to a specific register (most OS traps) |
| `callconv::CCall` | Standard C calling convention; used for internal stubs |
| `callconv::Raw` | Unused legacy tag; raw traps are `RAW_<Trap>(syn68k_addr_t)` functions run through `invokeFromRaw68K` |

### Marshallers

For every distinct signature in the generated set — the trap entrypoints, the
`funptr:` types and functions marked `executor_marshaling` — `multiversal` emits
four functions once per signature:

| Function | Direction / use |
|----------|-----------------|
| `invokeFrom68K(addr, f, ep, (const CC*)nullptr)` | guest 68K → native; used by the trap entries and `createCallbackUPP` |
| `invokeFromPPC(cpu, f, ep)` | guest PowerPC → native; keyed on the signature alone |
| `invokeUPP(upp, args...)` | native → guest 68K; used by `UPP<>::operator()` |
| `createCallbackUPP(f, out, ep)` | installs the 68K callback for a `*_FUNCTION_PTR` object |

The `const CC*` witness on `invokeFrom68K` is always `nullptr`; it carries the
convention in the parameter list because two conventions can share one signature
(`DisposeHandle` is `void(Handle)` as Pascal and
`Register<void(A0), ReturnMemErr<D0>>` as a trap).  The bodies are straight-line
code built from the shared primitives (`callconv::stack`, `D<n>`/`A<n>`,
`Out<>`/`InOut<>`, `TrapBit<>`, the extras and
`callfromPPC::ParameterPasser`).  The hand-written `callfrom68K`/`callto68K`/
`callfromPPC::Invoker` templates no longer exist.

## Generated vs. hand-written

The Executor generator (`multiversal/executor.rb` plus the `executor_types.rb`,
`executor_calling_conventions.rb` and `executor_traps.rb` modules it loads)
turns every `- function:` / `- dispatcher:` entry in `multiversal/defs/*.yaml`
into:

| Output | Content |
|--------|---------|
| `api/<Module>.h` | Types, `C_<Trap>` declarations, the `Wrapper_<Trap>` class + `extern` object per trap, and the marshaller declarations |
| `trap_entries/<Module>.cpp` | Wrapper methods and objects, PPC variant adapters, the per-signature marshallers and logging printers |
| `structdump/<Module>.cpp` | `describeStruct()` per type (used by `--logtraps` and the debugger) plus the type registry |

There is no per-module trap-instantiation translation unit any more: every trap,
including the raw 68K ones (`executor_raw: true` in the YAML), is a generated
wrapper.  A raw trap's body is still hand-written as
`syn68k_addr_t RAW_<Trap>(syn68k_addr_t)` (declared in the generated header, defined in
`emustubs.cpp`/`pack.cpp`/`patches.cpp` via `RAW_68K_IMPLEMENTATION`); the wrapper's
lambda calls `invokeFromRaw68K`, which runs it with the untyped logging.

Hand-written `src/` code only uses the `*_FUNCTION_PTR` macros in `src/base/traps.h` for
functions it publishes itself: `PASCAL_FUNCTION_PTR` / `REGISTER_FUNCTION_PTR` /
`CCALL_FUNCTION_PTR` (and their `EXTERN_` forms) publish device-manager and driver
callbacks that take `UPP`s. They are the only remaining users of `WrappedFunction`.
Everything else in the trap headers is generated; the generator `raise`s if it meets a
trap shape it cannot convert, rather than silently falling back to a macro.

## Source Files

| Path | Description |
|------|-------------|
| `src/base/dispatcher.cpp` | `alinehandler`; debug trap history |
| `src/base/dispatcher.h` | Declaration of `alinehandler` |
| `src/base/trap-entry.h` | `GeneratedEntrypoint` |
| `src/base/trap-entry.cpp` | `GeneratedEntrypoint::init()`, `GeneratedDispatcherTrap`, `invokeFromRaw68K` |
| `src/base/traps.h` | `Entrypoint`, `Entry68KFn`/`EntryPPCFn`, `GenericDispatcherTrap`, `GeneratedDispatcherTrap`, `WrappedFunction`, `SelectorKind`, the `*_FUNCTION_PTR` and `RAW_68K_IMPLEMENTATION` macros |
| `src/base/traps.impl.h` | `WrappedFunction::init()` and `traps::callback_install` (included by translation units) |
| `src/base/functions.h` | `callconv` namespace, descriptor types (`Out`, `TrapBit`, …), `UPP<>`, `ProcPtr` |
| `src/base/functions.impl.h` | Shared marshalling primitives (`callconv::stack`, `readCArg`, `callfromPPC::ParameterPasser`); `UPP<>::operator()` forwarder |
| `src/base/logging.{h,cpp}` | `--logtraps`: runtime gate, filter, guarded value formatting, data-driven trap call/return |
| `src/base/structdump.h` | `TypeDesc` + type registry used by `describeStruct` |
| `src/base/emustubs.cpp` | Low-level 68K stubs and glue |
| `src/base/patches.cpp` | Support for `GetTrapAddress` / `SetTrapAddress` patching |
| `multiversal/executor*.rb` | Generate all of the above |

## Important Data Structures / Classes

- **`Entrypoint`**: base class for everything dispatchable; carries a name, optional
  library name, and a breakpoint flag used by the debugger.
- **`GeneratedEntrypoint`**: one static-lifetime object per trap; holds the two
  entry pointers and the guest function pointer (`guestFP`) installed at startup.
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
| `GeneratedEntrypoint::handle68K` | Breakpoint check, then the stored entry with `this` |
| `GeneratedDispatcherTrap::addSelector()` | Add a sub-trap handler at a given selector value |
| `logging::logTrapCallData()` | Format one logged trap call (data-driven) |

## Logging

`--logtraps` prints every OS/toolbox call and return. Logging is a **runtime** feature:
the generated marshallers always contain the logging calls, gated by
`logging::enabled()`.

To keep the per-trap cost down, the marshallers do not use per-trap templates. Each
module has one generated printer per distinct argument/return type
(`logArg_<n>(const void*)`, calling `logging::logValue`), and each marshaller (and
each PPC variant) has a small `static const logging::LogArgFn` table. The
marshaller passes pointers to its typed locals plus that table to the single
non-template `logging::logTrapCallData` / `logTrapValReturnData` /
`logTrapVoidReturnData`, using `Entrypoint::name` as the logged name. Logged
output is therefore identical to the historical `logTrapCall(name, args...)`
format, and `--logtraps-filter` still applies.

Logging treats all argument and struct data as untrusted.  It never calls an
aborting host→guest address conversion, and it validates the whole object span
with `logging::validRange()` before dereferencing a pointer.  Invalid pointers,
opaque handles and truncated Pascal strings are printed as `?` / address-only;
`--logtraps` must not be able to crash or abort the emulator.

## Design Notes / Gotchas

- **Patching**: the generated wrapper's `operator()` checks `isPatched()` before calling
  the implementation directly. If an application has replaced the trap via
  `SetTrapAddress`, the call is routed through the guest trap table so the application's
  patch is honoured. `isPatched()` is always false for a trap number of 0 (i.e.
  `NOTRAP_FUNCTION`s).
- **Naming convention**: the C++ implementation of a trap named `Foo` is `C_Foo`; the
  entry object itself is `Foo` (or `stub_Foo` when it wraps another public function). Raw
  68K handler variants are `RAW_Foo`.
- **Breakpoints**: setting `Entrypoint::breakpoint = true` causes every invocation to
  call into the debugger before dispatching, without any extra overhead in the hot path.
- **Adding an API**: add it to the YAML, not to the headers; see `AGENTS.md`.
