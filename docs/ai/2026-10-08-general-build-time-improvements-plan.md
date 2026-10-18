# General build-time improvements

> Date: 2026-10-08
> Status: candidates only — no candidate has been researched beyond the
> measurements below, and none has been implemented.

## Purpose

The generated-trap-entrypoint work
(`docs/ai/2026-10-07-generated-trap-entrypoints-plan.md`) took the clean-build
wall time from 1m04.3s to ~47.5s.  That work is done.  This document lists
**general** further build-time improvements, i.e. ones that are not specific to
the trap machinery, with the measurements that motivate each one so the list can
be prioritised.

The raw leads gathered earlier live in
`docs/ai/2026-10-07-ordinary-tu-compile-time-notes.md`; this document turns them
into candidates with estimates.  That file stays the record of what was actually
observed at the time.

## Constraints

Same hard constraints as the trap work, because they are the user's, not the
previous plan's:

- **No build-configuration shortcuts.** No `-g0` (not on generated code, not
  anywhere), no headless-only or partial builds, no "don't compile/test that".
  A win must not come from dropping code, debug info, or front-ends.
- **Measure wall and user time, and binary size, for every candidate.**  The
  benchmark is `ninja clean && time ninja` in `build/` (Debug, all front-ends +
  tests), plus `stat`/`strip` on `build/executor`, `build/tests/tests` and
  `build/src/libromlib.a`, plus `ctest -LE xfail` (138/138) and the Retro68 Mac
  suite (90 passed + 2 known `xfail`s).
- A candidate that only helps *incremental* builds (caching, PCH invalidation
  behaviour) must be measured on `ninja clean && ninja`, not only on a warm
  tree, and its effect on incremental builds must be reported too.

## Where the time goes today

All measured 2026-10-08 on this machine (24 cores, Nix GCC 14.3.0), from a
`ninja clean && ninja` whose `.ninja_log` was then reduced to the outputs that
still exist.

| | value |
|---|---|
| wall | 47.5 s |
| user / sys | 10m36.2s / 2m50.0s |
| object files | 557 |
| **serial compile time** | **961 s** |
| ideal on 24 cores (961/24) | ~40 s |
| link steps (6 executables + `libromlib.a`) | ~13.5 s serial, ~2 s tail |

Per-category serial time:

| group | serial | notes |
|---|---|---|
| ordinary hand-written TUs | 582 s (61%) | the majority |
| generated `trap_entries/*.cpp` | 131 s | 66 TUs |
| generated `trap_instances/*.cpp` | 90 s | 66 nearly-empty TUs |
| generated `structdump/*.cpp` | 84 s | 66 TUs |
| per-front-end sources | 73 s | `main.cpp` ×5 + front-end dirs |

Translation-unit size distribution (557 `.o`):

| range | count |
|---|---|
| < 0.5 s | 69 |
| 0.5 – 1 s | 25 |
| 1 – 2 s | 310 |
| 2 – 4 s | 130 |
| ≥ 4 s | 23 |

Heaviest single TUs: `front-end-qt/qtkeycodes.cpp` 6.6 s; `main.cpp` ×5
(5.9–6.5 s each); `init.cpp` 6.1 s; `localvolume.cpp` 5.4 s; `prPrinting.cpp`
5.2 s; `trap_entries/FileMgr.cpp` 5.0 s.

Compile flags (Debug) are minimal: `-g -std=gnu++17`, 11 `-I`, 7 `-D`, no PCH,
no ccache/sccache, no `-fuse-ld` (the toolchain is configured
`--enable-default-pie`, so all objects are position-independent; `ld.gold` is
present but `mold`/`ld.lld` are not installed).

**The profile is front-end bound.**  `-ftime-report` on `init.cpp`
(`-fsyntax-only`): 4.14 s wall, of which 88% is `phase parsing`; within that,
`template instantiation` 28%, `name lookup` 19%, `preprocessing` 22%.  The full
compile is 6.1 s, so codegen is only ~2 s of it.  Reducing header/instantiation
work is therefore the highest-leverage lever for ordinary TUs.

Header fan-out:

- the 66 generated `api/*.h` total 24 301 lines and contain **2004 generated
  wrapper classes** (one per trap object) plus their `extern` declarations;
- **214** `src/*.cpp` translation units include at least one generated header;
- `init.cpp` includes **30** of them; `stdfile.cpp` 12.

## Candidates

Grouped by lever.  "Serial" is the expected reduction in the 961 s of serial
compile work; "wall" is a guess at the resulting wall-time change and is only
meaningful together with the measured candidate.

### A. Cut front-end work across many TUs

| # | idea | evidence | expected | effort | risk / trade-off |
|---|---|---|---|---|---|
| A1 | **Precompiled header** over the stable hand-written `base/*` headers, and possibly over the generated `api/*` headers | 88% of `init.cpp` is parsing; 214 TUs include generated headers | large serial (hundreds of s) if it works | medium | PCH must be rebuilt whenever any included header changes; the generated headers change on every YAML edit, so a PCH over them can make *incremental* builds worse. `api-module.h` is deliberately include-guard-free and re-included with a different `MODULE_NAME` per module header, which is exactly the construct a frozen PCH token stream is worst at — this needs a prototype before it can be trusted. |
| A2 | **Split the generated module headers** into "types + `C_` declarations" (what most hand-written code needs) and "trap objects" (wrapper classes, `extern` objects, which pull `base/trap-entry.h` → `traps.h` → `functions.h`).  Include the second only where trap objects are actually used. | 2004 wrapper classes; 214 TUs include generated headers but few use trap objects | medium serial | medium | Hand-written files that use `&Trap`/`operator&` (e.g. `init.cpp` storing `&stub_SwapMMUMode`) need the second header; getting the split wrong is a compile error, not a silent bug. |
| A3 | **Include hygiene / IWYU** on the widely-included hand-written headers (`base/functions.h`, `traps.h`, `logging.h`, `structdump.h`, `base/mactype.h`): drop transitive includes, forward-declare, move heavy bits behind narrower headers. | An IWYU report already exists locally; these headers are pulled in by every module header | medium serial | medium | Mechanical but broad; needs care with the generated `#include` order. |
| A4 | **Reduce template instantiation in `base/functions.h`** (`UPP`/`callconv` descriptors, the `logValueTo` overload set).  Small functions, but instantiated per use across 214 TUs. | template instantiation 28% of `init.cpp` parsing | medium serial | medium-high | This is the machinery hand-written `*_FUNCTION_PTR` sites depend on; changing it is what the trap work deliberately avoided. |
| A5 | **Forward declarations instead of includes** where only a pointer/reference is needed (type names in signatures). | general | small-medium | low | Low. |

### B. Remove duplicated and marginal work

| # | idea | evidence | expected | effort | risk / trade-off |
|---|---|---|---|---|---|
| B1 | **Compile `main.cpp` once, not five times.**  It is the same ~6 s TU built per front-end (qt/x/sdl/sdl2/wayland) | 5 × ~6 s ≈ 30 s serial, and each is a long pole | ~25–30 s serial; possibly 1–2 s wall | medium-high | Needs the front-end to be selected at link time (a small registration/callback seam) instead of by `#include`/`#ifdef`.  Risk of changing front-end selection semantics. |
| B2 | **Merge the tiny generated TUs.**  `structdump/*` (66 TUs, 84 s) and `trap_instances/*` (66 TUs, 90 s) are individually small; `structdump` in particular has no per-TU macro state and could be a single TU. | 132 small TUs | 20–50 s serial | medium | Fewer TUs = less parallelism; on 24 cores 961 s of work is 40 s ideal, so *removing* work matters more than merging, but merging also cuts fixed per-TU overhead (preprocessing of the same headers, CC1 startup).  `trap_instances/*` **cannot** simply be merged (each defines a different `INSTANTIATE_TRAPS_<Module>`), so only `structdump` and the entry files are candidates. |
| B3 | **`tests/guestvalues.compfail.cpp` ×4** (compile-fail tests) and **`x_keycodes.cpp` ×3**: fold the four expected failures into one `try_compile`-style check per header. | 4–7 extra small TUs | small | low-medium | The compile-fail tests are a real correctness net; keep them, just dedupe. |

### C. Tooling and caching

| # | idea | evidence | expected | effort | risk / trade-off |
|---|---|---|---|---|---|
| C1 | **ccache/sccache** in the dev shell (and CI) | none today | near-zero rebuild for unchanged code | low | Does not help a genuinely clean build (`nix build` with a cold cache), so it is a *developer-iteration* win, not a benchmark win.  Must be reported separately and not used to claim a wall-time improvement. |
| C2 | **Faster linker** (`mold` or `ld.lld`) | ~13.5 s serial link, ~2 s tail | < 1 s wall | low | Small; add to the dev shell only if it is free. |
| C3 | **IO/parallelism tuning**: build on tmpfs, `-pipe`, `ninja -j` above nproc. | output is on a normal filesystem | small | low | tmpfs needs RAM (build tree is ~3.5 GB of `.o`); `-j` above nproc can help a mixed compile+link phase. |
| C4 | **`-gsplit-dwarf`** for local Debug builds | keeps full debug info, shrinks `.o` | no compile win, faster link | low | Not a compile-time fix and it changes the Debug artefact layout; would need to be justified on its own.  Listed only to record that it is *not* a `-g0` shortcut. |
| C5 | **Try clang** for the ordinary TUs (and get `-ftime-trace`) | profile is parsing-bound, where clang is often faster; GCC only has `-ftime-report` | unknown | medium | Big change (different warnings, different accepted code); needs a full comparison build before any adoption. |

### D. Configuration

| # | idea | evidence | expected | effort | risk / trade-off |
|---|---|---|---|---|---|
| D1 | Confirm nothing is accidentally optimized in Debug | flags are `-g -std=gnu++17` only | n/a | n/a | Already clean. |
| D2 | Check the cost of position-independent code.  The toolchain is configured `--enable-default-pie`, so every object is PIC even though CMake passes no `-fPIE`; `-no-pie` would remove that work. | `g++ -v` reports `enable-default-pie`; no `-fPIE` in `build.ninja` | small-medium serial | low | Changes the executables to non-PIE; a behaviour/packaging decision, not purely a build one. |
| D3 | Trim the 11 `-I` / 7 `-D` set | minor | negligible | low | Low. |

### E. Process and infrastructure

| # | idea | evidence | expected | effort | risk / trade-off |
|---|---|---|---|---|---|
| E1 | **Track build time and sizes in CI** with a budget (the numbers in this document are the baseline). | manual today | prevents regressions | low | None. |
| E2 | A small script that reduces `.ninja_log` to *existing* outputs and prints the per-category/ per-TU table above. | `.ninja_log` keeps stale entries (896 of 1235 outputs no longer exist) and is easy to misread | makes every future comparison trustworthy | low | None; this document's tables were produced with an ad-hoc version of it. |
| E3 | **Explain the `nix build` vs `ninja` gap.**  `time nix build` was 1:26 while `ninja` in a configured tree is ~47 s; the difference includes the submodule builds (`syn68k`, `PowerCore`, `cxmon`, `lmdb`, gtest), CMake configuration, and Nix sandbox overhead. | user's 1:26 vs 47 s | unknown | medium | Needs `nix build --print-build-logs` / `--trace-verbose` before anything can be claimed; may be mostly fixed cost that no candidate here touches. |

### F. Continued work on the generated trap machinery (for completeness)

The trap work stopped when its own plan was complete; these are leftovers that
would now count as "general" build-time work:

- The generated headers still carry **2004 wrapper classes**; A2 above is the
  concrete way to stop every hand-written TU paying for them.
- `structdump` is still 66 separate TUs generating `describeStruct` for every
  type, even types no logging path mentions.

## Recommended sequencing

1. **E2** first (analysis tooling, ~no risk) so every later number is comparable.
2. **A1 prototype** (PCH over `base/*` only, then over `api/*`) because it has the
   largest potential serial reduction and the measurement will settle whether it
   is usable at all.  If it works, it may also make A3/A4 unnecessary.
3. **B1** (`main.cpp` ×5) — a contained, well-understood ~25 s serial saving.
4. **A2** (header split) — the direct continuation of the trap work, and the one
   most likely to help the 214 ordinary TUs.
5. **E3** in parallel (it is investigation, not editing).
6. Anything from A3/A4/B2/B3/C/D that the earlier measurements point at.

## Measurement protocol

For each candidate:

```
ninja -C build clean && time ninja -C build
stat -c '%s %n' build/executor build/tests/tests build/src/libromlib.a
strip -o /tmp/ex.stripped build/executor && stat -c '%s %n' /tmp/ex.stripped
ctest --test-dir build -LE xfail
cmake --build tests/build && build/executor --headless tests/build/tests.ad
```

plus, for the TUs a candidate claims to help, `-ftime-report` (GCC) or
`-ftime-trace` (clang) on one representative TU, and the existing output/user/sys
wall figures.  Candidates that change incremental behaviour (A1, C1) must also
report a warm-tree number.

## Non-goals

- Dropping `-g` in any form, on generated or hand-written code.
- Building only part of the tree (no headless-only, no generated-code-only, no
  skipping front-ends or tests).
- Anything that changes what is compiled and tested; only *how*.
- The trap-machinery refactors already done — this document assumes them.
