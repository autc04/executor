# UPP<> and calling-convention refactor (plan)

> Date: 2026-10-08
> Status: **designed, not started.**

The goal: make the *generator* the single source of truth for calling
conventions, so the hand-written invocation machinery in
`src/base/functions.impl.h` can go, without giving up `UPP<>`'s role as the
host-side representation of a guest procedure pointer.

## Decision

`UPP<F, CC>` **remains a template**.  Only the calling-convention functionality
moves out, into **two overload sets emitted by multiversal**:

```cpp
// host call -> guest (what UPP<>::operator() does today)
Ret  invokeUPP(const UPP<Ret(Args...), CC>&, Args...);

// native function -> guest callback (what WrappedFunction::init does today)
void createCallbackUPP(Ret (*f)(Args...), UPP<Ret(Args...), CC>& out);
```

Both are keyed on the **UPP type**, which already carries both `Sig` and `CC`,
and are **deduplicated by type identity, not by name**.  Names are only aliases
for `UPP<...>`; two names with the same signature are the same type and must map
to the same overload.

Then:

- `UPP<>::operator()` becomes a one-line forwarder to `invokeUPP`.
- The `*_FUNCTION_PTR` macro family **stays**, providing the immediate wrapper
  class (today `WrappedFunction<>`); it calls `createCallbackUPP`.
- The `callconv` **meta-language declarations stay** in `functions.h`/`impl.h`
  (the vocabulary the YAML and the generated code speak); most of the
  *implementation* — the invokers — goes.

## Why this shape

- **Signature identity is automatic.** Two names for the same signature are the
  same `UPP<...>` template-id, so they are the same type with no aliasing or
  conversion machinery.  This is load-bearing: `QDRectUPP` and `QDOvalUPP` are
  both `UPP<void(GrafVerb, const Rect *)>`, and `qHooks.cpp` compares a stored
  `QDRectUPP` against `&StdRect` and a `QDOvalUPP` against `&StdOval`.  A
  class-per-name design would need explicit aliasing or matcher machinery to
  preserve that.
- **Every type is spellable.** A generated wrapper's `operator&` can return
  `UPP<Sig, cc>` for any trap, so no type has to be minted for signature-only
  types and none of the 64 `&Trap` sites change.
- **The convention is part of the overload's key.** The calling convention is not
  in the parameter list (`DisposeHandle` is `void(Handle)` as Pascal and
  `Register<void(A0), ReturnMemErr<D0>>` as a trap), but it is part of
  `UPP<..., CC>`, so the two overloads do not collide.  Overloading on the
  parameter list alone would conflate them.
- **Overloads are preferable to template specialization here.** A missing
  overload is a `no matching function` naming the UPP type; a missing
  specialization is an `incomplete type` error, or ill-formed with no diagnostic
  if a primary template exists.  The two directions (host→guest and
  guest→native) also stay independent overload sets instead of one specialization
  carrying both.
- **`createCallbackUPP` needs no witness argument.** Its output parameter is
  typed `UPP<...>&`, which already carries `CC`; a bare `Ret (*)(Args...)`
  parameter could not, so a witness argument would otherwise be required.

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
| descriptors `D<n>`, `A<n>`, `D0HighWord`, `D0LowWord`, `Out<>`, `InOut<>`, `TrapBit<>` | `functions.impl.h` | **stay** (generated entry code uses them) |
| extras `ReturnMemErr<>`, `CCFromD0`, `ClearD0`, `SaveA1D1D2`, `MoveA1ToA0` | `functions.impl.h` | **stay** |
| `callconv::stack::push/pop` | `functions.impl.h` | **stay** (generated Pascal entries) |
| `callfrom68K` (`Invoker`, `PascalInvoker`, `RegInvoker`, `readCArgsAndCall`) | `functions.impl.h` | **goes** |
| `callto68K` (`Invoker`, `InvokerRec`) | `functions.impl.h` | **goes** |
| `callfromPPC::Invoker` | `functions.impl.h` | **goes** (only reachable through `WrappedFunction::init()` when `libname` is non-null, and the `*_FUNCTION_PTR` macros never set it) |
| `callfromPPC::ParameterPasser` | `functions.impl.h` | **stays** (generated PPC entries) |
| `UPP<>::operator()` | `functions.impl.h` | stays, becomes a forwarder |
| `WrappedFunction<>` + `operator&` | `traps.h` | stays (the `*_FUNCTION_PTR` wrapper class) |
| `WrappedFunction<>::init()` | `traps.impl.h` | stays, uses `createCallbackUPP` instead of the invokers |
| `PASCAL/REGISTER/CCALL_FUNCTION_PTR` (+ `EXTERN_`/`DEFINE_`) | `traps.h` | stays |

### Who uses what (verified)

- `callto68K` is used **only** by `UPP<>::operator()`.
- `callfrom68K` and `callfromPPC::Invoker` are used **only** by
  `WrappedFunction<>::init()`.
- The descriptors are used by both `callto68K`/`callfrom68K` and the **generated
  entrypoints** (`build/src/trap_entries/*.cpp`: `callconv::D0 c0; x = c0;`,
  `callconv::ReturnMemErr<D0>` in `extras`), so they are the shared primitive
  layer and stay.

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
instantiation, so its `init()` body must compile.  In U2 that body calls
`createCallbackUPP`, so the `(Sig, CC)` pair of **every** one of these 73 objects
must have a declared UPP to overload on.  That is also the coverage check: the
existing explicit instantiation turns a missing declaration into a build-time
`no matching function` naming the offending object, so no separate test is
needed.

Checked mechanically against the generated `build/src/api/*.h`, plus by hand for
every typedef-spelling case (`Ptr` vs `char*`, `int` vs `int32_t`, `short` vs
`int16_t`, `DialogPeek` vs `DialogPtr`, `DialogRecord*` vs `CInfoPBRec*`).  The
numeric-typedef equivalences were confirmed with `__is_same` on this host
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
- `ControlDefUPP`: `SInt32(SInt16 varCode, ControlRef theControl, ControlDefProcMessage message, SInt32 param)`

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
  so its `(Sig, CC)` is added to the U2 overload set even though no wrapper is
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
the three places noted below).  Commit after each step.

- **U1 — declarations and aliases.**  Split into six commits, in this order.  None
  of them touch the template machinery.  Verify after each: builds, both suites,
  CIncludes diff.  U1c (the new `ExecutorUnixMount` inline), U1d (MPW prototypes
  re-spelled `LONGINT` → `int32_t`, same type) and U1f (the four new API
  typedefs) should move it.
  - **U1a — `dotext` fix.**  Add an Executor-only `funptr` in `TextEdit.yaml`
    for `INTEGER(TEPtr, INTEGER, INTEGER, INTEGER)` with
    `Register<D0(A3, D3, D4, D7)>` (`TEDoTextUPP`); switch
    `ROMlib_dotext` to `REGISTER_FUNCTION_PTR`/`EXTERN_REGISTER_FUNCTION_PTR`.
    Also make `C_ROMlib_dotext` a single-exit function that ends with
    `EM_A0 = US_TO_SYN68K(qdGlobals().thePort);` before returning its result.
    That reproduces the `A0 = thePort` half of the `TEDoText` contract as a
    deliberate hack instead of a new descriptor: the generated
    `createCallbackUPP` thunk writes the result to `D0` after the call and does
    not touch `A0`, so the write survives.  Note the side effect: the two native
    (short-circuit) call paths also write `EM_A0`; neither reads `A0` back, and
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
    this only changes source spelling and the CIncludes prototypes.
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
- **U2 — emit the two overload sets as a behaviour-preserving seam.**  Generate an
  `invokeUPP` and a `createCallbackUPP` overload for every distinct `(Sig, CC)`
  in the union of (a) the signatures the generator already emits wrappers for,
  (b) every `funptr:` type, and (c) every `function:` marked
  `executor_marshaling: true`.  The bodies **delegate to the existing
  `callto68K`/`callfrom68K`**.  (c) is the escape hatch for a declaration that
  produces no wrapper but still needs a marshaller; `ExecutorUnixMount` is the
  only user, and it provides `OSErr(CInfoPBRec *)` to the existing
  `PASCAL_FUNCTION_PTR(unixmount)`.  Do not widen this to every declared
  function: many `- function:` items are CIncludes-only prototypes with no
  Executor implementation, and their signatures would only add dead overloads.
  Switch `UPP<>::operator()` and `WrappedFunction<>::init()` to them.  Behaviour
  byte-identical; this is the seam that lets U3 delete the invokers with no
  other change.
- **U3 — move the marshalling bodies in and delete the invokers.**  Replace the
  delegating bodies with straight-line generated marshalling (using the
  descriptors, `callconv::stack`, `ParameterPasser`); delete `callto68K`,
  `callfrom68K`, `callfromPPC::Invoker`.
- **U4 — traps.**  Emit `invokeUPP` for **non-selector** trap signatures and route
  the generated wrappers' patched path through it.  Dispatcher subtraps need
  nothing here: they are constructed with `trapno == 0`, so
  `GeneratedEntrypoint::isPatched()` is always false and their `operator()` never
  marshals into the guest.  (`operator&` still returns `UPP<Sig, cc>`, but that
  only needs the type, not the overload.)
- **U5 — sweep and docs.**  Delete whatever is left of the invocation machinery;
  update `docs/subsystems/trap-dispatch.md`, `docs/architecture.md`, `AGENTS.md`.

## Risks and open questions

- **Deduplication is by type identity, so the `(Sig, CC)` key must canonicalise
  exactly.**  `cdef1008` spells the same types as `cdef0` (`int16_t`/`int32_t` ≡
  `INTEGER`/`LONGINT`), so the generator must not emit two overloads for what is
  one `UPP<...>` type, and a textual-vs-type mismatch must be caught (a duplicate
  overload is a redefinition error, which is at least diagnosed).
- **Overload visibility.**  The generated overload declarations must be visible
  (by ordinary lookup or ADL) at every instantiation of `UPP<>::operator()` and
  of `WrappedFunction<>::init()`; a miss is a hard `no matching function`, which
  is the point of choosing overloads.
- **Breakpoints and `--logtraps` for `*_FUNCTION_PTR`.**  Today the per-object
  lambda in `WrappedFunction::init()` captures `this` for `checkBreak68K` and the
  logged name (the golden `--logtraps` output filters `ROMlib_vcatch`, so these
  do appear in today's log).  A generated `createCallbackUPP` keyed only on
  `(Sig, CC)` has no per-object state.  Decide whether to thread the
  `Entrypoint*` through as `callback_install`'s `arbitrary_argument` (the thunk is
  then `(Sig, CC)`-generated but calls back through the object for the breakpoint
  check and the name), or to drop name/breakpoint support for these objects.
- **`createCallbackUPP` naming/ergonomics.**  The `UPP<...>& out` parameter
  carries the convention; the two names (`invokeUPP`/`createCallbackUPP`) are
  provisional and exist mainly so the pair is greppable.
- **Register- and CCall-convention `*_FUNCTION_PTR`s.**  Covered by existing
  declarations: the `serial`/`mactcp` ones (`Register<D0(A0,A1)>`) by
  `DriverUPP`, and the `mpw` `CCall` ones by the `MPW*ProcPtr` types.
- **`dotext`'s `A0` write.**  The commented-out implementation in
  `teInsert.cpp` also returned the current port in `A0`, not just the result in
  `D0`.  **Decided:** reproduce it with an
  explicit `EM_A0 = US_TO_SYN68K(qdGlobals().thePort)` at the end of
  `C_ROMlib_dotext` (U1a), not with a descriptor.  This relies on the generated
  thunk leaving `A0` alone after the native call — U2/U3 must keep that true for
  `Register` conventions that do not mention `A0`.
- **Compile time / binary size.**  Hypothesis: deleting the per-argument
  recursion chain helps the generated `trap_entries/*.cpp` (they currently
  instantiate the invokers); the per-`(Sig, CC)` `UPP<>` instantiation itself
  stays.  Measure every sub-step; a modest win is a legitimate stopping point.

## Not in scope

- Removing `UPP<>` or the descriptor vocabulary — they stay.
- A lazy global native-address → callback map for `*_FUNCTION_PTR`; each object
  keeps installing its own callback through the macros.
- Any change to the guest ABI, the trap tables, or the generated entrypoint
  structure.
- The `funptr` → `New…UPP`/`Dispose…UPP` CIncludes surface.
