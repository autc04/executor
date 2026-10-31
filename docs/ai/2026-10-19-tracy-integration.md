# Tracy profiler integration

> **AI-generated.** This document was produced with the assistance of an AI language model and may contain inaccuracies.

## Building

Tracy support is a build-time option, off by default. The client is compiled
from source; `TRACY_SOURCE_DIR` must point at a Tracy checkout (the directory
containing `public/TracyClient.cpp`):

```
cmake -B build -DENABLE_TRACY=ON -DTRACY_SOURCE_DIR=/path/to/tracy
cmake --build build -j8
```

The Nix dev shell provides the profiler and capture tools and exports
`TRACY_SOURCE_DIR`:

```
nix develop
cmake -B build -DENABLE_TRACY=ON
```

## Runtime

There is no runtime switch. A Tracy-enabled build always starts the client at
startup, including during emulator initialization; attach the Tracy profiler
when you want a trace. `TRACY_ON_DEMAND` is intentionally not defined so that
startup can be profiled.

Tracy's worker threads are started from a static initializer with
`init_priority(105)`, i.e. before ordinary static initializers run. Any state a
thread inherits (notably the signal mask) must therefore be set up earlier;
see `2026-10-31-sigalrm-shutdown-crash.md` for the SIGALRM example.

## What is instrumented

- `GeneratedEntrypoint::handle68K` wraps every 68K trap dispatch (including
  raw traps) in a `ZoneTransientN` named after `Entrypoint::name`.
- The PPC entry lambda in `GeneratedEntrypoint::init` does the same for PPC
  traps.
- `GeneratedDispatcherTrap::invokeFrom68K` names the dispatcher and the
  selected sub-trap.
- `VideoDriver::updateBuffer` has a single `ZoneScoped`.
- Thread names: `emulator` for the executor thread, `vdriver-worker-N` for the
  video worker pool, and `timer` for the syncint timer thread (where that
  thread exists).

No generated code is changed: all trap zones go through the existing
`GeneratedEntrypoint` funnels.

## Source layout

`src/base/profiling.h` is the only file that includes `<tracy/Tracy.hpp>`.
When `EXECUTOR_TRACY` is off it defines `ZoneScoped` and `ZoneTransientN` as
empty macros and `tracy::SetThreadName` as a no-op. Using a different Tracy
macro in a non-Tracy build therefore fails at the use site, which is the
intended signal to add a stub.

## Why the client is compiled from source

The nixpkgs `tracy` package ships a prebuilt shared `libTracyClient.so`, but it
is built with Tracy's defaults. In particular it installs Tracy's crash handler
(`tracy::Profiler::Worker()` calls `InstallCrashHandler()` and imports
`sigaction`), which conflicts with Executor's own `SIGSEGV`/`SIGBUS` handler in
`src/base/fault_handler.cpp`. Compiling `public/TracyClient.cpp` ourselves lets
us define `TRACY_NO_CRASH_HANDLER` (and `TRACY_NO_BROADCAST` /
`TRACY_ONLY_LOCALHOST`), and avoids the packaged library's `libtbb` runtime
dependency.

## Runtime names

`ZoneScoped`/`ZoneScopedN` cache their source location in a function-local
`static` and use a literal name. The generated `invokeFrom68K` is shared by many
traps and receives the trap name in `Entrypoint::name`, so the trap zones use
`ZoneTransientN(zone, name, true)`, which copies a runtime name. The name is
looked up by `strlen`, so it must be a normal C string (`Entrypoint::name` is).
