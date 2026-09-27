# AGENTS.md

Executor is a classic Mac OS emulator (68K and PowerPC).

## Building and testing

The toolchain is normally provided by a Nix flake with direnv, but **this is
optional**. If `direnv` and `nix` are not installed on the machine, expect a
conventional environment (a normal C/C++ toolchain with CMake) and configure a
build directory yourself.

With the dev shell:

```
direnv exec . cmake --build build -j8
direnv exec . ctest --test-dir build
```

Without it, configure and build with CMake in the usual way (`cmake -B build`,
`cmake --build build`) and run `ctest` yourself.

Submodules (syn68k, PowerCore, cxmon, multiversal, …) must be checked out; a
fresh clone needs `git submodule update --init`.

A few `FileTest` cases are expected to fail (`xfail`) — they are not
regressions.

## Running

The application to run is a positional argument, after the options (`--help`
lists them).

- The GUI front-end needs a real window server, so it cannot run inside a
  sandbox that blocks that; run it outside such a sandbox when it must be used.
- For automated or unattended runs, pass `--headless` and bound the run time —
  a GUI run blocks waiting for the window server.

## Docs

- `docs/subsystems/*.md` — per-subsystem architecture (file manager, trap
  dispatch, CPU, debugger, and so on). Read the relevant one before changing a
  subsystem.
- `docs/ai/YYYY-MM-DD-*.md` — dated investigation notes. Add one when you work
  out something non-obvious, and update existing ones when they become stale.

## Skills

- `executor-guest-debugging` — debugging and tracing emulated guest code
  (`--logtraps`, `--debug`, the cxmon debugger, startup debug commands).
- `executor-file-manager` — File Manager diagnosis, especially HFS-image vs
  LocalVolume differences.
