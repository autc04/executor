---
name: executor-guest-debugging
description: Use when debugging or tracing 68K/PPC guest code running under Executor (the classic-Mac emulator) — a Mac application misbehaving, a trap returning an unexpected error, or needing to observe guest instructions, registers, or memory. Covers --logtraps, --debug, and the command-driven cxmon debugger (--break, --debug-cmd, EXECUTOR_DBG_INIT).
---

# Debugging guest code in Executor

These tools observe the emulated (guest) side. Build and run basics are in
`AGENTS.md`; File Manager specifics are in the `executor-file-manager` skill.

## Pick the tool

| Question | Tool |
|---|---|
| In what order did traps run, and with what refnums/args? | `--logtraps` |
| Which trap returned error *N*? | `--debug trapfailure` |
| Registers, memory, or disassembly at a stop | the debugger (`r`, `m`, `d68`) |
| What the guest does after a given trap | trap breakpoint (`atb`) + `steps` |
| What happens at an arbitrary app code address | code breakpoint (`ba $addr`) |

## Trap logging

`--logtraps` prints every OS/toolbox call and return. It requires a build with
`-DEXECUTOR_ENABLE_LOGGING=ON` and is very verbose. Caveats: pointer arguments
are dereferenced one level, so **parameter-block arguments print as `=> ?`** and
`ConstStringPtr` names print as `=> <length byte>`. For decoded fields, use the
debugger.

`--debug <list>` enables targeted diagnostics (comma-separated; `all` for
everything). Useful categories: `trapfailure` (every file trap returning
≠ `noErr`, printed as `file:LINE`), `unimplemented`, `unexpected`, `trace`,
`fslog`, `memcheck`, `textcheck`.

## The debugger

`--break` breaks into the debugger at process start; `Ctrl-C` interrupts it. It
is configured with commands supplied at startup, either with `--debug-cmd
"<command>"` (repeatable) or in `EXECUTOR_DBG_INIT="<command>; <command>; ..."`.
The commands are queued and run once when the first process is initialized
(before its first instruction), so both trap and address breakpoints are armed
in time.

| Command | Meaning |
|---|---|
| `atb "name"` / `atc "name"` | arm / clear a trap-entrypoint breakpoint |
| `ba $addr` / `br` / `bi` | add / remove / list code breakpoints (addresses use cxmon syntax: `$hex`, `_dec`) |
| `on * "cmd"` | run `cmd` at every stop (repeatable) |
| `skip * n` | ignore the first `n` stops |
| `limit * n` | process at most `n` stops, then keep going |
| `steps * n` | single-step (and disassemble) `n` instructions per stop |
| `r` / `d68` / `m` | registers / disassemble / hex dump |
| `o "file"` | redirect debugger output |
| `s` / `x` | single-step / resume |
| `es` | ExitToShell |

Trap entrypoint names are the trap wrapper names (e.g. `PBGetFInfo/PBHGetFInfo`,
`PBHOpen`); the same names appear in `--logtraps` output.

At a stop, `x` means *resume*. So `on * "x"` resumes at every stop — that is the
unattended/batch mode (there is no separate flag). Without any
`on`/`skip`/`limit`/`steps` command, a stop drops into the interactive monitor,
which reads stdin.

Trace 120 instructions after the 8th `PBHGetFInfo` call, unattended:

```
EXECUTOR_DBG_INIT='atb "PBGetFInfo/PBHGetFInfo"; skip * 7; limit * 1; steps * 120; on * "x"' \
  ./build/Executor\ 2000.app/Contents/MacOS/Executor\ 2000 --headless "/path/to/App"
```

Dump registers at the first three calls:

```
--debug-cmd 'atb "PBGetFInfo/PBHGetFInfo"' --debug-cmd 'on * "r"' \
--debug-cmd 'limit * 3' --debug-cmd 'on * "x"'
```

Notes:

- The debugger's own output (`r`, `d68`) goes to stdout; the `steps` trace goes
  to stderr. Output is flushed after each command script, so it survives a
  killed process.
- The application to run is a positional argument, after the options. For
  automated runs always pass `--headless` and bound the run — a GUI run blocks
  waiting for the window server.
- `steps` follows branches, because it uses the emulator's own single-step.

## Adding instrumentation

Check whether an existing tool already answers the question (table above) before
adding prints. If you do add logging, extend the central facility rather than
scattering env-gated output: for example, add a `logValue` overload in
`src/base/logging.{h,cpp}` so `--logtraps` decodes a struct type (such as the
File Manager parameter blocks) instead of printing `?`. Keep any new
instrumentation opt-in, and remove it once the investigation is done.

## Docs

`docs/subsystems/debugger.md`, `trap-dispatch.md`, `cpu-68k.md`.
