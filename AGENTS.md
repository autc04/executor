# AGENTS.md

Executor 2000 is a classic Mac OS emulator (68K and PowerPC). It needs no ROM
and no Apple software: every Mac OS trap is re-implemented in C++, in the spirit
of WINE. 68K code is dynamically recompiled by syn68k; PPC code is interpreted by
PowerCore. Emulated windows can appear rootless on the host desktop.

Deeper background is in `docs/architecture.md` (high-level overview) and
`docs/subsystems/*.md` (one file per subsystem). Read the relevant one before
changing a subsystem.

## Toolchain: Nix vs. plain environment

Two setups are supported, and the build/test commands differ slightly.

- **Nix dev shell (flake + direnv).** The repo ships `flake.nix`. If a dev shell
  is available, use it: run commands as `direnv exec . <command>` (or from inside
  `nix develop`). It provides CMake, a C++17 compiler, Qt 6, Boost, bison, perl,
  ruby, ninja, and the optional front-end libraries. Prefer it unless told
  otherwise.
- **Plain environment.** A normal C/C++ toolchain plus CMake is enough. Do not
  install Nix just to work on Executor. You need CMake ≥ 3.12, a C++17 compiler,
  Qt 6, Boost, bison, perl and ruby. SDL2/SDL, Wayland (waylandpp) and X11 are
  optional, for the extra front-ends; readline is optional, for the cxmon
  debugger.

## Build and test

Check out the submodules first (there are eight: syn68k, PowerCore, cxmon, lmdb,
lmdbxx, cmrc, multiversal, tests/googletest):

```
git submodule update --init --recursive
```

Configure and build. In-source builds are rejected by CMake, so always use a
separate build directory:

```
# plain environment
cmake -B build
cmake --build build -j8

# Nix dev shell
direnv exec . cmake -B build
direnv exec . cmake --build build -j8
```

Run the native test suite from the top-level build directory:

```
# plain environment
ctest --test-dir build

# Nix dev shell
direnv exec . ctest --test-dir build
```

A few `FileTest` cases are expected to fail: they are labelled `xfail` and are
not regressions (CI runs `ctest -LE xfail`).

The Mac-application test suite is separate. `tests/` cross-compiles Mac binaries
with [Retro68](https://github.com/autc04/Retro68) into `tests/build/`, which is a
*m68k cross-build directory of its own* — it has no CTest tests, so `ctest` must
be run against the top-level `build/`, not `tests/build/`. With Retro68 on
`PATH` (or configured via `LaunchAPPL`):

```
cmake --build tests/build
build/executor tests/build/tests.ad   # or executor-wayland, …
cat tests/build/out                   # test results
```

The VS Code tasks `test-executor`, `test-minivmac`, `test-geist` and
`test-serial` wrap these steps. The native CTest tests (host-side unit tests plus
the compile-fail tests in `tests/guestvalues.compfail.cpp`, which assert that
mixing guest and host types is a compile error) do **not** need Retro68.

### Build options

- `-DFRONT_ENDS="qt x sdl sdl2 wayland"` — which front-ends to build. The first
  one built is renamed to plain `executor` (on macOS it is named
  `Executor 2000`); the others are `executor-<name>`, e.g. `executor-wayland`.
  `-DFRONT_ENDS=headless` builds only the headless front-end.
- `-DTWENTYFOUR=YES` — 24-bit addressing (≈4 MB RAM, no PPC support).
- `-DNO_STATIC_BOOST=ON` — force dynamic Boost linkage.

## Running

```
build/executor path/to/disk-image.dsk
```

The application is a **positional argument after the options**; `--help` lists
them. Useful options: `--logtraps`, `--debug`,
`--debug-cmd`, `--break`, `--keyboards`, `--headless`.

- The GUI front-ends need a real window server, so a GUI run blocks and cannot
  work inside a sandbox that blocks one. Run those outside such a sandbox.
- For automated or unattended runs, pass `--headless` and bound the run time.

The built-in resources (fake `System` file, `Browser`, …) are compiled into the
binary and unpacked to `~/.executor/` on first run, so no separate installation
is needed.

## Docs

- `docs/subsystems/*.md` — per-subsystem architecture (file manager, trap
  dispatch, CPU, debugger, …). Read before changing a subsystem.
- `docs/architecture.md` — high-level architecture overview.
- `docs/ai/YYYY-MM-DD-*.md` — dated investigation notes and design documents
  (create the directory if it is missing). Add one when you work out something
  non-obvious, and update existing ones when they go stale.

## Skills

- `executor-guest-debugging` — debugging and tracing emulated guest code
  (`--logtraps`, `--debug`, the cxmon debugger, startup debug commands).
- `executor-file-manager` — File Manager diagnosis, especially HFS-image vs
  LocalVolume differences.

## Key concepts

### Guest memory and values

The guest (Mac) address space is a flat, big-endian byte array; the host is
usually little-endian.

- Use `GUEST<T>` for any value that lives in guest memory — never a plain
  pointer or plain integral type. Mixing guest and host types is deliberately a
  compile error.
- Structs that are part of the Mac ABI use the `GUEST_STRUCT` macro, which wraps
  every member in `GUEST<>`.
- Low-memory globals are accessed only via `LM(Name)` (`LM(ROMBase)`,
  `LM(TheZone)`, …); never hardcode their addresses.
- Address translation: `SYN68K_TO_US(addr)` / `US_TO_SYN68K(ptr)`.

### Trap dispatch

Guest code calls Mac APIs through 68K A-line traps. `alinehandler`
(`src/base/dispatcher.cpp`) decodes the trap number and dispatches through
`tooltraptable`/`ostraptable`, which `traps::init()` fills at startup by calling
`DeferredInit::initAll()` so that every `GeneratedEntrypoint` registers itself. Do not
manipulate the trap tables at runtime.

Trap declarations are **generated**, not hand-written. `multiversal/defs/*.yaml`
is turned by `multiversal/make-multiverse.rb` into the headers under
`build/src/api/` (`MemoryMgr.h`, `FileMgr.h`, …), the wrapper/marshaller sources
under `build/src/trap_entries/`, and the struct descriptions under
`build/src/structdump/`. Do not hand-edit generated headers — edit the YAML
definitions instead. Every trap is a generated `GeneratedEntrypoint`; there is no module
instanciation macro (the old `MODULE_NAME` / `api-module.h` / `INSTANTIATE_TRAPS_<name>`
mechanism is gone). Implementation functions are named `C_TrapName`; raw 68K handlers
are `RAW_TrapName` with `RAW_68K_IMPLEMENTATION(Name)`, declared in the YAML with
`executor_raw: true`.

Calling conventions (`callconv`, `src/base/functions.h`): `Pascal` (most Toolbox
traps), `Register<...>` (most OS traps), and `CCall`. Pick the convention of the
original Mac API. `Raw` is only a legacy tag; raw handlers are declared with
`executor_raw: true` and run through `invokeFromRaw68K`.

The marshalling itself is generated. For every signature in the set (generated
traps, `funptr:` types, and `executor_marshaling: true` declarations)
multiversal emits `invokeFrom68K`, `invokeFromPPC`, `invokeUPP` and
`createCallbackUPP`; generated wrapper constructors and `*_FUNCTION_PTR` objects
use those. Do not reintroduce the old `callfrom68K`/`callto68K`/
`callfromPPC::Invoker` templates. `invokeFrom68K`'s `const CC*` parameter is a
witness (always `nullptr`): two conventions can share one signature
(`DisposeHandle`), so the convention must be part of the parameter list.

## Coding conventions

- Namespace `Executor` for all emulator code; no new global-scope code.
- Allman brace style, 4 spaces, no tabs. Omit braces only for single-line
  `if`/`else`/`for` bodies.
- C++17 — `if constexpr`, structured bindings, `std::optional`, etc.
- `src/.clang-format` is authoritative; run `clang-format` before committing.
- Mac types come from `<ExMacTypes.h>` and the generated headers under
  `build/src/api/`; never hand-edit generated files.

## Gotchas

- **No ROM.** If you add an API, implement it from scratch; never assume a ROM
  image.
- **Guest/host mixing** is the most common source of byte-order bugs. A
  `GuestWrapper` type error means you are mixing forbidden types.
- **cxmon is GPL v2+**, so a binary built with it is effectively GPL-licensed.
  There is currently **no configure-time option** to exclude it —
  `add_subdirectory(cxmon)` is unconditional — so a non-copyleft build requires
  editing the build.
- **24-bit addressing** (`-DTWENTYFOUR=YES`) is off by default and changes memory
  assumptions; consult `src/mman/` before writing code that assumes either mode.
- **Submodules are required.** CMake aborts if `syn68k/` is missing.
