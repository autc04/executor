Executor 2000
=============

This is a modernized (as of 2019) fork of Executor, the "original" version
of which you can still find upstream at https://github.com/ctm/executor.
The home of the "Executor 2000" fork is at https://github.com/autc04/executor.

Why "2000"? Because that's what we used to call the future.

Executor was a commercially available Mac emulator in the 90s -
what makes it different from other Mac emulators is that it doesn't need a
ROM file or any original Apple software to run; rather, it attempts to
re-implement the classic Mac OS APIs, just as WINE does for Windows.

Executor 2000 feature highlights:

  - Builds and runs on modern 64-bit Linux and macOS (Windows support is planned).
  - Rootless - emulated windows are part of your desktop.
  - PowerPC support (well, not many Apps will run, but it's there)
  - 24-bit addressing support (compile-time option: `-DTWENTYFOUR=YES`)
  - Support for native Mac resource forks (Mac version)
  - Exchange files with Basilisk & SheepShaver
  - Lots of code cleanup, according to my own definition of "clean"
  - included debugger based on cxmon from Basilisk
  - started a test suite
  - Removed old, no longer functioning, DOS, NeXT and Windows ports. (Windows
    still works, though, via Qt and SDL ports.)
  - I probably broke lots of other things that used to work.

You can reach the maintainer of this fork at wolfgang.thaller@gmx.net or via
the github issues page at https://github.com/autc4/executor/issues.

License
-------

MIT-Style license, see COPYING.

The cxmon monitor, on which Executor's new built-in debugger is based,
is released under GPL v2+, so you are essentially bound by the GPL's terms
for all of Executor; however, cxmon is an optional component, which can easily
be removed should a non-copyleft version of Executor be needed.

Credits
-------

Executor was originally developped at Abacus Research and Development, Inc (ARDI)
and initially released in 1990. In 2008, Clifford Matthews (https://github.com/ctm)
open-sourced Executor under a MIT license.
The original Credits read:

```
Bill Goldman - Browser, Testing
Mat Hostetter - Syn68k, Low Level Graphics, DOS port, more...
Joel Hunter - Low Level DOS Sound
Sam Lantinga - Win32 port
Patrick LoPresti - High Level Sound, Low Level Linux Sound
Cliff Matthews - this credit list (and most things not listed)
Cotton Seed - High Level Graphics, Apple Events, more...
Lauri Pesonen - Low Level Win32 CD-ROM access (Executor 2.1)
Samuel Vincent - Low Level DOS Serial Port Support
and all the engineers and testers who helped us build version 1.x

Windows Appearance:

The windows appearance option uses "Jim's CDEFs" copyright Jim Stout and the "Infinity Windoid" copyright Troy Gaul.

Primary Pre-Beta Testers:

Jon Abbott - Testing, Icon Design
Ziv Arazi - Testing
Edmund Ronald - Advice, Testing
K. Harrison Liang - Testing
Hugh Mclenaghan - Testing
Emilio Moreno - Testing, Spanish Translation + Keyboard, Icon Design
Ernst Oud - Documentation, Testing
```

After Executor's open-sourcing, C.W. Betts (https://github.com/MaddTheSane)
picked it up and ported the whole thing from plain C to C++.
And after that, yours truly, Wolfgang Thaller (https://github.com/autc04) took
posession of it.  Other contributors include:

  - David Ludwig (https://github.com/DavidLudwig) - Windows build fixes

Building and Running
--------------------

Requirements:
* a modern C++17 compiler
* CMake 3.10 or later
* Qt 5.12 or later
* perl
* ruby 2.0 or later
* bison
* boost

Optional (for additional front-ends):
* SDL 2
* SDL 1.2
* X11 libraries
* waylandpp

Building:
```
git submodule init
git submodule update
mkdir build
cd build
cmake ..
cmake --build .
```

If you want to build executor with 24-bit addressing, use
`cmake .. -DTWENTYFOUR=YES` instead of the regular `cmake ..` command. Note that
this will limit you to about 4MB of emulated RAM and break PowerPC support.

When `./build/src/executor` is first invoked, it will automatically install its
fake Mac system file and the `Browser` finder replacement to `~/.executor/`, so
no further setup should be needed.

If you're using Wayland, avoid the Qt front-end and use the `executor-wayland` binary instead.

Executor 2000 should be able to use native Mac files on macOS, AppleDouble 
file pairs (`foo` and `%foo`) as used by older executor verions, as well as
files written by Basilisk or SheepShaver (`foo`, `.rsrc/foo` and `.finf/foo`).

It should be possible to build Executor 2000 for Microsoft Windows;
see [here](docs/building-on-windows.md) for some only slightly outdated instructions.

Nix Package Manager
-------------------

The source distribution contains a `flake.nix` file for the [Nix Package Manager](https://www.nixos.org).
If you are not a Nix user, skip this section.

As Executor uses submodules, a regular `nix build` will not work; instead, you have to use `nix build .#submodules=1`.
A development shell (`nix develop` or via direnv) should work as usual.

To use executor using nix without checking out the source code, use `git+ssh://www.github.com/autc04/executor?submodules=1#` as the URL, e.g.:
```
nix shell 'git+ssh://www.github.com/autc04/executor?submodules=1'
nix shell 'git+ssh://www.github.com/autc04/executor?submodules=1' -c executor
nix shell 'git+ssh://www.github.com/autc04/executor?submodules=1' -c executor-wayland
```

Build-time Options
------------------

You can specify some build-time options via `cmake`:

### TWENTYFOUR - 24-bit addressing mode

    cmake . -DTWENTYFOUR=TRUE

When this is active, Executor will use 24-bit addressing. This limits the amount of
usable memory to about 4MB, and disables PowerPC support, but might allow some older
applications to run.

### EXECUTOR_ENABLE_LOGGING - Enable per-trap logging

    cmake . -DEXECUTOR_ENABLE_LOGGING=TRUE

When this is active, you can start executor with the `-logtraps` option to get logging
output for every MacOS function (trap) called by the running program.

### NO_STATIC_BOOST - Look for dynamically linked boost libraries (e.g. from MacPorts)

    cmake . -DNO_STATIC_BOOST=TRUE

Without this, you will get the following error when trying to find dynamically linked boost libraries, such as from MacPorts:

    CMake Error at /opt/local/share/cmake-3.24/Modules/FindPackageHandleStandardArgs.cmake:230 (message):
      Could NOT find Boost (missing: filesystem system program_options) (found
      version "1.76.0")
    Call Stack (most recent call first):
      /opt/local/share/cmake-3.24/Modules/FindPackageHandleStandardArgs.cmake:594 (_FPHSA_FAILURE_MESSAGE)
      /opt/local/share/cmake-3.24/Modules/FindBoost.cmake:2376 (find_package_handle_standard_args)
      src/CMakeLists.txt:16 (find_package)

Overview
--------

### Git Submodules

Executor 2000 includes several git submodules, some to include third-party
libraries, and some which are maintained together with Executor.

#### Third party libraries:
- LMDB/lmdb (database library used for filesystem implementation)
- google/googletest
- vector-of-bool/cmrc (the CMake resource compiler, for embedding binaries)

#### Third party library forked with minor fixes and changes:
- autc04/cxmon (low-level debugger from the macemu project)
- autc04/lmdbxx (C++ wrapper for lmdb)

#### Components maintained along with Executor 2000:
- autc04/syn68k (68K emulator core)
- autc04/PowerCore (PowerPC emulator core)
- autc04/multiversal (classic MacOS API definitions plus generator scripts)

### Directories 

- `src/` - main source directory
- `docker/` - used for CI on azure-pipelines
- `res/` - files that are embedded into the executor binary
    (`System`, `Browser`, etc.)
- `tests/` - automated tests for regression testing and for figuring out how
    MacOS really works
- `packages/go/` - source code for the Browser application. Seems to not 
    exactly match the compiled  Browser binary, and I haven't been able to
    compile it yet.
- `packages/skel/` - System Folder and Freeware apps that used to be shipped
    with Executor.
- `docs/` - some extra documentation
- `docs/outdated/` - Outdated docs that might help understanding the past of
    Executor.
- `util/` - a collection of scripts and helper programs, all obsolete

### The "Multiversal Interfaces"

On classic MacOS, compilers would ship with an Apple-supplied package called
the "Universal Interfaces" that contained C header files for all APIs provided
by MacOS.

The `multiversal` repository, included as a git submodule, contains the
"Multiversal Interfaces", an open source reimplementation that is shared
between Executor 2000 and the Retro68 cross-compiler suite.

The directory `multiversal/defs` contains YAML files that describe (a subset
of) the MacOS API; the ruby scripts in `multiversal`, invoked from
`src/CMakeLists.txt`, generate actual C++ header files for Executor from those
descriptions. You can find the C++ headers in `build/src/api` once you've built
Executor 2000, but you should of course only edit the YAML files they are
generated from. 

There is a JSON schema describing the format of those YAML files in
`multiversal/multiversal.schema.json`. If you happen to be using Visual Studio
Code as your text editor, you can install the YAML extension from Red Hat and
add the following to your workspace settings (`.vscode/settings.json`), and
you'll get some autocompletion and automatic error squiggles as you type:

```json
    "yaml.schemas": {
        "multiversal/multiversal.schema.json": "multiversal/defs/*.yaml"
    }
```

### Inside `src/`

The code in `src/` is loosely organized into subdirectories. Each subdirectory
can contain both headers and source files. Some of these headers might be
intended for use from other files in the subdirectory only, while others might
be public. This should be systematically cleaned up at some point. Source files
that didn't fit into any subdirectory have been left at the top level. The
header files corresponding to those `.cpp` files can for historical reasons be
found in `include/rsys/`. I never found out what `rsys` stands for.

