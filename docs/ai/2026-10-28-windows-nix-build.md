# Windows cross-build in the Nix flake

The flake exposes three MinGW-w64 cross builds (on `x86_64-linux` only, because
the build itself needs Wine):

```
nix build .#windows-headless   # FRONT_ENDS=headless
nix build .#windows-sdl2       # FRONT_ENDS=sdl2
nix build .#windows-qt6        # FRONT_ENDS=qt (also `.#windows`)
```

All produce `executor.exe` in `$out/bin`. The SDL2 variant additionally ships
`SDL2.dll` and `SDL3.dll`; the Qt variant ships the Qt DLLs and platform
plugins.

## How the build works

- `pkgsCross.ucrt64.stdenv` provides the target toolchain: the
  `x86_64-w64-mingw32` triple with the UCRT runtime rather than the legacy
  MSVCRT one (matching MSYS2 and Windows 10+). Native `cmake`, `ninja`, `bison`,
  `perl` and `ruby` come from the host package set.
- **UCRT runtime:** the produced binaries import `ucrtbase.dll` and the
  `api-ms-win-crt-*` API sets instead of `msvcrt.dll`. These are part of
  Windows 10+; on older Windows the UCRT redistributable is required. The C++
  and threading runtimes (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`,
  `libmcfgthread-2.dll`) are still MinGW DLLs and have to be shipped.
- **Submodules must be part of the flake source.** The flake sets
  `inputs.self.submodules = true`; otherwise the git fetcher copies only the
  superproject's tracked files and CMake fails with
  "The git submodule 'syn68k' is not available".
- **Generators are run through Wine.** `syn68k/syngen` and
  `PowerCore/powergenerator` are compiled for Windows and then executed during
  the build to produce C sources/headers. `CMAKE_CROSSCOMPILING_EMULATOR`
  points at `wine64`, which makes CMake prefix those `COMMAND`s with Wine.
  The derivation initialises the Wine prefix once in `preBuild`, because ninja
  runs both generators in parallel and concurrent first runs of Wine race on
  prefix creation.
- **The MinGW runtime is linked statically** (`-DCMAKE_EXE_LINKER_FLAGS=-static`).
  The generators otherwise need `libstdc++-6.dll`/`libgcc_s_seh-1.dll` at
  runtime, which are not on Wine's DLL search path.
- **Boost:** nixpkgs' MinGW Boost build fails while compiling
  Boost.Stacktrace's `from_exception` backend (`dlfcn.h` is POSIX-only).
  Executor only needs `filesystem` and `program_options`, so the flake overrides
  Boost with `extraB2Args = [ "--without-stacktrace" ]`.
- **SDL2:** nixpkgs' SDL2 is `sdl2-compat`, which `LoadLibrary`s `SDL3.dll` at
  runtime. The flake uses the same `sdl3.override { traySupport = false; }` as
  `sdl2-compat` and copies the resulting `SDL3.dll` next to `executor.exe`.
- **Qt 6:** nixpkgs' Qt 6 does not support a MinGW host out of the box. qtbase
  pulls in libglvnd (POSIX-only), and its system HarfBuzz/libjpeg dependencies
  cannot be cross-compiled (HarfBuzz → glib → target Python and
  libsysprof-capture; libjpeg-turbo's SIMD/12-bit code fails on MinGW, with
  MSVCRT and UCRT alike). Qt also
  propagates libxml2/libxslt, which drag in shared-only libiconv and break the
  `-static` link above. Executor only needs QtBase's Gui module, and Qt can use
  its bundled HarfBuzz/libjpeg, so the flake drops those inputs and disables
  `libproxy`, `sctp`, system HarfBuzz and system JPEG. `QT_HOST_PATH` points at
  the native (build-platform) qtbase for `moc`/`rcc`/`uic`.
-
  The qtbase setup hook insists on a wrapper decision: `dontWrapQtApps = true`
  is set because the wrapper only knows how to wrap ELF executables. The
  platform plugins are installed in `$out/bin/platforms`, which is where this
  Qt build searches (it appends `platforms` to the application directory).
- **Tests are not built.** `tests/CMakeLists.txt` unconditionally calls
  `gtest_discover_tests()`, which tries to run the freshly built test binary at
  build time — impossible for a cross build. The derivation therefore passes
  `ninjaFlags = [ "executor-<front end>" ]` and installs the executable itself.

## MacTCP on Windows

`src/mactcp/mactcp.cpp` is implemented directly on POSIX sockets (`arpa/inet.h`,
`poll`, `ifaddrs`, `writev`, ...). On `_WIN32` it now compiles to a no-op
`InitMacTCPDriver()`, so Windows builds link but do not register the `.IPP`
driver. A Winsock backend would be needed to enable it.

## Testing under Wine

```
nix run nixpkgs#wine64 -- ./result/bin/executor.exe --help
nix run nixpkgs#wine64 -- ./result/bin/executor.exe --headless tests/build/tests.ad
```

The latter runs the in-guest test suite and writes `tests/build/out`. It
reports the same known `FileTest` failures as the native build
(`FileTest.SetFInfo_CrDat`, `FileTest.SetFLock`).

For the Qt build, note that nixpkgs' Wine does not pass environment variables
whose names start with `QT_` through to the Windows process. Set them inside
`cmd` instead, e.g. to run against the offscreen platform:

```
nix run nixpkgs#wine64 -- cmd /c 'set QT_QPA_PLATFORM=offscreen&Z:\path\to\result\bin\executor.exe Z:\path\to\tests\build\tests.ad'
```

With no variable set the Qt front end uses `qwindows` and opens a real window.
Both paths run the full suite (92 tests, 90 pass, the same two known
`FileTest` failures).
