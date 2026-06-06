# Low-Memory Globals

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

Classic Mac OS reserved the first 64 KB of the 68K address space for "low-memory
globals" — fixed-address variables that both the OS and applications read and write
directly by absolute address. Examples include `TheZone` (current memory zone),
`CurrentA5` (application globals pointer), `ROMBase`, `KeyMap`, and hundreds of
others. Executor maintains a matching region at the base of the guest address space
and provides a type-safe, byte-order-correct API for accessing it.

## Key Concepts

**`LowMemGlobal<T>`**: a simple struct that records the fixed guest address of one
low-memory variable and its C++ type:

```cpp
template<class T>
struct LowMemGlobal
{
    using type = T;
    uint32_t address;
};
```

Individual globals are declared throughout the codebase as `constexpr` (or `const`)
`LowMemGlobal<T>` values, for example:

```cpp
const LowMemGlobal<Byte[6]> macfpstate { 0xA4A };
```

**`LM(name)`**: the accessor macro / inline function that converts a
`LowMemGlobal<T>` to a `GUEST<T>&` reference located at `ROMlib_offset + lm.address`.
Because it returns a reference to a `GUEST<T>`, both reads and writes go through the
byte-swapping wrapper automatically:

```cpp
template<class T>
inline GUEST<T>& LM(LowMemGlobal<T> lm)
{
    return *(GUEST<T>*)(ROMlib_offset + lm.address);
}
```

**`ROMlib_offset`**: the host base address of the flat guest memory array. Adding a
`syn68k_addr_t` to `ROMlib_offset` yields a host pointer. This value is set during
memory initialisation and never changes during a session.

**`GUEST<T>`**: the byte-swapping wrapper for any value that lives in guest memory.
Assigning a host value to a `GUEST<T>` stores it in big-endian order; reading from a
`GUEST<T>` returns it in host byte order. For big-endian hosts the wrapper is a no-op.

## Source Files

| Path | Description |
|------|-------------|
| `src/base/lowglobals.h` | `LowMemGlobal<T>` struct and `LM()` accessor |
| `src/base/mactype.h` | `GUEST<T>`, `GUEST_STRUCT`, `SwapTyped()` |
| `src/base/byteswap.h` | `swap16()`, `swap32()`, `swap64()` primitives |
| `multiversal/` | Mac OS header tree; `LowMem.h` lists all standard globals |

## Important Data Structures

- **`GUEST<T>`**: wraps `T` in a struct with `operator=` and conversion operators that
  call `SwapTyped`. Arithmetic between two `GUEST<T>` values is intentionally a
  compile error, enforced by the type system.
- **`GUEST_STRUCT`**: a macro placed at the top of any struct whose instances may
  reside in guest memory. It disables implicit construction and forces all fields to be
  `GUEST<>`-wrapped. The compile-fail tests in `tests/guestvalues.compfail.cpp` verify
  that mixing guest and host types is rejected at compile time.

## Commonly Used Globals

| Name | Type | Address | Description |
|------|------|---------|-------------|
| `TheZone` | `THz` | 0x118 | Current heap zone |
| `ApplZone` | `THz` | 0x2AA | Application heap zone |
| `SysZone` | `THz` | 0x2A6 | System heap zone |
| `CurrentA5` | `Ptr` | 0x904 | Application globals pointer |
| `ROMBase` | `Ptr` | 0x2AE | Base of ROM (mapped to Executor's built-in data) |
| `MemErr` | `OSErr` | 0x220 | Last Memory Manager error |
| `WMgrCPort` | `CGrafPtr` | 0xD2C | Window Manager colour port |
| `KeyMap` | `KeyMap` | 0x174 | Current keyboard state bitmap |
| `macfpstate` | `Byte[6]` | 0xA4A | SANE floating-point state |

## Design Notes / Gotchas

- **Never use a raw pointer or integer to access low-memory globals.** Always go
  through `LM()`. Raw access bypasses byte-swapping and is the most common source of
  silent data corruption on little-endian hosts.
- The `LowMemGlobal<T>` declarations are scattered across all subsystem headers, not
  centralised in one file. When adding a new global, declare it in the header for the
  subsystem that owns it.
- The guest address `0` is valid (it is within the low-memory region) so
  `SYN68K_TO_US_CHECK0` must not be used to translate pointers that might legitimately
  point to low memory; use `SYN68K_TO_US` directly in those cases.
- Changes to the `GUEST<T>` layout or `GUEST_STRUCT` macro require a full rebuild
  because they affect every translation unit that includes `mactype.h`.
