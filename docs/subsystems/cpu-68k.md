# 68K CPU Emulation (syn68k)

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

syn68k is the dynamic recompiler responsible for executing Motorola 68K machine code
inside Executor. It translates 68K instruction blocks into native host code at runtime,
caching the result so subsequent executions of the same block are fast. All Mac
application code — and Executor's own toolbox stubs that return to guest code — runs
through syn68k.

## Key Concepts

**Address space**: The entire Mac guest address space is a single flat byte array in host
memory. `ROMlib_offset` is the host base address of that array. The type
`syn68k_addr_t` (a `uint32_t`) is a guest address. The macros `SYN68K_TO_US(addr)` and
`US_TO_SYN68K(ptr)` translate between guest addresses and host pointers. Null-tolerant
variants (`SYN68K_TO_US_CHECK0`, `US_TO_SYN68K_CHECK0`) handle the zero address
without crashing.

**Block caching**: syn68k groups 68K instructions into basic blocks and compiles each
block once. `ROMlib_destroy_blocks(start, count, flush_only_faulty)` invalidates cached
blocks in a guest address range — this must be called any time Executor writes new
machine code into the guest address space (e.g. when installing trap glue stubs or
patching applications).

**A-line traps**: The 68K `A-line` exception vector (trap number `0xA000`–`0xAFFF`) is
the primary mechanism by which Mac applications call OS services. syn68k routes
unrecognised A-line instructions to `alinehandler` in `src/base/dispatcher.cpp`.

**Register file**: syn68k maintains the emulated 68K register file. Registers are
accessed by the rest of Executor using macros such as `EM_D0`, `EM_A0`, `EM_A7`
(stack pointer). The register union is defined in `syn68k_public.h` and handles byte /
word / long sub-register access without affecting the remaining bytes of the register,
mirroring real 68K behaviour.

**Interrupts**: syn68k operates in `SYNCHRONOUS_INTERRUPTS` mode; it polls for
interrupts at safe points rather than being asynchronously interrupted by the host OS.
This simplifies synchronisation between the emulator core and the host event loop.

**Endianness**: syn68k emulates a big-endian CPU. All values stored in the guest address
space are therefore big-endian. The `GUEST<T>` wrapper in `src/base/mactype.h` handles
byte-swapping transparently on little-endian hosts.

## Source Files

| Path | Description |
|------|-------------|
| `syn68k/include/syn68k_public.h` | Public API — address types, register union, callback install |
| `syn68k/include/syn68k_private.h` | Internal structures used by the translator |
| `syn68k/runtime/` | Runtime support code linked into the host |
| `syn68k/syngen/` | Instruction-set description language and generator |
| `src/base/dispatcher.cpp` | `alinehandler` — entry point for every Mac trap |
| `src/base/cpu.h` | `execute68K()`, `executePPC()`, `ROMlib_destroy_blocks()` |

## Important Data Structures

- **`syn68k_addr_t`** (`uint32_t`): a guest address within the flat Mac address space.
- **`REGTYP` / `cpu_state_t`**: the syn68k internal register file; accessed externally
  via `EM_D0`…`EM_D7`, `EM_A0`…`EM_A7`.
- **`CPUMode`** (enum in `cpu.h`): distinguishes `m68k` vs `ppc` execution context;
  used by trap dispatch and Mixed Mode to know which calling convention is active.

## Key Functions

| Symbol | Location | Description |
|--------|----------|-------------|
| `execute68K(addr)` | `src/base/cpu.h` | Run the 68K emulator starting at a guest address |
| `alinehandler(pc, ignored)` | `src/base/dispatcher.cpp` | A-line exception handler; dispatches every Mac trap |
| `ROMlib_destroy_blocks(start, n, faulty)` | `src/base/cpu.h` | Flush syn68k block cache for a guest address range |
| `callback_install(fn, data)` | `syn68k_public.h` | Install a host callback reachable from 68K code |

## Design Notes / Gotchas

- **Never write to the guest address space without calling `ROMlib_destroy_blocks`**
  on the modified range. Stale cached translations will silently execute the old code.
- syn68k originates from ARDI's 1990s commercial work and predates C++17; the
  internal code is C with some C++ wrapping at the integration layer.
- `SYNCHRONOUS_INTERRUPTS` means the host must not call syn68k re-entrantly. All
  interrupt delivery goes through the emulator's own polling mechanism.
- The `callback_install` mechanism is how Executor installs native C++ functions as
  68K-callable addresses. Every `TrapFunction` init path calls this to create the
  in-guest stub that eventually reaches the C++ implementation.
