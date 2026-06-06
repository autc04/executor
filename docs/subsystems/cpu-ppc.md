# PowerPC CPU Emulation (PowerCore)

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

PowerCore is a pure-interpreter PowerPC emulator used by Executor to run "Fat Binary"
and CFM (Code Fragment Manager) applications that contain a PPC code segment. Unlike
syn68k, PowerCore does not compile to native code; it interprets each PPC instruction
at runtime. This is adequate for the PPC workloads Executor sees, which are typically
short CFM entry points that quickly call back into 68K code via Mixed Mode.

## Key Concepts

**Single-instance model**: Executor creates one `PowerCore` object (accessed via
`Executor::getPowerCore()` in `src/base/cpu.h`). The object owns the full PPC
architectural state.

**Memory segmentation**: `PowerCore` holds a `memoryBases[4]` array of host pointers
that map the upper two bits of a PPC effective address to a host memory region. This
lets the guest PPC code address the same flat array that syn68k uses without a costly
full address translation on every memory access.

**Syscall / interrupt hooks**: The `syscall` and `handleInterrupt` function pointers on
`PowerCore` are set by Executor's Mixed Mode layer. When PPC code executes a `sc`
instruction (system call), `syscall` is invoked with the current `PowerCore` state,
allowing the 68K/PPC bridge to return control to the 68K world.

**Debugger hook**: The `debugger` function pointer, and the `getNextBreakpoint` pointer
for single-stepping, let `cxmon` interrupt PPC execution at arbitrary addresses.

**Cache management**: `flushCache()` and `flushCache(from, size)` discard any
interpreter state associated with a guest address range. Compared to syn68k, this is
inexpensive because PowerCore has no compiled output to discard.

**`execute()`**: The main interpreter loop. On each call it fetches, decodes, and
executes one PPC instruction, advancing `CIA` (Current Instruction Address). The loop
exits when the `syscall` hook returns a non-`~0` sentinel value or when an interrupt is
pending.

## Source Files

| Path | Description |
|------|-------------|
| `PowerCore/include/PowerCore.h` | Full public API and `PowerCoreState` struct |
| `PowerCore/src/PowerCore.cc` | Main interpreter loop and instruction dispatch |
| `PowerCore/src/PowerCoreTemplate.cc` | Instruction implementation templates |
| `PowerCore/src/PowerCoreInlines.h` | Inline helpers for common PPC operations |
| `PowerCore/generator/` | Python code-generator that produces the opcode table |
| `PowerCore/src/powerpc.ppcdef` | Machine-readable PPC instruction definitions |
| `src/base/cpu.h` | `executePPC()` — Executor-side entry point |

## Important Data Structures

- **`PowerCoreState`**: base class of `PowerCore`; holds the full PPC register file:
  - `r[32]` — general-purpose registers
  - `f[32]` — floating-point registers (host `double`)
  - `cr` — condition register
  - `lr`, `ctr` — link and count registers
  - `CIA` — current instruction address (program counter)
  - `SO`, `OV`, `CA` — summary overflow / overflow / carry bits (XER sub-fields)
- **`interruptFlag`** (`std::atomic_flag`): set by `requestInterrupt()` from any thread;
  the interpreter checks it between instructions.

## Key Functions / Methods

| Symbol | Description |
|--------|-------------|
| `PowerCore::execute()` | Main PPC interpreter loop |
| `PowerCore::flushCache(from, size)` | Invalidate interpreter state for a range |
| `PowerCore::requestInterrupt()` | Signal an interrupt from another thread |
| `getPowerCore()` | Return the singleton `PowerCore` instance |
| `executePPC(addr)` | Executor wrapper: sets CIA and calls `execute()` |

## Design Notes / Gotchas

- PowerCore is an **interpreter**, not a JIT compiler. It is intentionally simple
  because the PPC code paths in classic Mac applications are short and infrequent
  compared to the 68K code paths.
- Floating-point registers are stored as host `double`. This is a deliberate
  approximation; full IEEE 754 double-extended (80-bit) emulation is only required on
  the 68K side (handled by SANE, see `sane.md`).
- The `memoryBases[4]` scheme means only 4 distinct 1-GB regions can be mapped. This
  is sufficient for Executor's flat address space model.
- PPC code can call back into 68K code exclusively through the Mixed Mode trap
  (`MIXED_MODE_TRAP`). Direct branches from PPC to 68K addresses are not supported.
