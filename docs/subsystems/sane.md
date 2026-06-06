# SANE Floating-Point

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Purpose

SANE (Standard Apple Numeric Environment) is the Mac OS floating-point library.
It provides 80-bit extended-precision arithmetic, trigonometric and transcendental
functions, IEEE 754 exception handling, and decimal-to-binary / binary-to-decimal
conversion. On 68K Macs, SANE was implemented partly in the 68881/68882 FPU and partly
in software traps. Executor reimplements SANE in C++ using the host's `long double`
type as a proxy for the 80-bit `extended` type.

## Key Concepts

**`ieee_t` (`long double`)**: Executor's internal representation of an 80-bit IEEE
value. On x86-64 Linux, `long double` is the 80-bit x87 extended type, making the
mapping exact. On platforms where `long double` is 64-bit (e.g., Windows MSVC or ARM),
some precision is lost.

**`x80_t`**: the Mac on-disk/in-guest layout for an 80-bit extended value: a 16-bit
`sgn_and_exp` field (sign in bit 15, 15-bit biased exponent in bits 0–14) followed by
a 64-bit mantissa. The `GET_X80_SGN`, `SET_X80_SGN`, `GET_X80_EXP`, `SET_X80_EXP`
macros manipulate the sign and exponent fields.

**`macfpstate`**: `LowMemGlobal<Byte[6]>` at address `0xA4A`. The SANE library stores
its floating-point status word (exception flags, rounding mode, precision mode) there.
Executor reads and writes this location using `LM(macfpstate)`.

**Opcode dispatch**: SANE operations on 68K Macs were encoded as 16-bit words following
the `_FP68K` trap word. The low 6 bits of the following word are the SANE opcode
(masked by `OPCODE_MASK = 0x3F00` after shifting). `float4.cpp`, `float5.cpp`, and
`float7.cpp` implement the opcodes for the three SANE precision classes (single,
double, and extended/comp).

**`floatnext.cpp`**: the `FP68K` dispatcher. It reads the opcode word from the 68K
instruction stream, decodes the source and destination operand types from the high bits,
and dispatches to the appropriate C++ helper.

**Decimal conversion** (`floatconv.h`, `float_fcw.h`): `SANE_strtod` and `SANE_dtostr`
implement decimal ↔ binary conversion for `NumToString`, `StringToNum`, and related
traps.

## Source Files

| Path | Description |
|------|-------------|
| `src/sane/float.h` | `ieee_t`, `x80_t` macros, `macfpstate` LM global |
| `src/sane/floatnext.cpp` | `_FP68K` dispatcher, opcode decode |
| `src/sane/float4.cpp` | Single-precision (32-bit) SANE opcodes |
| `src/sane/float5.cpp` | Double-precision (64-bit) SANE opcodes |
| `src/sane/float7.cpp` | Extended / Comp-precision (80/64-bit) SANE opcodes |
| `src/sane/floatconv.h` | Decimal ↔ binary conversion helpers |
| `src/sane/float_fcw.h` | FPU control word manipulation macros |

## Important Data Structures

- **`x80_t`** (guest struct): 10-byte 80-bit extended: `sgn_and_exp` (16-bit) +
  `mantissa` (64-bit). Must be read/written through the `GET/SET_X80_*` macros; the
  two fields are not naturally aligned and the Mac format differs from the x87 layout
  in bit ordering.
- **`SANE status word`** (6 bytes at `macfpstate`): exception flags (invalid, overflow,
  underflow, divide-by-zero, inexact) + rounding mode + precision mode.

## Key Functions / Traps

| Symbol | Trap | Description |
|--------|------|-------------|
| `C_FP68K` | `_FP68K` | Main SANE dispatcher; all SANE operations go through here |
| `C_Unimplemented` (fallback) | `_FPFP88` | 68882 FPU trap (stub) |
| internal `fadd`, `fsub`, `fmul`, `fdiv` | — | Per-precision arithmetic helpers |
| internal `fsin`, `fcos`, `ftan`, `fln`, `fexp` | — | Transcendental functions |

## Design Notes / Gotchas

- **`long double` precision**: on x86-64 Linux/macOS, `long double` is 80-bit, giving
  full SANE fidelity. On 64-bit ARM (Apple Silicon) `long double` is 128-bit
  (quad precision), which is wider than needed but usually produces correct results.
  On Windows (MSVC) `long double` is 64-bit, causing a precision loss that may break
  numeric-sensitive applications.
- **`OPCODE_MASK`**: the low 6 bits of the word *following* the `_FP68K` trap encode
  the opcode; the upper 10 bits encode the source and destination formats. The mask
  `0x3F00` isolates the format bits after shifting.
- **`macfpstate`**: this low-memory global must be zeroed at application startup
  (`launch.cpp`). Stale exception flags from a previous run can cause unexpected SANE
  errors in applications that check the status word.
- SANE's `Comp` type is a 64-bit integer stored in an `extended`-sized slot; it is not
  a floating-point value. Operations on Comp operands in `float7.cpp` must cast to
  `int64_t` before any arithmetic.
