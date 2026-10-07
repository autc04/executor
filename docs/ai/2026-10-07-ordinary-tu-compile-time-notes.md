# Ordinary-TU compile-time: existing findings (leads only)

> Date: 2026-10-07
> Status: notes only — no research, planning or implementation done on this

These are findings already gathered while working on
`docs/ai/2026-10-07-generated-trap-entrypoints-plan.md`. They are recorded here
so the leads aren't lost; they have **not** been investigated beyond what is
written, and no plan is proposed.

> Update (later on 2026-10-07): the generated-trap work continued past the point
> these numbers were taken (phases 2a–4c and L3), and those translation units are
> now smaller still.  The ordinary-TU figures below are unaffected by that and
> remain the reason most of the remaining serial compile work is *outside* the
> generated trap TUs.

## Context

Measurements below are from this machine (24 cores), `CMAKE_BUILD_TYPE=Debug`,
`EXECUTOR_ENABLE_LOGGING=ON`, full `ninja clean && ninja`, taken after the
generated-trap entrypoint work (commits through "Phase 1"). Wall time was
0m51.7s.

## Where the serial compile work is

From `build/.ninja_log`, last-wins per output:

| group | serial | TUs |
|---|---|---|
| `trap_instances/*.cpp.o` | 230.7 s | 66 |
| `trap_entries/*.cpp.o` | 99.9 s | 66 |
| `structdump/*.cpp.o` | 86.2 s | 66 |
| **everything else** | **602.3 s** | ~350 |
| total | 1019.0 s | ~489 |

So the majority of the serial compile work (~59%) is already in ordinary,
hand-written translation units, averaging ~1.7 s each.

Wall time tracks total work, not the critical path: 1019 s / 24 cores ≈ 42 s
ideal, against 51.7 s observed. (The longest single TU, `FileMgr`
`trap_instances` at 18.8 s, is well under the wall time.)

## Largest ordinary TUs observed

From the same log (and earlier runs):

| TU | approx |
|---|---|
| `src/config/front-ends/qt/qtkeycodes.cpp` | 6.7–7.1 s |
| `src/main.cpp.o` (per front-end; Qt/X/SDL/SDL2/Wayland) | 5.4–6.3 s each |
| `src/init.cpp` | 6.7 s |
| `src/config/front-ends/qt/qt.cpp` | 5.7 s |
| `src/config/front-ends/qt/available_geometry.cpp` | 5.1 s |
| `src/config/front-ends/wayland/wayland.cpp` | 3.9 s |
| `tests/files.cpp` | 4.4 s |
| `src/config/front-ends/sdl2/sdl2.cpp` | 3.3 s |
| `tests/quickdraw.cpp`, `tests/files_internal.cpp` | 3.5–3.7 s |

`main.cpp`/`init.cpp`/`qtkeycodes.cpp` are among the heaviest because they
include many generated API headers.

## Observations already noted

- The per-front-end code is compiled once per front-end: a `main.cpp` object for
  each of the Qt/X/SDL/SDL2/Wayland front-ends, plus per-front-end sources
  (Qt: `qtkeycodes`, `qt`, `available_geometry`; SDL2: `sdl2`, `keycode_map`;
  Wayland: `wayland`; SDL: `sdlwin`, `sdlevents`, `sdlwm`, `sdlsound`; X: `x`).
- Each front-end executable is also linked separately, ~8 s per link, six links
  including `tests` (a link-time cost centre, distinct from compilation).
- Generated API headers are included widely across ordinary TUs. After the
  generated-trap work each module header additionally carries one wrapper-class
  definition per Pascal trap (566 total across modules), which increases the
  parse/semantic cost of every TU that includes those headers.
- `-ftime-report` on `trap_instances/FileMgr.cpp` (before Phase 1) showed
  template instantiation ~20%, overload resolution + name lookup ~16%, and the
  remainder in codegen — i.e. header *parsing* was not the dominant cost there.
  This was measured on a generated TU, not on an ordinary one, so it may not
  transfer.

## Leads noted but NOT investigated

Recorded verbatim as leads, with no analysis or plan attached:

- Precompiled headers over the stable `base/*` headers plus the generated API
  headers.
- Reducing what each generated module header pulls in.
- Sharing/merging/splitting ordinary TUs.
- Compiler caching (ccache/sccache) across clean builds.
- The ~6 separate ~8 s final link steps.
