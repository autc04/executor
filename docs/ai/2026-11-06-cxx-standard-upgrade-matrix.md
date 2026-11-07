# C++ standard upgrade matrix

Status: research note, 2026-11-06. Executor has since been switched to C++20
(`set(CMAKE_CXX_STANDARD 20)` in `CMakeLists.txt`); the analysis below remains
the rationale for not going beyond that yet.

When this note was written, Executor compiled as C++17
(`set(CMAKE_CXX_STANDARD 17)`). This note works out which C++20/23/26 subsets
are reachable on which compilers and which environments, so that a standard
bump can be scoped against real toolchains rather than assumptions.

Data is taken from the per-feature support tables at
<https://en.cppreference.com/w/cpp/compiler_support> (which track unreleased
GCC 17 / Clang 24 / MSVC 19.52 development), GCC's
<https://gcc.gnu.org/projects/cxx-status.html>, Clang's
<https://clang.llvm.org/cxx_status.html>, Microsoft's
[language conformance](https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance)
and [compiler version](https://learn.microsoft.com/en-us/cpp/overview/compiler-versions)
pages, CMake's `cmake-cxxmodules(7)` manual, and the package metadata of
Nixpkgs 25.11 / nixpkgs-unstable, Ubuntu, Debian, Arch and Visual Studio.
Distribution versions were read from the packages themselves; the only
inferred figures are the Apple Clang 21 percentages, which cppreference does
not tabulate yet (noted below). For reference, LLVM 22.1.0 was released on
2026-02-24 and LLVM 23.1.0 on 2026-08-25; LLVM 24 is still the development
branch (expected around Feb 2027).

Conventions used below:

- **All** means every non-N/A feature that cppreference lists in that section.
  **≥ 90 %**, **≥ 95 %**, **≥ 85 %**, **≥ 50 %** are counted over the same
  lists (DRs included, N/A rows excluded).
- A few "missing" items are dead defect reports that no compiler implements.
  They are identified explicitly so they are not mistaken for real gaps.
- Environment columns in the table give the oldest *currently maintained*
  release whose stock compilers satisfy the subset, naming the compiler that
  does it. For rolling systems (Arch, Debian sid, nixpkgs-unstable) the date is
  the package or snapshot date. `AC` = Apple Clang.
- The **max portable set** rows are intersections, not thresholds: they name
  the largest C++23 core+library feature set supported simultaneously by every
  compiler in a target list. Their cells say which environment covers that
  set, not which compiler has the most features.

## The subsets

### C++20 concepts

P0734R0: constrained templates, `requires`-clauses and `requires`-expressions,
and abbreviated function templates. It is a language-only feature with no
library dependency beyond `<concepts>`, so every compiler picked it up early
and support has been stable for years.

**Required:** GCC 10.1 (May 2020) · Clang 10.0 (Mar 2020) · MSVC 19.30 / VS 2022 17.0 (Nov 2021) · Apple Clang 12.0 (Sep 2020).

### C++20 coroutines

P0912R5: `co_await` / `co_yield` / `co_return`, the promise-type protocol, and
`<coroutine>`. GCC has had it since 10 and MSVC since 19.28; Clang shipped
partial support from 8 but only became dependable in 14, and coroutines on
Clang's Windows targets still carry ABI/stability caveats. The library part is
small, but writing a usable coroutine type requires a fair amount of library
support, so "the language works" is not the same as "a generator library
works" — `<generator>` is C++23 and comes much later (see below).

**Required:** GCC 10.1 (May 2020) · Clang 14.0 (Mar 2022) · MSVC 19.28 / VS 2019 16.8 (Nov 2020) · Apple Clang 15.0 (Sep 2023).

### C++20 core language, no modules

Everything in C++20 except P1103 modules: three-way comparison, designated
initializers, `consteval`/`constinit`, `using enum`, `explicit(bool)`, class
types as non-type template parameters, the lambda changes, `char8_t`,
`[[likely]]`/`[[unlikely]]`/`[[no_unique_address]]`, constexpr extensions.

GCC and MSVC reach 100 % at 12 and 19.31 respectively. Clang's ceiling is
68/69: the single missing item is **DR11 "Simplifying implicit lambda
capture"**, which Clang has never implemented and which no code written today
depends on — Clang 17 is already at 66/69, so the practical floor is lower
than the "complete" number suggests. Apple Clang 16 (Xcode 16) adds three more
gaps — conditionally-trivial special member functions and CTAD for alias
templates and for aggregates — which Apple Clang 17 (LLVM 19.1.5) closes.

**Required:** GCC 12.1 (May 2022) · Clang 19.1 (Sep 2024), or Clang 17 in practice · MSVC 19.31 / VS 2022 17.1 (Feb 2022) · Apple Clang 17.0 (Sep 2025).

### C++20 core and library, no modules

Adds the C++20 standard library: ranges and views, `<format>`, `<span>`,
`<bit>`, `<numbers>`, `<source_location>`, `std::jthread`/`std::stop_token`,
calendars and time zones, constexpr containers, `std::atomic_ref`,
`std::osyncstream`.

This is the first row where standard libraries, not compilers, are the
constraint. libstdc++ completes the list in GCC 14 (79/79); MSVC's STL in
19.42. **libc++ never completes it**: even at LLVM 24 (unreleased; due around
Feb 2027) it is at 76/79, missing
standard-library header units, `std::atomic<std::shared_ptr>`/`std::weak_ptr`,
and the layout-compatibility/pointer-interconvertibility traits. Apple's libc++
16 (Xcode 26.0–26.3) is at 72/79, additionally missing
`std::basic_osyncstream`, atomic compare-and-exchange with padding bits,
`std::execution::unseq` and `std::stop_token`/`std::jthread`; Apple Clang 21
(Xcode 26.4+) is at 75/79. All of these are niche in a program like Executor,
so Clang 20 / Apple Clang 21 is "complete for practical purposes".

**Required:** GCC 14.1 (May 2024) · Clang 20.1 (Mar 2025) · MSVC 19.42 / VS 2022 17.12 (Nov 2024) · Apple Clang 21.0 (Mar 2026).

### C++20 named modules

P1103R3 and P1766R1: `export module`, `import`, module partitions, global
module fragments. Compiler support is only half the story: CMake's scanner
(`cmake-cxxmodules(7)`, in-tree since CMake 3.28) supports **GCC 14+, Clang
16+, MSVC toolset 14.34+ (VS 17.4+)**, and clang-cl 19.1+ (CMake 4.4+, and not
with the Visual Studio generators). A GCC 13 toolchain has `-fmodules-ts` but
CMake will not order the build for it, which is why Ubuntu 24.04's default GCC
appears as "Clang only" in the table.

GCC is still the weak implementation: even in GCC 16 modules need
`-fmodules-ts`, and private module fragments plus parser-level global-module-
entity merging are unimplemented. Clang has been complete since 15, MSVC since
19.28/VS 16.8. Header units remain unreliable everywhere, so a migration
should use named modules only.

**Required:** GCC 14.1 (May 2024; compiler partial since 11) · Clang 16.0 (Mar 2023; compiler complete since 15) · MSVC 19.28 / VS 2019 16.8 (Nov 2020) · Apple Clang 16.0 (Sep 2024).

### C++20 plus `import std`

Technically a C++23 library feature (P2465R3, the `std` and `std.compat`
modules), but normally used from C++20/23 code. It needs both a compiler that
can build the module interface and a standard library that ships it:
libstdc++ 15+, libc++ 17+ (still partial), MSVC STL 19.36+. **Clang on
libstdc++ cannot do this at all** — the module belongs to the library, so the
stdlib choice matters more than the compiler.

**Required:** GCC 15.1 (Apr 2025) · Clang 17.0 + libc++ (Sep 2023) · MSVC 19.36 / VS 2022 17.6 (Jun 2023) · Apple Clang 17.0 (Sep 2025).

### C++23 core language, ≥ 90 % ("almost all")

Deducing `this` (P0847), multidimensional subscript (P2128), `static
operator()`/`operator[]` (P1169/P2589), `auto(x)` (P0849), `#elifdef` (P2334),
`#warning` (P2437), delimited escape sequences (P2290), named universal
character escapes (P2071), labels at the end of compound statements (P2324),
relaxed `constexpr` restrictions (P2448), `[[assume]]` (P1774), CTAD from
inherited constructors (P2582), `if consteval` (P1938), named character
escapes.

What is still missing *at the threshold*:

- **GCC 14 (40/43):** P2644R1/P2718R0 (lifetime extension of temporaries in a
  range-for initializer), P2036R3/P2579R0 (scope of a lambda
  trailing-return-type), P2615R1 (meaningful `export`s). All three arrive in
  GCC 15, and P2036R3 in GCC 16.
- **Clang 20 (39/42, Mar 2025):** requirements for optional extended floating-point
  types, CTAD from inherited constructors, and the DR20 adjustment of the
  `__cpp_concepts` value. None of these change at Clang 21–23.
- **MSVC 19.51 (41/42):** only P2564R3 (propagation of `consteval`), fixed in
  19.52.
- **MSVC 19.50 (32/42)** is noticeably behind for a VS 2026 toolset: it is
  still missing character sets/encodings, non-encodable wide literals, labels
  at the end of blocks, named UCNs, relaxed `constexpr`, CTAD from inherited
  constructors, `[[assume]]`, static `constexpr` variables in `constexpr`
  functions, range-for lifetime extension and `consteval` propagation.
- **Apple Clang 21** (Mar 2026) tracks Clang 21 (Aug 2025) and therefore has
  Clang's three gaps.

**Required:** GCC 14.1 (May 2024) · Clang 20.1 (Mar 2025) · MSVC 19.51 / VS 2026 18.x (2026) · Apple Clang 21.0 (Mar 2026).

### C++23 core language, all

The ≥ 90 % set plus the remaining defect reports, notably P2036R3/P2579R0 and
P2564R3. Only **GCC 16 and MSVC 19.52** reach 100 %.

Clang and Apple Clang stop at 39/42 and will not improve without new work:
CTAD from inherited constructors and the `__cpp_concepts` DR are unimplemented
(the third row is the optional extended-floating-point feature, which is
optional and unsupported by most compilers). This is a permanent ceiling, not
a "wait one release" situation.

**Required:** GCC 16.1 (Apr 2026) · Clang: never (39/42) · MSVC 19.52 / VS 2026 18.x (2026) · Apple Clang: never.

### C++23 library, ≥ 90 %

`<expected>`, `<print>`, `<stacktrace>`, `<mdspan>`, `<flat_map>`/`<flat_set>`,
`<generator>`, `std::move_only_function`, `std::byteswap`,
`std::unreachable`, `out_ptr`/`inout_ptr`, `resize_and_overwrite`, and the
ranges additions (`zip`, `chunk`, `slide`, `join_with`, `stride`, `enumerate`,
`cartesian_product`, fold algorithms).

What is still missing *at the threshold*:

- **GCC 16 (98/102):** disallowing user specializations of
  `std::allocator_traits`, deprecating `std::numeric_limits::has_denorm`
  (GCC 17), the DR "cleaning up integer-class types", and the DR on
  `std::barrier`'s phase-completion guarantees.
- **MSVC 19.42 (95/102):** constexpr `<cmath>`/`<cstdlib>`,
  `std::reference_constructs_from_temporary` /
  `std::reference_converts_from_temporary`, `<flat_map>`, `<flat_set>`,
  `std::generator`, `std::start_lifetime_as`, `std::is_implicit_lifetime` —
  the latter five all land in 19.51/19.52.
- **libc++ 23 (83/102, Aug 2026)** is the reason no Clang configuration appears here.
  Missing: `<stacktrace>`, `<spanstream>`, heterogeneous erasure for
  associative containers, `std::move_only_function`, constexpr
  `<cmath>`/`<cstdlib>`, `views::chunk`/`slide`, the freestanding "easy"
  facilities, `views::as_const`, `views::cartesian_product`, ranges iterators
  as inputs to non-ranges algorithms, `std::generator`,
  `std::start_lifetime_as`, localized chrono formatting, CTAD guides for
  explicit-object call operators, extended-floating-point library names, and
  several DRs (input-range adaptors, chrono locale handling, integer-class
  types, `std::barrier`).
- **libc++ 21 (77/102, i.e. Apple Clang 21)** additionally lacks `views::zip`
  (partial until 22), `ranges::iota`/`shift_left`/`shift_right` and
  `views::stride` and `views::enumerate` (23), the ranges fold algorithms, and
  move-only support in the comparison concepts (22).

**Required:** GCC 16.1 (Apr 2026) · Clang: never (81 % at LLVM 23, Aug 2026) · MSVC 19.42 / VS 2022 17.12 (Nov 2024) · Apple Clang: never (75 %).

### C++23 core and library, ≥ 95 % ("almost all of C++23")

This is the row to use as a working target if both language and library
matter. **GCC 16** gives 100 % of the language and 96 % of the library; its
only four library gaps are listed above. **MSVC 19.51** gives 98 %/99 %, with
P2564R3 (`consteval` propagation) as the sole language gap and constexpr
`<cmath>`/`<cstdlib>` the sole library gap. **GCC 17** (unreleased) would be
99 % library.

Nothing else qualifies: no Clang/Apple configuration can reach 95 % of the
C++23 library because of the libc++ ceiling described above, regardless of how
new the compiler is. For Executor this mostly means `std::expected`,
`<print>`, `std::generator`, `std::move_only_function` and `flat_map` are the
features that decide whether a given toolchain is usable.

**Required:** GCC 16.1 (Apr 2026) · Clang: never · MSVC 19.51 / VS 2026 18.x (2026) · Apple Clang: never.

### C++23 max portable set — nixpkgs, VS 2026, Xcode 26.4, Ubuntu 26.10

This row flips the question: instead of "which features do the newest
compilers have", it asks "which C++23 features can one code base use if it must
build on all of these at once" — nixpkgs-unstable (GCC 16.2 on Linux, Clang 21
+ libc++ on Darwin), VS 2026 18.11 (MSVC 19.52), Xcode 26.4 (Apple Clang 21 /
libc++ 21) and Ubuntu 26.10 (GCC 15.2, Clang 22). The intersection is **38 of
the 41 applicable core-language features (93 %) and 68 of 102 library features
(67 %)**. The library is limited by libc++ 21 (reached via Xcode 26.4 and via
Nixpkgs' Darwin stdenv), the language by Clang 21 plus GCC 15.2.

What falls out of the set:

- **Core (3):** CTAD from inherited constructors (never implemented by Clang),
  the DR on the scope of a lambda trailing-return-type (GCC 16), and the DR
  adjusting the `__cpp_concepts` value (never implemented by Clang).
  "Requirements for optional extended floating-point types" is also absent,
  but it is optional and MSVC marks it N/A, so it is not counted.
- **Library (34).** Absent from libc++ 21: `<stacktrace>`, `<spanstream>`,
  `std::move_only_function`, `std::generator`, `views::chunk`/`slide`,
  `views::as_const`, `views::cartesian_product`, the freestanding "easy"
  `<utility>`/`<ranges>`/`<iterator>` facilities, constexpr
  `<cmath>`/`<cstdlib>`, `std::start_lifetime_as`, the chrono
  localized-formatting clarification, CTAD guides for explicit-object call
  operators, the extended-FP library names, and the input-range-adaptor and
  chrono-locale DRs. Newer than libc++ 21: `views::zip` and move-only
  comparison concepts (22), `ranges::iota`/`shift_left`/`shift_right`,
  `views::stride`, `views::enumerate` and the ranges fold algorithms (23).
  Added in GCC 16, and therefore absent from libstdc++ 15 (Ubuntu 26.10) as
  well: allocator size feedback, `ranges::starts_with`/`ends_with`,
  heterogeneous erasure, `std::mdspan`, ranges iterators as non-ranges inputs,
  `basic_string::substr() &&`, `std::is_implicit_lifetime`,
  `common_reference_t` of `reference_wrapper`, and explicit multi-parameter view
  constructors. Shared DRs: integer-class types and `std::barrier` phase
  completion. GCC 17 only: disallowing `allocator_traits` specialization and
  deprecating `numeric_limits::has_denorm`.

On the pinned Nixpkgs 25.11 the same set is reachable through the packaged
`gcc15` or an opt-in libc++, but neither is the default toolchain.

**Required:** GCC 15.1 (Apr 2025) · Clang 21.1 (Aug 2025) · MSVC 19.52 / VS 2026 18.x (2026) · Apple Clang 21.0 (Mar 2026).

### C++23 max portable set — the same targets plus Ubuntu 26.04

Adding Ubuntu 26.04 does not shrink the subset: its toolchain (GCC 15.2,
Clang 21.1.6 / libc++ 21.1.6) is already represented by Ubuntu 26.10's GCC 15.2
and by Xcode 26.4's Clang 21, so it removes nothing the other targets did not
already remove. The row stays at **38/41 core and 68/102 library** and is
identical in content to the previous one. The two would diverge only if 26.04
were pinned to a Clang older than 21 or if the target list dropped the Clang 21
baseline; in that case this row is the conservative one.

**Required:** same as the previous row.

### C++26, some features

Relaxed `constexpr` (P2242/P2448 follow-ons), pack indexing (P2662),
`= delete("reason")` (P2573), variadic friends (P2893), `#embed` (P1967),
`@`/`$`/backtick in the basic character set (P2558), constexpr cast from
`void*` (P2738), user-generated `static_assert` messages (P2741), structured
binding changes. On the library side `std::inplace_vector`, `<simd>`,
`std::function_ref`, `std::copyable_function`, `std::dims`, `<debugging>`.

GCC 14 and Clang 17 are the compiler floors; Apple Clang 16 (Xcode 16) already
has a handful of them. **MSVC has no documented C++26 core-language support at
all** — only scattered STL features from 19.38 — so on Windows "some C++26"
means "some of the library", not the language.

**Required:** GCC 14.1 (May 2024) · Clang 17.0 (Sep 2023) · MSVC: STL only, from 19.38 / VS 2022 17.8 (Nov 2023) · Apple Clang 16.0 (Sep 2024).

### C++26 core language, ≥ 85 %

The defining items are **contracts (P2900), static reflection (P2996),
expansion statements (P1306), constexpr virtual inheritance, trivial unions,
and erroneous-behaviour/uninitialized-read rules**. **GCC 16 is the only
compiler that ships contracts and reflection**, which is why it is the only
one at 86 %.

What is still missing *at the threshold* for GCC 16 (36/42): ordering of
constraints involving fold expressions; concept and variable-template template
parameters; trivial unions (GCC 17); the DR allowing `#line` before module
declarations; CTAD for type template-template parameters; and const-ification
of splice expressions in contract assertions (GCC 17).

For comparison, Clang 22 (27/42, Feb 2026) is missing the list above plus everything
else that is new in C++26: static storage for braced initializers,
`[[indeterminate]]`/erroneous behaviour, the module-declaration-macro DR,
constexpr exceptions, observable checkpoints, contracts, reflection, expansion
statements, constexpr virtual inheritance, diagnosing preprocessor errors,
`main` attached to the global module, the `#line` DR, CTAD for
template-template parameters, and splice const-ification. Apple Clang 21
(≈ 25/42) also lacks constexpr structured bindings and concept/variable-
template template parameters. MSVC documents zero of the 42.

**Required:** GCC 16.1 (Apr 2026) · Clang: never (64 % max) · MSVC: never (0 documented) · Apple Clang: never (~60 %).

### C++26 library, ≥ 50 %

libstdc++ has made by far the largest C++26 investment: GCC 16 is at 94/137
(69 %). Clang 23 (Aug 2026) is at 48/137 (35 %), Apple's libc++ 21 (Mar 2026)
at 32/137 (23 %), MSVC 19.52 at 26/137 (19 %).

The 43 features GCC 16 still lacks are concentrated in areas rather than
scattered: `<rcu>`, `<hazard_pointer>`, `<linalg>`, `<hive>`; the execution
control library and senders/receivers (parallel scheduler, async scopes,
coroutine task types, `execution::write_env`, parallel range algorithms);
remaining `<simd>` operations (math, bit, permutation, interleaved complex,
`chunk`); constexpr `atomic`/`atomic_ref` and constexpr `shared_ptr`;
atomic floating-point min/max and reduction operations; most freestanding
library work; `ranges::generate_random`,
`approximately_sized_range`/`reserve_hint`, constexpr `inplace_vector` for
non-trivial types, `std::is_within_lifetime` and `std::start_lifetime` (both
GCC 17), `apply_result`/applicability traits, `path::display_string`,
constexpr text formatting, the `flat_*` fixes and the valarray `begin`/`end`
DR; plus removing the deprecated `<codecvt>` facets, strstreams,
`wstring_convert`, the `shared_ptr` atomic-access APIs, the zero-argument
`basic_string::reserve()` and defanging `memory_order::consume`.

**Required:** GCC 16.1 (Apr 2026) · Clang: never (35 % max) · MSVC: never (19 % max) · Apple Clang: never (23 %).

### C++26, all

Not achievable with any shipped compiler. The furthest along is GCC 17 (in
development, expected April 2027) at 38/42 core language features (90 %) and
97/137 library features (71 %); its four remaining language gaps are ordering
of constraints involving fold expressions, concept/variable-template
template parameters, and two DRs (`#line` before module declarations, CTAD
for type template-template parameters), while almost all of the execution,
SIMD and freestanding work is still outstanding.
Clang's C++26 library support is progressing roughly one LLVM release per
feature cluster and is several years behind. The standard was finalized in
2026; a "complete C++26 compiler" is a 2027–2028 proposition at the earliest.

**Required:** none available.

## Final table

`—` = not achievable on that environment at any current release.

| Subset | GCC | Clang | Apple Clang | MSVC | nixpkgs Linux | nixpkgs macOS | macOS (Xcode) | Ubuntu | Debian | Arch (rolling) | Visual Studio |
|---|---|---|---|---|---|---|---|---|---|---|---|
| C++20 Concepts | 10.1 (May 2020) | 10.0 (Mar 2020) | 12.0 (Sep 2020) | 19.30 / VS 17.0 (Nov 2021) | 25.11 (Nov 2025) · GCC 14.3 | 25.11 (Nov 2025) · Clang 21 | 26.0 (Sep 2025) · AC 17 | 24.04 LTS (Apr 2024) · GCC 13.2 | 13 (Aug 2025) · GCC 14.2 | GCC 16.2 (Aug 2026) | 17.14 (May 2025) · MSVC 19.44 |
| C++20 Coroutines | 10.1 (May 2020) | 14.0 (Mar 2022) | 15.0 (Sep 2023) | 19.28 / VS 16.8 (Nov 2020) | 25.11 (Nov 2025) · GCC 14.3 | 25.11 (Nov 2025) · Clang 21 | 26.0 (Sep 2025) · AC 17 | 24.04 LTS (Apr 2024) · GCC 13.2 | 13 (Aug 2025) · GCC 14.2 | GCC 16.2 (Aug 2026) | 17.14 (May 2025) · MSVC 19.44 |
| C++20 core language, no modules | 12.1 (May 2022) | 19.1 (Sep 2024) † | 17.0 (Sep 2025) † | 19.31 / VS 17.1 (Feb 2022) | 25.11 (Nov 2025) · GCC 14.3 | 25.11 (Nov 2025) · Clang 21 | 26.0 (Sep 2025) · AC 17 | 24.04 LTS (Apr 2024) · GCC 13.2 | 13 (Aug 2025) · GCC 14.2 | GCC 16.2 (Aug 2026) | 17.14 (May 2025) · MSVC 19.44 |
| C++20 core + library, no modules | 14.1 (May 2024) | — (76/79 max; LLVM 24, unreleased) | — (75/79 max, AC 21) | 19.42 / VS 17.12 (Nov 2024) | 25.11 (Nov 2025) · GCC 14.3 | — (libc++ 75/79; opt-in GCC closes it) | — | 26.04 LTS (Apr 2026) · GCC 15.2 | 13 (Aug 2025) · GCC 14.2 | GCC 16.2 (Aug 2026) | 17.14 (May 2025) · MSVC 19.44 |
| C++20 named modules | 14.1 (May 2024) ‡ | 16.0 (Mar 2023) ‡ | 16.0 (Sep 2024) | 19.28 / VS 16.8 (Nov 2020) | 25.11 (Nov 2025) · GCC 14.3 | 25.11 (Nov 2025) · Clang 21 | 26.0 (Sep 2025) · AC 17 | 24.04 LTS (Apr 2024) · Clang 18 | 13 (Aug 2025) · GCC 14.2 | GCC 16.2 (Aug 2026) | 17.14 (May 2025) · MSVC 19.44 |
| C++20 + `import std` | 15.1 (Apr 2025) | 17.0 (Sep 2023) + libc++ | 17.0 (Sep 2025) · libc++ 19 | 19.36 / VS 17.6 (Jun 2023) | 25.11 (Nov 2025) · Clang 21 + libc++ | 25.11 (Nov 2025) · Clang 21 + libc++ (default) | 26.0 (Sep 2025) · libc++ 19 | 24.04 LTS (Apr 2024) · Clang 18 + libc++ | 13 (Aug 2025) · Clang 19 + libc++ | GCC 16.2 (Aug 2026) | 17.14 (May 2025) · MSVC 19.44 |
| C++23 core, ≥90 % ("almost all") | 14.1 (May 2024) | 20.1 (Mar 2025) | 21.0 (Mar 2026) | 19.51 / VS 2026 18.x (2026) | 25.11 (Nov 2025) · GCC 14.3 | 25.11 (Nov 2025) · Clang 21 | 26.4 (Mar 2026) · AC 21 | 24.04 LTS (Apr 2024) · Clang 18 | 13 (Aug 2025) · GCC 14.2 | GCC 16.2 (Aug 2026) | 18.x (2026) · MSVC 19.51 |
| C++23 core, all | 16.1 (Apr 2026) | — (39/42 max) | — (39/42 max) | 19.52 / VS 2026 18.x (2026) | unstable (Nov 2026) · GCC 16.2 | unstable (Nov 2026) · gcc16 (opt-in) | — | — (GCC 15.2 = 42/43) | sid (Nov 2026) · GCC 16.2 | GCC 16.2 (Aug 2026) | 18.x (2026) · MSVC 19.52 |
| C++23 library, ≥90 % | 16.1 (Apr 2026) | — (81 % max; LLVM 23, Aug 2026) | — (75 % max, AC 21) | 19.42 / VS 17.12 (Nov 2024) | unstable (Nov 2026) · GCC 16.2 | unstable (Nov 2026) · gcc16 (opt-in) | — | — (84 % max, GCC 15.2) | sid (Nov 2026) · GCC 16.2 | GCC 16.2 (Aug 2026) | 17.14 (May 2025) · MSVC 19.44 |
| C++23 core + lib, ≥95 % ("almost all of C++23") | 16.1 (Apr 2026) · 96 % lib | — | — | 19.51 / VS 2026 18.x (2026) | unstable (Nov 2026) · GCC 16.2 | unstable (Nov 2026) · gcc16 (opt-in) | — | — | sid (Nov 2026) · GCC 16.2 | GCC 16.2 (Aug 2026) | 18.x (2026) · MSVC 19.51 |
| C++23 max portable set — nixpkgs + VS 2026 + Xcode 26.4 + Ubuntu 26.10 (38/41 core, 68/102 lib) | 15.1 (Apr 2025) | 21.1 (Aug 2025) | 21.0 (Mar 2026) | 19.52 / VS 2026 18.x (2026) | unstable (Nov 2026) · GCC 16.2 | 25.11 (Nov 2025) · Clang 21 + libc++ | 26.4 (Mar 2026) · AC 21 | 26.10 (Oct 2026) · GCC 15.2 | sid (Nov 2026) · GCC 16.2 | GCC 16.2 (Aug 2026) | 18.x (2026) · MSVC 19.52 |
| C++23 max portable set — same plus Ubuntu 26.04 (38/41 core, 68/102 lib) | 15.1 (Apr 2025) | 21.1 (Aug 2025) | 21.0 (Mar 2026) | 19.52 / VS 2026 18.x (2026) | unstable (Nov 2026) · GCC 16.2 | 25.11 (Nov 2025) · Clang 21 + libc++ | 26.4 (Mar 2026) · AC 21 | 26.04 LTS (Apr 2026) · GCC 15.2 | sid (Nov 2026) · GCC 16.2 | GCC 16.2 (Aug 2026) | 18.x (2026) · MSVC 19.52 |
| C++26, some features | 14.1 (May 2024) | 17.0 (Sep 2023) | 16.0 (Sep 2024) | STL only: 19.38 / VS 17.8 (Nov 2023) | 25.11 (Nov 2025) · GCC 14.3 | 25.11 (Nov 2025) · Clang 21 | 26.0 (Sep 2025) · AC 17 | 24.04 LTS (Apr 2024) · GCC 13.2 | 13 (Aug 2025) · GCC 14.2 | GCC 16.2 (Aug 2026) | 17.14 (May 2025) · MSVC 19.44 |
| C++26 core, ≥85 % | 16.1 (Apr 2026) · 86 % | — (64 % max; LLVM 22, Feb 2026) | — (~60 %, AC 21) | — (0 documented) | unstable (Nov 2026) · GCC 16.2 | unstable (Nov 2026) · gcc16 (opt-in) | — | — (60 % max, GCC 15.2) | sid (Nov 2026) · GCC 16.2 | GCC 16.2 (Aug 2026) | — |
| C++26 library, ≥50 % | 16.1 (Apr 2026) · 69 % | — (35 % max; LLVM 23, Aug 2026) | — (23 % max, AC 21) | — (19 % max, 19.52) | unstable (Nov 2026) · GCC 16.2 | unstable (Nov 2026) · gcc16 (opt-in) | — | — | sid (Nov 2026) · GCC 16.2 | GCC 16.2 (Aug 2026) | — |
| C++26, all | — (GCC 17: 90 % core / 71 % lib) | — | — | — | — | — | — | — | — | — | — |

### Table notes

- † Complete except dead DRs: Clang never implements DR11 "simplifying
  implicit lambda capture" (68/69). Apple Clang 16 has three additional gaps
  that Apple Clang 17 closes.
- ‡ Modules need compiler **and** build system: CMake ≥ 3.28 scans for modules
  with GCC 14+, Clang 16+, MSVC 14.34+ (VS 17.4+). GCC still requires
  `-fmodules-ts` and lacks private module fragments.
- `import std` needs a standard library with the `std` module: libstdc++ 15+,
  libc++ 17+ (partial), MSVC STL 19.36+. Clang on libstdc++ cannot do it.
- MSVC has no `/std:c++23` and no `/std:c++26`; use `/std:c++latest` (or
  `/std:c++23preview` from VS 17.13). MSVC versioning is decoupled from Visual
  Studio versioning since VS 2026, hence "18.x (2026)" for 19.51/19.52.
- Apple Clang skipped 18–20: 17.0 ships with Xcode 26.0–26.3 (LLVM 19.1.5),
  21.0 with Xcode 26.4+ and Xcode 27 (LLVM 21.1.6). cppreference has no Apple
  Clang 21 column yet, so those figures are inferred from Clang 21/libc++ 21.
  Xcode 26 requires macOS 15.6+; Xcode 27 requires macOS 26.6+.
- LLVM/Clang release dates used above: 20.1.0 — Mar 2025, 21.1.0 — Aug 2025,
  22.1.0 — 24 Feb 2026, 23.1.0 — 25 Aug 2026. **LLVM 24 has not been
  released** (in development, expected around Feb 2027), which is why it only
  appears as a ceiling in the C++20 library row.
- Nixpkgs uses different default compilers per platform. On **Linux** the
  stdenv compiler is GCC (14.3 in 25.11, 16.2 in unstable). On **Darwin** it is
  **Nixpkgs' own Clang, not Apple's**: `pkgs/stdenv/darwin/default.nix` sets
  `llvmVersion = "21"` and `cc = ...llvmPackages_21.clang`, wrapped by
  `cc-wrapper` with `apple-sdk` supplying headers and frameworks and
  `cctools`/`ld64` doing the linking. Apple's `/usr/bin/clang` is not part of
  the stdenv, and `apple-sdk` ships no compiler at all. The Darwin default
  stdlib is libc++, which is why `import std` works with the default Clang 21
  there, whereas Nixpkgs' Clang on Linux defaults to libstdc++ and needs an
  opt-in libc++. GCC is packaged on Darwin (Hydra builds gcc16 for both
  x86_64- and aarch64-darwin) but is never the stdenv compiler and cannot
  compile Objective-C++/Cocoa. Nixpkgs' default is also not its maximum: both
  releases default to Clang 21, while unstable packages Clang 24 / GCC 16.2 and
  25.11 packages Clang 23 / GCC 15.2.
- Environment cells name the oldest currently maintained release whose stock
  compilers satisfy the row. Ubuntu 26.10 (Clang 22.1.6/GCC 15.2, Oct 2026)
  appears only in the max-portable rows, where it is the nominal target; its
  Clang 22 adds no feature those rows rely on over 26.04.

## Implications for Executor

C++20 without modules is safe everywhere in the table, including the pinned
Nixpkgs 25.11: GCC 14.3 plus libstdc++ 14 is 100 % of both the core language
and the C++20 library. That is the low-risk bump.

C++23 requires a decision about which compilers to support. "Almost all of
C++23" needs GCC 16 (Nixpkgs unstable, Debian sid, Arch) or MSVC 19.51+ (VS
2026); it excludes Ubuntu 24.04/26.04, Debian 13, macOS of any Xcode version,
and VS 2022. A weaker but much broader target is C++23 core-only, which GCC
14 / Clang 20 / Apple Clang 21 / MSVC 19.51 all cover, with the standard
library on those toolchains being C++20-level in places.

If one binary must build on all of nixpkgs, VS 2026, Xcode 26.4 and Ubuntu
26.10, the intersection is 38/41 core features and 68/102 library features
(93 % / 67 %). The library losses are dominated by libc++ 21 — `<stacktrace>`,
`<spanstream>`, `std::move_only_function`, `std::generator`, `views::chunk`,
`views::cartesian_product`, plus `std::mdspan` and a few others that require
GCC 16 — so a genuinely portable C++23 subset is noticeably smaller than
"almost all of C++23". Adding Ubuntu 26.04 to the target list changes nothing.

C++26 is not a viable target: GCC 16 (86 % core, 69 % library) is the
furthest along and no environment reaches "almost all".
