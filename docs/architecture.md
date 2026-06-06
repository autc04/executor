# Executor 2000 — Architecture Overview

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

This document is intended for C++ developers who want to contribute to Executor 2000. It explains how the major components fit together, not merely what they are.

---

## 1. What Executor Is

Executor 2000 is a classic Mac OS emulator targeting the 68K/early-PPC era (roughly 1984–1995). Its defining property is that it requires **no ROM file** and no original Apple software: every Mac OS API call is handled by a C++ implementation inside Executor itself. This philosophy is identical to WINE for Windows.

The project originated commercially at ARDI in the 1990s, was open-sourced in 2008, and has been modernised ("2000") since 2019. It currently builds and runs on 64-bit Linux, macOS, and Windows, and includes rootless windowing (emulated windows appear on the host desktop).

---

## 2. High-Level Architecture

```
┌──────────────────────────────────────────────────────────┐
│                   Mac Application (.ad / .dsk)           │
└───────────────────────────┬──────────────────────────────┘
                            │ A-line trap instruction
┌───────────────────────────▼──────────────────────────────┐
│                    Trap Dispatch Layer                    │
│  alinehandler → trap table → TrapFunction<> / DispatcherTrap │
└────────────┬──────────────────────────────┬──────────────┘
             │ calls                        │ calls
┌────────────▼──────────────┐  ┌────────────▼──────────────┐
│   Mac API Implementations │  │   Low-Memory Globals      │
│   (src/quickdraw, mman,   │  │   LM(TheZone), LM(ROMBase)│
│    wind, textedit, …)     │  │   (guest address space)   │
└────────────┬──────────────┘  └───────────────────────────┘
             │ reads/writes
┌────────────▼──────────────┐
│   Guest Address Space     │
│   (flat byte array,       │
│    big-endian 68K layout) │
└────────────┬──────────────┘
             │ executed by
┌────────────▼──────────────────────────────┐
│   CPU Emulation                           │
│   syn68k (68K dynamic recompiler)         │
│   PowerCore (PPC interpreter)             │
└────────────┬──────────────────────────────┘
             │ I/O delivered through
┌────────────▼──────────────────────────────┐
│   Platform Front-End (VideoDriver)        │
│   Qt / SDL2 / Wayland / X11 / Win32 /     │
│   headless                                │
└───────────────────────────────────────────┘
```

---

## 3. Guest vs Host: The Fundamental Duality

The Mac ran on big-endian 68K hardware. Executor runs on little-endian x86-64. Every value that lives in the emulated Mac's address space is therefore stored in big-endian byte order.

### The guest address space

The guest address space is a contiguous region of host memory allocated at startup. The pointer `ROMlib_offset` is the host base address of that region. A 32-bit guest address `addr` translates to host pointer `ROMlib_offset + addr`. The macros in `syn68k_public.h` perform this translation:

```cpp
SYN68K_TO_US(addr)   // guest address → host pointer
US_TO_SYN68K(ptr)    // host pointer → guest address
```

The `_CHECK0` and `_CHECK0_CHECKNEG1` variants handle null (0) and `-1` sentinel values that several Mac APIs use.

### `GUEST<T>`

Any value that resides in guest memory must be wrapped in `GUEST<T>` (`src/base/mactype.h`). The wrapper transparently byte-swaps on read and write via `SwapTyped`:

```cpp
GUEST<int16_t> x;   // stored big-endian in the guest address space
x = 42;             // writes 0x2A 0x00
int16_t v = x;      // reads and swaps back → 42 on the host
```

Pointer members inside guest-resident structs store the guest 32-bit address (not a host pointer), and `GuestTypeTraits<T*>` handles the translation automatically.

Any struct that may be passed by pointer to guest code must be declared with `GUEST_STRUCT` and wrap all fields in `GUEST<>`. Forgetting this produces silent data corruption on little-endian hosts.

The type system deliberately makes mixing `GUEST<T>` with plain `T` in arithmetic a compile error. Compile-fail tests in `tests/guestvalues.compfail.cpp` verify this.

### Low-memory globals

Mac OS stored many OS-level variables at fixed addresses in the first 64 KB of the address space (e.g., `TheZone` at 0x0118, `ROMBase` at 0x02AE). Access these exclusively through `LM()` (`src/base/lowglobals.h`):

```cpp
LM(TheZone)   // returns GUEST<THz>&
LM(ROMBase)   // returns GUEST<uint32_t>&
```

`LM()` simply computes `*(GUEST<T>*)(ROMlib_offset + lm.address)`. Never hardcode raw addresses.

---

## 4. CPU Emulation Layer

### syn68k — 68K dynamic recompiler

`syn68k/` is a git submodule containing the 68K CPU emulator written by Mat Hostetter at ARDI. It dynamically recompiles 68K basic blocks into native host code, caching the results. The public interface is in `syn68k_public.h`.

`execute68K(addr)` in `src/base/cpu.cpp` sets `currentCPUMode = CPUMode::m68k` and calls `CALL_EMULATOR(addr)`. When a compiled 68K block executes an A-line trap instruction, syn68k calls `alinehandler` (registered at startup via `trap_install_handler(0xA, …)`).

`ROMlib_destroy_blocks()` invalidates the syn68k translation cache for a range of guest addresses, required whenever guest code is modified (e.g., after `BlockMove` into an executable area).

### PowerCore — PPC interpreter

`PowerCore/` is a git submodule containing a PowerPC interpreter. `execute68K` and `executePPC` (`src/base/cpu.h`) switch `currentCPUMode` accordingly. `executePPC` sets the CIA (current instruction address) and `lr = 0xFFFFFFFC` (a sentinel return address that terminates the interpreter) and calls `cpu.execute()`.

`CPUMode currentCPUMode` tracks which CPU is currently running, which matters for Mixed Mode callbacks (see §10).

---

## 5. Trap Dispatch

### The A-line mechanism

The 68K instruction set reserves A-line opcodes (`0xAxxx`) for OS traps. When syn68k encounters an A-line opcode it calls `alinehandler(pc, …)` (`src/base/dispatcher.h` / `.cpp`). The handler decodes the trap number from the opcode, looks it up in `tooltraptable` or `ostraptable`, and transfers control to the corresponding stub.

Trap tables are filled at startup by `traps::init()` (`src/base/traps.cpp`), which calls `DeferredInit::initAll()` to let every `TrapFunction<>` object register itself.

### `Entrypoint` and `TrapFunction<>`

Every Mac API entry point is represented by a statically-allocated `TrapFunction<>` object (`src/base/traps.h`). It inherits from `Entrypoint`, which inherits from `DeferredInit`. The `DeferredInit` linked list guarantees that every trap installs itself before the emulator starts, without requiring manual registration calls.

A simplified view of the class hierarchy:

```
DeferredInit
  └── Entrypoint           (name, breakpoint support, debugger hook)
        └── WrappedFunction<F*, CallConv>   (owns the 68K stub)
              └── TrapFunction<F*, trapno, CallConv>   (owns trap table slot)
              └── SubTrapFunction<F*, trapno, selector, CallConv>
```

`WrappedFunction::init()` synthesises a small 68K thunk ("stub") in guest memory that, when called by guest code, marshals arguments according to `CallConv`, calls the C++ implementation, and returns.

`TrapFunction::operator()` checks whether the trap has been patched (via `GetTrapAddress`/`SetTrapAddress`) and, if so, calls through the trap table; otherwise it calls the C++ function directly. This is how Mac applications can patch OS traps — a common technique.

### Calling conventions

The `callconv` namespace (`src/base/functions.h`) models the three conventions used by the original Mac OS:

| Tag | Description |
|-----|-------------|
| `callconv::Pascal` | Parameters pushed right-to-left on the 68K stack; return value on stack |
| `callconv::Register<Ret(Args...), locs...>` | Parameters and return value in specified 68K registers (`A0`–`A7`, `D0`–`D7`) |
| `callconv::CCall` | Standard C calling convention |
| `callconv::Raw` | Direct `syn68k_addr_t(syn68k_addr_t, void*)` handler — use only when you must access CPU state directly |

### Selector-dispatched traps

Some Mac traps (e.g., `_OSDispatch`, file manager traps) multiplex many functions under a single trap number, dispatching on a selector value. These use `DispatcherTrap<SelectorConvention>`, which reads the selector from D0, D1, a stack word, or the trap bits, then forwards to the matching `SubTrapFunction<>`.

### Declaration macros

Traps are declared in module headers using macros (`src/base/traps.h`):

```cpp
PASCAL_TRAP(GetHandle, 0xA9A0)
REGISTER_TRAP(DisposeHandle, 0xA023, D0 (A0))
PASCAL_SUBTRAP(HLock, 0xA029, 0x0020, "MemoryMgr")
```

These macros expand differently depending on whether `INSTANTIATE_TRAPS_<MODULE_NAME>` is defined in the current translation unit (see §6).

---

## 6. Mac API Implementations

### Module pattern

Each subsystem follows the same convention (`src/base/api-module.h`):

1. The **header** defines `MODULE_NAME` then `#include`s `<base/api-module.h>`. This puts the `TrapFunction<>` objects in `extern` declarations and expands trap macros as `extern` variable declarations.

2. The **`.cpp`** defines `INSTANTIATE_TRAPS_<MODULE_NAME> 1` before including the header. `api-module.h` switches to `DEFINE` mode, causing the macros to produce actual variable definitions with explicit template instantiations. This makes every trap self-registering with zero manual glue code.

Implementation functions are named `C_TrapName` by convention:

```cpp
// In mman.h (header):
#define MODULE_NAME base_mman
#include <base/api-module.h>
PASCAL_TRAP(NewHandle, 0xA122)

// In mman.cpp (implementation):
#define INSTANTIATE_TRAPS_base_mman 1
#include <mman/mman.h>

Handle Executor::C_NewHandle(Size logicalSize)
{
    // ... implementation ...
}
```

### Subsystems

The major Mac managers are implemented across `src/`:

| Directory | Mac Manager |
|-----------|-------------|
| `quickdraw/` | QuickDraw (2D drawing) |
| `mman/` | Memory Manager |
| `res/` | Resource Manager |
| `wind/` | Window Manager |
| `menu/` | Menu Manager |
| `ctl/` | Control Manager |
| `dial/` | Dialog Manager |
| `textedit/` | TextEdit |
| `osevent/`, `appleevent/` | Event Manager, Apple Events |
| `sound/` | Sound Manager |
| `sane/` | SANE floating-point |
| `hfs/` | HFS / HFS+ disk images |
| `file/` | File Manager |

---

## 7. Memory Layout

The guest address space is a single flat allocation on the host. From low to high:

```
0x0000 – 0x03FF   Low-memory globals (68K trap vectors, OS state)
0x0400 – …        System Zone (SysZone) — OS heap
 …                Application Stack (grows downward from StackBase)
 …                Application Zone (ApplZone) — application heap
 …                Temporary Memory region
```

Sizes are configurable:

| Zone | Default | Bounds |
|------|---------|--------|
| SysZone | 512 KB | 128 KB – 2047 MB |
| AppZone | 64 MB (32-bit) / 3 MB (24-bit) | 512 KB – 2046 MB |
| Stack | 256 KB | 64 KB – 2047 MB |

The 24-bit addressing mode (`-DTWENTYFOUR=YES`) constrains addresses to 24 bits, mimicking early Mac hardware where the high byte of a pointer carried flags rather than address bits. In this mode the address space is limited to 16 MB and the default application zone shrinks accordingly.

`ROMlib_syszone` and `ROMlib_memtop` are host-space boundaries used to validate pointers before dereferencing, preventing spurious crashes from bogus pointer values passed by guest code.

---

## 8. Video & Platform Front-Ends

### `VideoDriver` abstraction

All display and input code lives under `src/config/front-ends/`. Each front-end subclasses `VideoDriver` (`src/vdriver/vdriver.h`), which owns a `Framebuffer`:

```cpp
struct Framebuffer
{
    std::shared_ptr<uint8_t> data;
    int width, height, bpp, rowBytes;
    bool rootless;
    // ...
};
```

The core emulator writes pixels directly into `Framebuffer::data` (which maps into the guest address space for `screenBits.baseAddr`). When QuickDraw modifies screen pixels it marks dirty rectangles; `VideoDriver::updateScreen(top, left, bottom, right)` blits the dirty region to the host window.

### Event delivery

Events flow inward through `IEventListener` / `EventSink`. The front-end calls methods such as `mouseMoved`, `mouseButtonEvent`, and `keyboardEvent`; `EventSink` marshals them onto the emulator thread and posts them to the Mac event queue via the standard OS event mechanisms.

`EventSink::pumpEvents()` is called by the emulator's event loop to drain the pending event queue.

### Available front-ends

| Directory | Front-end |
|-----------|-----------|
| `qt/` | Qt 5/6 (default) |
| `sdl2/` | SDL 2 |
| `sdl/` | SDL 1.2 |
| `wayland/` | Native Wayland (via waylandpp) |
| `x/` | X11 |
| `win32/` | Win32 |
| `headless/` | Headless (for testing) |

The build system selects the default front-end based on available libraries; the binary is named `executor`, `executor-wayland`, `executor-sdl2`, etc.

`VideoDriver` also provides clipboard integration via `putScrap` / `getScrap`, and cursor management (`setCursor`, `hideCursor`).

---

## 9. File System Emulation

### HFS disk images

`src/hfs/` implements the HFS (Hierarchical File System) volume format. Executor can mount `.dsk` or `.img` files as Mac volumes, presenting them to guest applications as normal HFS volumes via the standard File Manager interface.

### LocalVolume — host directory mapping

`src/file/localvolume/` maps a host directory as a Mac volume. This lets the guest read and write files on the host file system. Because Mac file names and CNID (catalog node ID) numbers must be stable across sessions, a persistent LMDB database (`LMDBCNIDMapper`) records the mapping between host paths and Mac CNIDs. This avoids re-scanning the directory tree on every launch.

### File Manager

`src/file/` implements the Mac File Manager traps (`PBOpenSync`, `PBReadSync`, `PBWriteSync`, `PBGetCatInfo`, etc.). It dispatches to either the HFS backend (for mounted disk images) or LocalVolume (for host-directory mounts).

---

## 10. PPC/CFM Support and Mixed Mode

### Code Fragment Manager (CFM)

`src/cfm.cpp` implements the Code Fragment Manager, which loads PowerPC applications stored in PEF (Preferred Executable Format) binaries. It parses the PEF container, allocates sections in the guest address space, resolves imports against Executor's built-in library exports, and sets up the `RoutineDescriptor` table.

### Mixed Mode

`src/mixed_mode.cpp` implements the Mixed Mode Manager, which allows 68K code to call PPC routines and vice versa. A `RoutineDescriptor` struct in guest memory holds a pointer to either a 68K or PPC routine along with an `ISA` (Instruction Set Architecture) flag.

`CallUniversalProc` inspects the ISA: if it matches `currentCPUMode`, control flows directly; otherwise it switches CPU mode (68K → PPC or PPC → 68K) and resumes execution. The synthetic trap `0xAAFE` (`modeswitch`) is used as the re-entry point after a mode switch returns.

---

## 11. Debugger — cxmon Integration

`cxmon/` is a git submodule containing a command-line monitor/debugger originally from the Basilisk II emulator, licensed under GPL v2+. This makes the combined binary GPL-licensed. Excluding cxmon from the build produces a non-copyleft binary.

The debugger (`src/debug/mon_debugger.cpp`) implements the `base::Debugger` interface. It is invoked when:
- a trap's `breakpoint` flag is set (via `Entrypoint::breakpoint`),
- the guest executes an illegal instruction, or
- the user presses the debugger hotkey.

cxmon provides a 68K disassembler, memory examination commands, breakpoint management, and support for Mac OS data structures. It runs on the emulator thread when a break occurs, blocking guest execution.

---

## 12. Build System

### CMake structure

The top-level `CMakeLists.txt` enforces out-of-source builds (in-source builds abort with a fatal error). Key targets:

| Target | Contents |
|--------|----------|
| `syn68k` | 68K recompiler (submodule) |
| `PowerCore` | PPC interpreter (submodule) |
| `cxmon` | Debugger (submodule) |
| `lmdb` | LMDB storage engine (submodule) |
| `resources` | CMakeRC resource bundle |
| `executor` (and variants) | Main emulator binary |

`src/CMakeLists.txt` builds the emulator core and selects front-end sources based on `find_package` results for Qt, SDL2, SDL, Wayland, and X11.

### Resource embedding — cmrc

System resources (the built-in `System` file, `Browser`, `Printer`, and related files in `res/`) are compiled directly into the executable via `cmrc_add_resource_library`. This means no external resource files need to be installed; the binary is self-contained.

### Code generation

Several parts of the build run scripts before compilation:

- `multiversal/`: a Ruby script processes Apple's Universal Headers (included as-is) to generate the `<ExMacTypes.h>` and related headers used throughout the source. Do not hand-edit these files.
- Bison generates parser code used in the command-line option handling.
- Perl scripts assist with trap name tables.

### Submodule initialisation

All four submodules (`syn68k`, `PowerCore`, `cxmon`, `lmdb`) must be initialised before CMake will succeed:

```bash
git submodule update --init --recursive
```

CMake checks for `syn68k/CMakeLists.txt` and aborts with a clear message if missing.

### Cross-compilation for tests — Retro68

The test suite (`tests/`) compiles Mac application binaries using the [Retro68](https://github.com/autc04/Retro68) cross-compiler toolchain. These binaries run either under Executor itself or on real hardware via `LaunchAPPL`. Native CTest host-side tests (compile-fail tests and unit tests) do not require Retro68.

The VS Code task `test-executor` builds both the emulator and the test binary, runs the test under Executor, and prints the results from `tests/build/out`.

### Optional build flags

| Flag | Effect |
|------|--------|
| `-DEXECUTOR_ENABLE_LOGGING=ON` | Enables the `-logtraps` runtime option for trap call logging |
| `-DTWENTYFOUR=YES` | Compile-time 24-bit addressing mode |
| `-DNO_STATIC_BOOST=ON` | Force dynamic Boost linkage |
