# UPP<> and calling-convention refactor (plan)

> Date: 2026-10-08
> Status: U1 is done.  U2 is partly landed: the generator already emits an
> `invokeUPP`/`createCallbackUPP` pair for some signatures and the two
> consumers use it.  The remaining work is described under U2–U5.

The goal: make the *generator* the single source of truth for calling
conventions by generating **one marshaller set per signature**, and using that
set from all three places that marshal today: the trap entrypoints,
`UPP<>::operator()`, and the `*_FUNCTION_PTR` callbacks.  The hand-written
invocation machinery in `src/base/functions.impl.h` then goes, without giving
up `UPP<>`'s role as the host-side representation of a guest procedure pointer.

## Decision

`UPP<F, CC>` **remains a template**.  For every signature in the generated set
(below) the generator emits four functions:

```cpp
// guest -> host, 68K: decode the arguments, log, call f, log, encode the
// result and return the guest return address.  Used by the trap entrypoints
// and, through createCallbackUPP, by *_FUNCTION_PTR callbacks.
syn68k_addr_t invokeFrom68K(syn68k_addr_t addr, Ret (*f)(Args...),
                            traps::Entrypoint* ep);

// guest -> host, PowerPC.  Used by the trap entrypoints.
uint32_t invokeFromPPC(PowerCore& cpu, Ret (*f)(Args...),
                       traps::Entrypoint* ep);

// host -> guest, 68K.  Used by UPP<>::operator().
Ret invokeUPP(const UPP<Ret(Args...), CC>& f, Args... args);

// *_FUNCTION_PTR adapter: installs a 68K callback that checks breakpoints and
// then calls invokeFrom68K.  It contains no marshalling logic of its own.
void createCallbackUPP(Ret (*f)(Args...), UPP<Ret(Args...), CC>& out,
                       traps::Entrypoint* ep);
```

All four are keyed on the **emitted C++ type**, never on a name:

- `invokeFrom68K`, `invokeUPP` and `createCallbackUPP` are keyed on
  `(Sig, CC)`;
- `invokeFromPPC` is keyed on `Sig` alone, because the PowerPC ABI reads the
  same argument list regardless of the 68K register/stack descriptors in `CC`.

Names are only aliases for `UPP<...>`; two names with the same signature are
the same type and must map to the same overload.  Type identity, not spelling,
is what deduplicates the generated definitions.

`GeneratedEntrypoint` is not a template and must store one 68K and one PPC
entry for every trap, so it stores the **entry** shape rather than the
signature-typed marshaller:

```cpp
// Uniform types stored by traps::GeneratedEntrypoint.
using Entry68KFn = syn68k_addr_t (*)(syn68k_addr_t, traps::Entrypoint*);
using EntryPPCFn = uint32_t (*)(PowerCore&, traps::Entrypoint*);
```

Each generated wrapper class already exists per trap, so its constructor passes
a non-capturing lambda that names its own implementation and calls the typed
marshaller:

```cpp
// Generated once per trap.
Wrapper_Open::Wrapper_Open()
    : traps::GeneratedEntrypoint("Open", "InterfaceLib", 0xA000,
        [](syn68k_addr_t addr, traps::Entrypoint* ep) -> syn68k_addr_t
        {
            return invokeFrom68K(addr, &C_Open, ep);
        },
        [](PowerCore& cpu, traps::Entrypoint* ep) -> uint32_t
        {
            return invokeFromPPC(cpu, &C_Open, ep);
        })
{}
```

The lambdas are non-capturing; `&C_Open` is a namespace-scope function, not a
capture.  The implementation pointer therefore never leaves its typed form: the
compiler checks it against the marshaller's parameter, and there is no
`void*`/`reinterpret_cast` bridge and no cross-translation-unit declaration for
a per-signature entry thunk.  The marshaller itself is still emitted once per
signature; only the one-line call site is per trap.

Raw 68K handlers use the same shape, through one hand-written function:

```cpp
syn68k_addr_t invokeFromRaw68K(syn68k_addr_t addr,
                               syn68k_addr_t (*f)(syn68k_addr_t),
                               traps::Entrypoint* ep);
```

A raw wrapper's lambda passes `&RAW_<name>` directly and `invokeFromRaw68K`
reproduces the untyped register/stack dump.

Then:

- `GeneratedEntrypoint` stores the 68K/PPC entry pointers; `handle68K`/`handlePPC`
  do the breakpoint check and call them.  For a typed trap each entry is a
  non-capturing lambda that calls the signature's marshaller with its own
  implementation.  The generated per-trap `entry_*` functions disappear for
  everything except the PPC-only variant adapters (which forward to another
  wrapper and never cross the guest boundary).
- `UPP<>::operator()` forwards to `invokeUPP`.
- The `*_FUNCTION_PTR` macro family **stays**, providing the immediate wrapper
  class (today `WrappedFunction<>`); its `init()` calls `createCallbackUPP`.
- The `callconv` **meta-language declarations stay** in `functions.h`/`impl.h`
  (the vocabulary the YAML and the generated marshallers speak); the invokers go.

### The signature set

Marshallers are emitted for every distinct signature in the union of:

1. every entrypoint the generator emits (`:pascal`, `:register`, `:sub`,
   `:notrap`) — every signature that can be called from guest code;
2. every `funptr:` type — the callback surface;
3. every function declaration marked `executor_marshaling: true` — the escape
   hatch for a declaration that produces no wrapper but still needs a
   marshaller.  `ExecutorUnixMount` is the only user.

`Raw` entries have no typed signature and are not in the set.  PPC-only
`variants` forward to another generated wrapper; they are not in the set
either.

All four functions are emitted for every key in the set.  The set is defined by
what the generator knows about, not by which consumer happens to need a
particular shape; a missing overload is then a hard compile error naming the
UPP type at the call site, which is the property that keeps the macros and
hand-written declarations honest.

## Why this shape

- **Signature identity is automatic.**  Two names for the same signature are
  the same `UPP<...>` template-id, so they are the same type with no aliasing
  or conversion machinery.  This is load-bearing: `QDRectUPP` and `QDOvalUPP`
  are both `UPP<void(GrafVerb, const Rect *)>`, and `qHooks.cpp` compares a
  stored `QDRectUPP` against `&StdRect` and a `QDOvalUPP` against `&StdOval`.
  A class-per-name design would need explicit aliasing or matcher machinery to
  preserve that.
- **Every type is spellable.**  A generated wrapper's `operator&` can return
  `UPP<Sig, cc>` for any trap, so no type has to be minted for signature-only
  types and none of the 64 `&Trap` sites change.
- **The convention is part of the overload's key.**  The calling convention is
  not in the parameter list (`DisposeHandle` is `void(Handle)` as Pascal and
  `Register<void(A0), ReturnMemErr<D0>>` as a trap), but it is part of
  `UPP<..., CC>`, so the two overloads do not collide.  Overloading on the
  parameter list alone would conflate them.
- **Overloads are preferable to template specialization here.**  A missing
  overload is a `no matching function` naming the UPP type; a missing
  specialization is an `incomplete type` error, or ill-formed with no
  diagnostic if a primary template exists.  The two directions (host→guest and
  guest→native) also stay independent overload sets instead of one
  specialization carrying both.  `createCallbackUPP` and `invokeUPP` take the
  `UPP<...>`/typed function pointer directly, so no witness argument is needed
  to carry `CC`.
- **One marshaller per signature, not per trap.**  The marshalling body is
  generated once per signature.  The per-trap data that remains at runtime is
  the `Entrypoint*`, which carries the name and the breakpoint check; the
  implementation is named in the per-trap one-line lambda.  Logging therefore
  does not multiply the generated code; it is the same data-driven
  `logging::logTrapCallData` / per-type printer scheme the generated
  entrypoints already use.
- **No type erasure between the wrapper and the marshaller.**  The generated
  wrapper's call site names its implementation directly, so the function
  pointer is checked by the compiler and never cast.  Sharing one entry per
  signature would require `void*` + `reinterpret_cast` plus a declaration in
  every mentioning module; the per-trap lambda costs one tiny function per trap
  and removes both.
- **`createCallbackUPP` is a thin adapter.**  It only installs a callback,
  performs the breakpoint check and calls `invokeFrom68K`; it is usable for any
  signature in the set and is not coupled to the `*_FUNCTION_PTR` family.

## Background

### What `UPP<>` is

`UPP<F, CC>` is doing three jobs at once:

1. **Guest value** — the host-side representation of a Mac universal procedure
   pointer stored in guest memory.  `GUEST<QDTextUPP>`-style fields rely on
   `guestvalues::GuestTypeTraits<UPP<T, CC>>` and
   `GuestWrapperOps<UPP<Ret(Args...), CC>>` (`src/base/mactype.h`).
2. **Typed callable** — `operator()` marshals a host call into the guest.
3. **Calling-convention tag** — the `CallConv` parameter makes the type carry a
   `callconv::Register<...>` spec.

All three stay.  Only (2)'s implementation moves.

### Inventory of the machinery

All in `src/base/functions.*` unless noted:

| Piece | Location | Fate |
|---|---|---|
| `UPP<>` | `functions.h` | **stays** (thin; `operator()` forwards to `invokeUPP`) |
| convention tags (`Pascal`, `CCall`, `Raw`, `Register<T, Extras...>`) | `functions.h` | **stay** (meta-language) |
| descriptors `D<n>`, `A<n>`, `D0HighWord`, `D0LowWord`, `Out<>`, `InOut<>`, `TrapBit<>` | `functions.impl.h` | **stay** (the generated marshallers use them) |
| extras `ReturnMemErr<>`, `CCFromD0`, `ClearD0`, `SaveA1D1D2`, `MoveA1ToA0` | `functions.impl.h` | **stay** |
| `callconv::stack::push/pop` | `functions.impl.h` | **stay** (the generated marshallers use them) |
| `readCArg` | `functions.impl.h` | **stays**, moves out of `callfrom68K` into `callconv` (shared primitive for CCall) |
| `callfrom68K` (`Invoker`, `PascalInvoker`, `RegInvoker`, `readCArgsAndCall`) | `functions.impl.h` | **goes** |
| `callto68K` (`Invoker`, `InvokerRec`) | `functions.impl.h` | **goes** |
| `callfromPPC::Invoker` | `functions.impl.h` | **goes** (only reachable through the `libname` branch of `WrappedFunction::init()`, which no `*_FUNCTION_PTR` macro can reach) |
| `callfromPPC::ParameterPasser` | `functions.impl.h` | **stays** (generated PPC marshallers) |
| `UPP<>::operator()` | `functions.impl.h` | stays, becomes a forwarder |
| `WrappedFunction<>` + `operator&` | `traps.h` | stays (the `*_FUNCTION_PTR` wrapper class) |
| `WrappedFunction<>::init()` | `traps.impl.h` | stays, calls `createCallbackUPP` and `invokeFromPPC` |
| `LoggedFunction`, `makeLoggedFunction`, `makeLoggedFunction1` | `logging.h` | **go** (the marshallers log data-driven) |
| `logTrapCall`, `logTrapValReturn`, `logTrapVoidReturn`, `logList` | `logging.h` | **go** with `LoggedFunction` (nothing else uses them) |
| `PASCAL/REGISTER/CCALL_FUNCTION_PTR` (+ `EXTERN_`/`DEFINE_`) | `traps.h` | stays |

### Who uses what (verified)

- `callto68K` is used **only** by `UPP<>::operator()`.
- `callfrom68K` is used **only** by `WrappedFunction<>::init()` (through
  `createCallbackUPP`).
- `callfromPPC::Invoker` is used **only** by `WrappedFunction<>::init()`'s
  `libname` branch.  Every `*_FUNCTION_PTR` macro passes only a name to the
  `Entrypoint` constructor, so `libname` is null and the branch cannot run.
- The descriptors are used by both directions of the old invokers and by the
  **generated entrypoints** (`build/src/trap_entries/*.cpp`:
  `callconv::D0 c0; x = c0;`, `callconv::ReturnMemErr<D0>` in `extras`), so
  they are the shared primitive layer the generated marshallers also use.
- `LoggedFunction` is used only by the callback paths.  The generated
  entrypoints already log with `logTrapCallData`.

So "marshal and call" is implemented **three times** today: the generated
entrypoints, `callfrom68K`+`callfromPPC` (for `*_FUNCTION_PTR`) and `callto68K`
(for `UPP<>` calls).  This plan leaves one (the generator) plus the shared
primitives.

### The surface

- `*_FUNCTION_PTR`: **73 definitions** across 19 files.
- `funptr:` YAML entries: **50** (`executor.rb:956-973`).
- Generated wrappers: **2039** objects (33 raw; ~600 dispatcher subtraps);
  **849 distinct `(Sig, CC)`** pairs but only 838 distinct `Sig`s.
  `operator&` returns `UPP<Sig, cc>` (`executor.rb:636,874`) and the patched path
  is `(UPP<Sig, cc>(SYN68K_TO_US(tableEntry())))(args)` (`executor.rb:885,891`).
- The **64 hand-written address-of uses** of trap objects (all in `.cpp`; 49 of
  them need the *typed* UPP, the rest a plain `ProcPtr`).  These do not change.

## U1 — the UPP types to name / declare

Eleven hand-written `UPP<...>` aliases remain in `src/`.  Each has to be
replaced by a declared type, or by an existing declared type with the same
signature, so that every signature used outside generated code is declared in
the YAML.  They fall into these categories:

| alias | what it is | action |
|---|---|---|
| `pritemprocp` | **is** `UserItemUPP` (`void(DialogPtr, INTEGER)`) | use the declared name |
| `useritemp` | **is** `UserItemUPP` (`WindowPtr`≡`DialogPtr` in Executor) | use the declared name |
| `modalprocp` | **is** `ModalFilterUPP` | use the declared name |
| `prinitprocp` | `TPPrDlg(THPrint)` — no declared type, but the signature **is** that of traps `PrStlInit`/`PrJobInit` | marshaller exists via the traps; keep the local alias or give it an Executor-internal name |
| `cmpf` | `INTEGER(Ptr,Ptr,INTEGER,INTEGER)` — signature **is** that of traps `IUMagString`/`IUMagIDString` | same |
| `menuprocp` | menu definition procedure (MDEF) | declare **`MenuDefUPP`** in `MenuMgr.yaml` |
| `mbdfprocp` | menu bar definition function (MBDF) | declare **`MenuBarDefUPP`** in `MenuMgr.yaml` |
| `listprocp` | list definition procedure (LDEF) | declare **`ListDefUPP`** in `ListMgr.yaml` |
| `ctlfuncp` | control definition function (CDEF) | declare **`ControlDefUPP`** in `ControlMgr.yaml` |
| `snthfp` | `Boolean(SndChannelPtr, SndCommand*, ModifierStubPtr)` (sound `snth`) | declare as **`SndModifierUPP`** in `SoundMgr.yaml`, together with an opaque `ModifierStub` struct (both `only-for: Executor`) |
| `dcmpProcPtr` | `void(Ptr, Ptr, Ptr, Size)` (`dcmp` resource) | declare in `ResourceMgr.yaml` as `ResourceDecompressionUPP` |

The four def-proc names are canonical Apple API types (from the Universal
Interfaces: `MenuDefUPP`/`MenuDefProcPtr` in `Menus.h`, `MenuBarDefUPP` in
`Menus.h`, `ListDefUPP` in `Lists.h`, `ControlDefUPP` in `Controls.h`; there is
no `MDEFUPP`/`MBDFUPP`/`LDEFUPP`/`CDEFUPP` spelling).  Because they are real API
types they belong in the normal API YAMLs and **will also appear in the
CIncludes output**; that change is intended.  `snthfp`/`dcmpProcPtr` have no
canonical Apple name and are Executor-internal (`only-for: Executor`); they
become `SndModifierUPP` and `ResourceDecompressionUPP`.

`prinitprocp`/`cmpf` are worth noting separately: their marshallers are covered
by trap signatures, so their local aliases can remain (they are already named
typedefs); naming them is optional tidiness.

### Coverage of the 73 `*_FUNCTION_PTR` objects

Every `PASCAL_`/`REGISTER_`/`CCALL_FUNCTION_PTR` expands to an explicit
`template class WrappedFunction<decltype(C_NAME), &C_NAME[, CC]>`
instantiation, so its `init()` body must compile.  That body calls
`createCallbackUPP`, so the `(Sig, CC)` pair of **every** one of these 73
objects must have a declared UPP to overload on.  That is also the coverage
check: the existing explicit instantiation turns a missing declaration into a
build-time `no matching function` naming the offending object, so no separate
test is needed.

Checked mechanically against the generated `build/src/api/*.h`, plus by hand
for every typedef-spelling case (`Ptr` vs `char*`, `int` vs `int32_t`, `short`
vs `int16_t`, `DialogPeek` vs `DialogPtr`, `DialogRecord*` vs `CInfoPBRec*`).
The numeric-typedef equivalences were confirmed with `__is_same` on this host
(`int`≡`int32_t`, `short`≡`int16_t`).

**59 of the 73 are already covered** by an existing declared UPP:

| declared UPP | `*_FUNCTION_PTR`s it covers |
|---|---|
| `VoidUPP` | `adb_service_stub`, `handle_refresh`, `sound_timer_handler`, `ROMlib_vcatch`, `ROMlib_wakeup`, `ROMlib_WNETimeout` |
| `UserItemUPP` | `ROMlib_circle_ok`, `ROMlib_orientation`, `ROMlib_myjobproc`, `ROMlib_mystlproc` |
| `ModalFilterUPP` | `ROMlib_myfilt`, `ROMlib_stdffilt`, `ROMlib_stlfilterproc`, `ROMlib_numsonlyfilterproc` |
| `ControlActionUPP` | `ROMlib_mytrack`, `ROMlib_stdftrack` |
| `SoundUPP` | `ROMlib_mysound` |
| `SelectorFunctionUPP` | `GestaltTablesOnly` |
| `DriverUPP` | `ROMlib_serial{open,prime,ctl,status,close}`, `ROMlib_ipp{open,prime,ctl,status,close}` |
| `DeviceLoopDrawingUPP` | `new_draw_scroll`, `new_pos_ctl` |
| `WindowDefUPP` | `wdef0`, `wdef16` |
| `QDTextUPP` | `PrText`, `donotPrText`, `textasPS` |
| `QDLineUPP`, `QDRectUPP`, `QDRRectUPP`, `QDOvalUPP`, `QDArcUPP`, `QDPolyUPP`, `QDRgnUPP`, `QDBitsUPP`, `QDCommentUPP`, `QDTexMeasUPP` | the `PrX`/`donotPrX` pairs for the matching `QDProcs` slot, plus `PrComment`, `PrTxMeas` (the get/put-pic slots are the exception — see below) |
| `MPWQuitProcPtr`, `MPWFileProcPtr`, `MPWAccessProcPtr`, `MPWIOCtlProcPtr` | `mpwQuit`; `mpwClose`/`mpwRead`/`mpwWrite`; `mpwAccess`; `mpwIOCtl` (the source spells the results/arguments `int`; identical to `int32_t` on the supported ABIs) |

**7 need the U1 declarations above:** `cdef0`, `cdef16`, `cdef1008` (→
`ControlDefUPP`), `ldef0` (→ `ListDefUPP`), `mdef0` (→ `MenuDefUPP`), `mbdf0`
(→ `MenuBarDefUPP`), `snth5` (→ `SndModifierUPP`).  `cdef1008` spells
`int16_t`/`int32_t` where `cdef0` spells `INTEGER`/`LONGINT` — identical types,
so one `ControlDefUPP` covers both.

The canonical Apple signatures (from `InterfacesAndLibraries`; the two `Ref`
aliases already exist in Executor's own generated headers — `MenuRef` in
`MenuMgr.h`, `ControlRef` in `ControlMgr.h`):

- `MenuDefUPP`: `void(short message, MenuRef theMenu, Rect *menuRect, Point hitPt, short *whichItem)`
- `MenuBarDefUPP`: `long(short selector, short message, short parameter1, long parameter2)`
- `ListDefUPP`: `void(short lMessage, Boolean lSelect, Rect *lRect, Cell lCell, short lDataOffset, short lDataLen, ListHandle lHandle)`
- `ControlDefUPP`: `SInt32(SInt16 varCode, ControlHandle theControl, ControlDefProcMessage message, SInt32 param)`

These match the local `C_mdef0`/`C_mbdf0`/`C_ldef0`/`C_cdef0` argument lists
exactly, modulo the numeric typedefs.

**The last 7 objects are resolved by signature updates or new declarations:**

| object | resolution |
|---|---|
| `unixmount` | declare the API **function** `ExecutorUnixMount` in the new `defs/Executor.yaml`, with the same `m68k-inline` sequence as `packages/go/go.h` (`[0x2078, 0x0058, 0x2068, 0x0020, 0x4E90]`) and `executor_marshaling: true`.  This is a genuine Executor extension API: a normal API declaration that is also generated for CIncludes, not a trap and not a CFM export.  `PASCAL_FUNCTION_PTR(unixmount)` stays — the declaration and the macro object are separate entities, wired together only by the inline opcodes and the hand-written jump-table initialization. |
| `ROMlib_filebox` | **is** a `UserItemUPP`: `DialogPeek`/`WindowPeek`/`DialogPtr`/`WindowPtr`/`GrafPtr` were all interconvertible in the classic API; the type distinction is modern-C++ spelling of one contract.  Take `DialogPtr` and move the `DialogPeek` cast inside the function. |
| `ROMlib_dotext` | new Executor-only funptr in `TextEdit.yaml`, plus the convention fix below. |
| `PrGetPic`, `PrPutPic`, `donotPrGetPic`, `donotPrPutPic` | change `void(Ptr, INTEGER)` to the public API's `void(void*, INTEGER)` / `void(const void*, INTEGER)` (`QDGetPicUPP` / `QDPutPicUPP`).  The old API used `Ptr` for the same semantics; `Ptr` = `char*` is what differed, not the intent. |

Two details:

- **`unixmount`'s home.**  `defs/*.yaml` is globbed by `make-multiverse.rb`, so a
  new `defs/Executor.yaml` is picked up automatically — no module registration
  and no build-system change.  Declare it as a plain `- function:`: no `trap:`,
  no `executor:`, no `only-for:` (it belongs in the CIncludes output too, like
  any other API declaration).  The Executor generator then emits only a
  prototype; the guest entry remains the manually installed
  `PASCAL_FUNCTION_PTR(unixmount)` object.  Mark it `executor_marshaling: true`
  so its `(Sig, CC)` is added to the U2 set even though no wrapper is
  generated.  That is what makes `OSErr(CInfoPBRec *)` available to the
  existing macro object.
- **`dotext`.**  The `FIXME`, the commented-out implementation below it and
  `ROMlib_call_TEDoText` (`teMisc.cpp:599-630`) all agree the real ABI is *TE
  record in A3, the three `INTEGER`s in D3/D4/D7, result in D0*:
  `Register<D0(A3, D3, D4, D7)>`, not `Pascal`.  Fix the declaration, and switch
  the object to `REGISTER_FUNCTION_PTR(ROMlib_dotext, D0(A3, D3, D4, D7))`
  (`EXTERN_REGISTER_FUNCTION_PTR` in `tesave.h`).  `TEDoText` itself stays a
  `lowmem:` of type `ProcPtr` in `TextEdit.yaml`; the new UPP type is
  Executor-only.  This is the one U1 phase that changes guest-visible behaviour.
  The commented-out implementation also wrote `A0 = thePort` on return; that is
  part of the contract and is implemented as an explicit write inside
  `C_ROMlib_dotext` (see U1a), not as a new calling-convention descriptor.

The four `Pr*` functions keep their `PASCAL_FUNCTION_PTR` lines; after the
signature change their types are the already-declared `QDGetPicUPP` and
`QDPutPicUPP`, so no new declaration is needed.  Their only uses are inside the
two `#if 0` blocks in `prPrinting.cpp`'s `ourinit()`; the live code assigns
`&StdGetPic`/`&StdPutPic`.

## Sub-steps

Each step is validated with a full build (all front-ends, both test suites), a
`ninja clean && time ninja` in `build/` with debug and stripped binary sizes, a
`--logtraps` structure diff, and a `-G CIncludes` diff (only U1 changes it, in
the two places noted above).  Commit after each step.

- **U1 — declarations and aliases.**  Split into six commits, in this order.
  None of them touch the template machinery.  Verify after each: builds, both
  suites, CIncludes diff.  U1c (the new `ExecutorUnixMount` inline) and U1f
  (the four new API typedefs) should move it; U1d does not, because the
  CIncludes generator skips `MPW.h` outright.
  - **U1a — `dotext` fix.**  Add an Executor-only `funptr` in `TextEdit.yaml`
    for `INTEGER(TEPtr, INTEGER, INTEGER, INTEGER)` with
    `Register<D0(A3, D3, D4, D7)>` (`TEDoTextUPP`); switch
    `ROMlib_dotext` to `REGISTER_FUNCTION_PTR`/`EXTERN_REGISTER_FUNCTION_PTR`.
    Also make `C_ROMlib_dotext` a single-exit function that ends with
    `EM_A0 = US_TO_SYN68K(qdGlobals().thePort);` before returning its result.
    That reproduces the `A0 = thePort` half of the `TEDoText` contract as a
    deliberate hack instead of a new descriptor: the generated marshaller
    writes the result to `D0` after the call and does not touch `A0`, so the
    write survives.  Note the side effect: the two native (short-circuit) call
    paths also write `EM_A0`; neither reads `A0` back, and
    `ROMlib_call_TEDoText` ignores it, so this is inert there.
    The only guest-visible behaviour change in U1 — exercise it with an app that
    really edits text, not just the self-tests.
  - **U1b — Sound Manager API.**  Add an opaque `ModifierStub` struct to
    `SoundMgr.yaml` (Executor-only — it is documented in Inside Macintosh but
    absent from Apple's headers), then `SndModifierUPP`
    (`Boolean(SndChannelPtr, SndCommand*, ModifierStub*)`, `only-for:
    Executor`) and use it for `snth5`.
  - **U1c — Executor extension API.**  New `defs/Executor.yaml` declaring
    `ExecutorUnixMount` as a plain function with the `go.h` call sequence
    (`m68k-inline: [0x2078, 0x0058, 0x2068, 0x0020, 0x4E90]`) and
    `executor_marshaling: true` (a new key, consumed in U2) — no `trap:`, no
    `executor:`, no `only-for:`.
    No source change: `C_unixmount`, `PASCAL_FUNCTION_PTR(unixmount)` and the
    manual table entry in `init.cpp` stay as they are.
  - **U1d — `mpw` type cleanup.**  Use `int32_t` — not `int`, not `LONGINT` —
    for the exported `C_mpw*` signatures and for the matching `MPW*ProcPtr`
    types in `MPW.yaml`.  Internal helpers (`getEntry` and locals) may stay
    `int`.  `int`/`LONGINT`/`int32_t` are one type on the supported ABIs, so
    this only changes source spelling.  `MPW.h` is not part of the CIncludes
    output (`make-multiverse.rb` skips it), so CIncludes does not move.
  - **U1e — align the `Ptr`/`Peek` functions with the declared UPPs.**
    `ROMlib_filebox` takes `DialogPtr` (cast to `DialogPeek` inside);
    `PrGetPic`/`PrPutPic`/`donotPrGetPic`/`donotPrPutPic` take
    `void*`/`const void*` to match `QDGetPicUPP`/`QDPutPicUPP`.
  - **U1f — remaining types and aliases.**  Add `MenuDefUPP`, `MenuBarDefUPP`,
    `ListDefUPP`, `ControlDefUPP` to the API YAMLs.  Add
    `ResourceDecompressionUPP` (`void(Ptr, Ptr, Ptr, Size)`, `only-for:
    Executor`) to `ResourceMgr.yaml` — it is the `dcmp` resource decompressor
    used by `resOpen.cpp`, and its signature exists nowhere else.  Replace
    `pritemprocp`/`useritemp`/`modalprocp` with
    `UserItemUPP`/`UserItemUPP`/`ModalFilterUPP`.  Keep `prinitprocp`/`cmpf`
    (already covered by trap signatures).

- **U2 — emit the marshaller set.**  The generator emits all four functions for
  every key in the set:

  | function | key | body |
  |---|---|---|
  | `invokeFrom68K` | `(Sig, CC)` | current `gen_68k_entry` / `gen_68k_register_entry` / CCall invoker body, with `impl` replaced by `f(args...)` and the compile-time trap name replaced by `ep->name` |
  | `invokeFromPPC` | `Sig` | current `gen_ppc_entry` body, same substitution |
  | `invokeUPP` | `(Sig, CC)` | initially delegates to `callto68K::Invoker` (removed in U4) |
  | `createCallbackUPP` | `(Sig, CC)` | installs the callback, checks breakpoints, calls `invokeFrom68K` |

  `invokeFrom68K` and `invokeFromPPC` log with the same data-driven scheme as
  the generated entrypoints: the defining module emits `logArg_<i>` for every
  argument/return type of the marshallers it defines and the marshaller builds
  the `lgvals` array; the name comes from `ep->name`.  This is what keeps the
  callback path from re-instantiating the variadic `logTrapCall` templates.

  The candidate record that feeds the key set must carry the decomposed
  convention (`retconv`, `argconvs`, `extras`, or nil for Pascal/CCall), not
  just the `cc` string, because the 68K body is generated from the descriptors.

  All four functions are emitted for every key, trap signatures included.
  The generated code stays proportional to the set because the bodies are
  non-template straight-line code; the ~800 trap keys add no template
  instantiations.  In particular `createCallbackUPP` is not restricted to the
  signatures that `*_FUNCTION_PTR` objects happen to use today: it is a generic
  adapter, and the emitted set is defined by the signature set, not by its
  current callers.

  Also in U2: `UPP<>::operator()` forwards to `invokeUPP`; `WrappedFunction<>::init()`
  calls `createCallbackUPP` and, when `libname` is set, registers its PPC entry
  with `invokeFromPPC`; raw handlers get `invokeFromRaw68K`; the PPC variant
  adapters keep their generated `entry_*_ppc` shape (with the extra
  `traps::Entrypoint*` parameter) because they call a wrapper object, not a free
  function.

  Validation: behaviour must be byte-identical for `--logtraps`; the two
  consumers (UPP calls and callbacks) now exercise `invokeUPP`/`invokeFrom68K`
  while the entrypoints still use their own `entry_*` code, so the two paths
  can be compared.

- **U3 — entrypoints use the marshallers.**  Change `GeneratedEntrypoint` to
  store `Entry68KFn`/`EntryPPCFn`; `handle68K`/`handlePPC` keep the breakpoint
  check and call them with `(addr, this)` / `(cpu, this)`.  Generate the wrapper
  constructors (in `trap_entries/<module>.cpp`) with the two non-capturing
  lambdas; the implementation is named inside the lambda and the marshaller
  checks its type.  Update `GeneratedDispatcherTrap::addSelector` and `SelectorEntry` to carry the entry
  function and the subtrap's `traps::Entrypoint*` (the dispatcher keeps its own
  breakpoint check on the subtrap).  Delete `gen_68k_entry` / `gen_ppc_entry`
  for everything except the PPC variant adapters.

  This is the step where the per-trap decode/encode code disappears.  The
  emitted `trap_entries/*.cpp` shrink to wrapper constructors, reference
  functions and the per-signature marshallers.  `ReferenceEntries_<module>`
  simply odr-uses every wrapper object in the module.

- **U4 — replace `invokeUPP`'s delegating body and delete the invokers.**
  Generate the straight-line host→guest body, mirroring `callto68K` exactly:
  Pascal pushes the return slot then the arguments, saves/restores D1–D7/A0–A6
  around `execute68K` and pops the result; Register applies `ArgConv::set` in
  declaration order, executes, applies `afterCall` in reverse order and reads
  the return with `RetConv()`.  Delete `callto68K`, `callfrom68K` and
  `callfromPPC::Invoker`; keep `readCArg` (moved to `callconv`),
  `ParameterPasser`, `stack` and the descriptors.

- **U5 — sweep and docs.**  Delete `LoggedFunction`/`makeLoggedFunction` and
  the variadic logging helpers that become unreachable; delete whatever else
  is left of the invocation machinery; update
  `docs/subsystems/trap-dispatch.md`, `docs/architecture.md`, `AGENTS.md`.

## U1 — done

Landed as six commits (U1a–U1f):

- `TEDoTextUPP` is declared with the real `Register<D0(A3, D3, D4, D7)>`
  convention, and `C_ROMlib_dotext` writes `A0 = thePort` on every return.
- `SndModifierUPP` and the opaque `ModifierStub` exist; `snthfp` is gone.
- `Executor.yaml` declares `ExecutorUnixMount` with `executor_marshaling: true`.
- The MPW function-pointer types are spelled `int32_t` on both sides.
- `ROMlib_filebox` is a `UserItemUPP`, and the four `Pr*Pic` helpers match
  `QDGetPicUPP`/`QDPutPicUPP`.
- `MenuDefUPP`, `MenuBarDefUPP`, `ListDefUPP`, `ControlDefUPP` and
  `ResourceDecompressionUPP` exist; every hand-written `UPP<...>` alias except
  `prinitprocp`/`cmpf` is gone.

Validation per commit: 138/138 native tests, Mac suite 90 passed + the two known
`FileTest` xfails, `--logtraps` structure identical, and only the expected
CIncludes changes.

Measurements (Debug, full clean builds): wall 41.9–42.6 s throughout; `executor`
66,128,592 → 66,191,744; stripped 9,582,568 unchanged; `libromlib.a`
119,811,898 → 119,967,390, almost all of it the two empty per-file TUs that
`Executor.yaml` now generates.

## Signature identity (U2)

The overloads are emitted once per distinct emitted **C++ type**.  Names are
only aliases, so the key ignores them, and it is computed on the spelling that
will actually appear in the generated code — after `decl()` guestifies it — not
on the YAML spelling.

### What goes into the key

A candidate is `(ret, [arg types], cc)`:

- for a generated wrapper: `e[:ret]`, `e[:arg_types]`, `e[:cc]`;
- for a `funptr:`: its `UPP<...>` arguments built exactly as `declare_funptr`
  builds them (the return type as written, the arguments through `decl`);
- for an `executor_marshaling: true` function: `arg_types` as
  `declare_function` computes them.

`cc` is normalised by stripping the `callconv::` prefix (`declare_funptr` emits
`Register<...>`, wrappers emit `callconv::Register<...>`) and by normalising
whitespace.  The convention is part of the key: the same `Sig` occurs with
several of them (`void(Handle)` is Pascal for `DisposeHandle` and
`Register<void(A0), ReturnMemErr<D0>>` for the trap).

`invokeFrom68K`, `invokeUPP` and `createCallbackUPP` use the full `(Sig, CC)`
key.  `invokeFromPPC` drops `CC` and uses `(ret, [arg types])`: on PowerPC the
arguments are read from the PPC register/spill area regardless of the 68K
descriptors, so two conventions with one signature need one body.

### Canonicalising a type

1. Normalise whitespace and `const` placement (`T*`/`T *`,
   `const T*`/`T const*`).
2. Resolve every `typedef:` name recursively to its definition, including
   inside `GUEST<...>` (`GUEST<INTEGER>` ≡ `GUEST<int16_t>`).  Typedefs are
   transparent in C++, so this is exactly type identity.
3. Stop at `struct`, `union` and `enum` names, and at names declared in
   hand-written headers (`ProcPtr`, `OpaqueUntyped68KProc`).  An enum is a
   distinct type even when its representation is `int16_t`; `ProcPtr` must not
   collapse to `void*`.
4. Preserve `const`, pointer depth and `GUEST<>` — each changes the type
   (`QDGetPicUPP` and `QDPutPicUPP` differ only by `const`).  The one exception
   is `GUEST<T>` when `T` is a `GUEST_STRUCT`: `GuestType<T>::type` is `T`, so
   `GUEST<AEDesc>` and `AEDesc` are the same type.  (The generator knows the
   guest structs: they are the `struct`/`union` items.)
5. In parameter position, an array type decays to a pointer (`FormatType`,
   a typedef for `char[4]`, is the same parameter type as `char*`).  No current
   signature needs this, but the rule belongs with the rest.

Typedef resolution needs a generator-wide table, built once from the same view
the Executor generator uses: `only-for`/`not-for` filtered with
`filter_key = "executor"`, and `api: carbon` items skipped (Executor's
`make_api_ifdef` drops them).  That picks the Executor definition of the few
duplicated names (`IOMiscType` = `uint32_t`, `CGrafPtr` = `CGrafPort*`).
Identical duplicates (`Boolean`, `UniversalProcPtr`) are harmless; a
conflicting one is a generation-time error.  Memoise and detect cycles.

The generator only canonicalises YAML spellings.  Whether a hand-written
`decltype(C_NAME)` matches is decided by the C++ compiler at the call site: an
`int` in source and an `int32_t` in the YAML are the same type on the supported
ABIs.  U1d removes that particular mismatch by spelling the MPW signatures
`int32_t` on both sides.

The definition of each marshaller is emitted in the module that first declares
the key, using that module's spelling of the types (the representative).  Every
name in it is therefore in scope in that translation unit, which is why the
representative spelling, not the canonical spelling, is what the body uses.

### Failure modes

Both mistakes are compile-time errors:

- **Under-merge** (two spellings of one type kept apart) emits two identical
  overloads → redefinition.
- **Over-merge** (two different types collapsed) leaves one type without an
  overload → `no matching function` at its use.

As an extra guard against over-merging, every spelling is checked against its
canonical rendering in the module that mentions it:
`static_assert(std::is_same_v<UPP<local>, UPP<canonical>>)`.  That spelling's
names are all in scope there, so a wrong canonicalisation is a generation-time
compile error even when the type is otherwise unused.

## Current state

The generator already emits, per distinct canonical signature:

- an `invokeUPP` declaration in every module that mentions it, and one
  definition (delegating to `callto68K`) in the module that first declares it;
- a `createCallbackUPP` declaration in every mentioning module, and a
  definition only where a `WrappedFunction` can use it (the `funptr:`
  signatures and the `executor_marshaling: true` functions);
- a `static_assert` in each mentioning module, comparing that module's spelling
  with the generated canonical spelling.

813 `invokeUPP` definitions (all distinct signatures except the four CCall
ones), 56 `createCallbackUPP` definitions and 1070 `static_assert`s.
`UPP<>::operator()` and `WrappedFunction<>::init()` use the generated
functions.  The remaining work is U2 (the full set and the straight-line
bodies), U3 (entrypoints), U4 (delete the invokers) and U5 (sweep).

Validation: 138/138; Mac suite 90 passed + the two known `FileTest` xfails;
`--logtraps` structure identical; `-G CIncludes` unchanged.

Measurements (Debug, full clean build): wall 42.6 s → 44.0 s; `executor`
66,191,744 → 67,620,216; stripped 9,582,568 → 9,836,520; `libromlib.a`
119,967,390 → 122,232,512.

## Risks and open questions

- **Per-trap entry lambdas.**  Each wrapper gets one tiny non-capturing lambda
  per direction (around 2000 in total) instead of a shared per-signature entry
  plus an erased implementation pointer.  The lambda's function-pointer target
  is used only inside the defining TU, where the global wrapper object is
  constructed; its identity is never compared across TUs.  `--logtraps` and
  breakpoints are keyed on `Entrypoint::name`, not on the pointer.  Debug
  symbols show closure names for these frames; the marshaller body remains one
  named function per signature.
- **Signature canonicalisation.**  The key is the emitted C++ type, not the
  YAML spelling; the rules and failure modes are above.  `cdef1008` vs
  `cdef0` is the smallest example of two spellings that must collapse.
- **Definition spelling.**  The marshaller body is emitted in the first
  declaring module; its argument and return types must be spellable there.  A
  type that exists only in another module's header is a `no matching
  function`/unknown-name error at generation-compile time.
- **Overload visibility.**  The generated overload declarations must be visible
  (by ordinary lookup or ADL) at every instantiation of `UPP<>::operator()` and
  of `WrappedFunction<>::init()`; a miss is a hard `no matching function`, which
  is the point of choosing overloads.
- **Logging equivalence for callbacks.**  The callback path moves from
  `LoggedFunction`/`logTrapCall` to the data-driven printers.  `logValue` and
  `logList` produce the same text, and `--logtraps` over the Mac suite is the
  check, but any `*_FUNCTION_PTR` that appears in the golden log is the place
  to look if it changes.
- **`createCallbackUPP` for all keys.**  It is a thin adapter now, so emitting
  it for the ~800 trap signatures adds one small function per key; the
  measurement after U2 is the check.  The set is not narrowed to its current
  callers.
- **CCall `invokeUPP` is not emitted.**  No CCall UPP is ever invoked
  host→guest today (the `MPW*ProcPtr` types are stored in guest structs and
  called by guest code), and `callto68K` has no CCall invoker to mirror.  A
  host→guest CCall would therefore be a link error rather than dead code.  If
  one appears, the body is the C-caller mirror of the 68K callback: read the
  arguments from the stack, call `execute68K`, pop them again, result in `D0`.
- **CCall `invokeFrom68K`.**  That direction is generated and has no current
  guest entrypoint exercise; its only users are the six `mpw` callbacks.  It is
  generated from the same rules as the old CCall invoker; `readCArg` stays for
  it.
- **Breakpoints and `--logtraps` for `*_FUNCTION_PTR`.**  `createCallbackUPP`
  keeps the owning `Entrypoint*`, so the breakpoint check and the logged name
  survive; `GeneratedEntrypoint` does the same for entrypoints.
- **`dotext`'s `A0` write.**  The commented-out implementation in
  `teInsert.cpp` also returned the current port in `A0`, not just the result in
  `D0`.  **Decided:** reproduce it with an explicit
  `EM_A0 = US_TO_SYN68K(qdGlobals().thePort)` at the end of
  `C_ROMlib_dotext` (U1a), not with a descriptor.  This relies on the generated
  marshaller leaving `A0` alone after the native call.
- **Compile time / binary size.**  The generated marshallers are per signature
  (813/838) instead of per trap (~2000), and the two invoker template chains
  plus the `LoggedFunction` instantiations disappear.  The per-trap call sites
  are one-line non-capturing lambdas with no template code.  Deleting the
  `callfromPPC::Invoker` instantiation from each of the 73 `*_FUNCTION_PTR`
  objects and the `callto68K` instantiation from each distinct invoked UPP is
  the direct compile-time win; the per-signature bodies are straight-line
  copies of code that already compiled.  Measure every sub-step; a modest win
  is a legitimate stopping point.

## Not in scope

- Removing `UPP<>` or the descriptor vocabulary — they stay.
- A lazy global native-address → callback map for `*_FUNCTION_PTR`; each object
  keeps installing its own callback through the macros.
- Any change to the guest ABI, the trap tables, or the generated entrypoint
  structure beyond the function-pointer plumbing described in U3.
- The `funptr` → `New…UPP`/`Dispose…UPP` CIncludes surface.
