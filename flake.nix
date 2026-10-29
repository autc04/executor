{
  description = "Executor 2000 - classic mac emulator";

  inputs = {
    # The emulator's own dependencies (syn68k, PowerCore, cxmon, lmdb,
    # multiversal, ...) are git submodules.  Without this, the git fetcher
    # copies only the superproject's tracked files and the build fails with
    # "The git submodule 'syn68k' is not available".
    self.submodules = true;

    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";
  };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems =
        [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      perSystem = { config, self', inputs', pkgs, system, ... }:
      let
        # 64-bit Windows cross build with MinGW-w64, using UCRT rather than
        # the legacy MSVCRT runtime (matches MSYS2 and Windows 10+).
        winPkgs = pkgs.pkgsCross.ucrt64;

        # nixpkgs' MinGW Boost build fails on Boost.Stacktrace's
        # `from_exception` backend, which includes the POSIX-only <dlfcn.h>.
        # Executor only needs filesystem and program_options, so skip it.
        winBoost = winPkgs.boost.override {
          extraB2Args = [ "--without-stacktrace" ];
        };

        # nixpkgs' SDL2 is sdl2-compat, which dlopens SDL3.dll at runtime.
        # Match the override used by the sdl2-compat package itself so this
        # resolves to the exact same derivation.
        winSdl3 = winPkgs.sdl3.override { traySupport = false; };

        # nixpkgs' Qt 6 does not support a MinGW host as-is: qtbase pulls in
        # libglvnd (POSIX-only), and its system HarfBuzz/libjpeg dependencies
        # cannot be cross-compiled here (Harfbuzz drags in glib, which needs a
        # target Python and libsysprof-capture).  Executor only needs QtBase's
        # Gui module, and Qt can use its bundled HarfBuzz/libjpeg, so drop
        # those plus the Linux-only feature flags.
        winQtBase = winPkgs.qt6.qtbase.overrideAttrs (old: {
          propagatedBuildInputs = builtins.filter
            (x: !(builtins.elem (x.pname or "")
              [ "libglvnd" "harfbuzz" "libjpeg-turbo"
                "libxml2" "libxslt" ]))
            (old.propagatedBuildInputs or [ ]);
          cmakeFlags = (old.cmakeFlags or [ ]) ++ [
            "-DQT_FEATURE_libproxy=OFF"
            "-DQT_FEATURE_sctp=OFF"
            "-DQT_FEATURE_system_harfbuzz=OFF"
            "-DQT_FEATURE_system_jpeg=OFF"
          ];
        });

        # syn68k ships its own instruction generator (`syngen`) as part of
        # the build and runs it to produce C sources.  In a cross build that
        # executable is a Windows binary, so tell CMake to run it through
        # Wine (CMAKE_CROSSCOMPILING_EMULATOR does this for custom commands
        # that reference an executable target).
        mkWindowsPackage = { frontEnd, buildInputs ? [ ], extraCmakeFlags ? [ ],
                             runtimeFiles ? [ ] }:
          winPkgs.stdenv.mkDerivation {
            pname = "executor2000-windows-${frontEnd}";
            version = "0.1.0";
            src = ./.;
            nativeBuildInputs = with pkgs; [
              cmake
              ninja
              bison
              perl
              ruby
              wine64
            ];
            buildInputs = [ winBoost ] ++ buildInputs;
            hardeningDisable = [ "all" ];
            # The Qt setup hook wants an explicit choice; its wrapper is an
            # ELF-script mechanism and cannot wrap a Windows executable.
            dontWrapQtApps = true;
            cmakeFlags = [
              "-DFRONT_ENDS=${frontEnd}"
              "-DCMAKE_BUILD_TYPE=Release"
              # Link the MinGW runtime statically: the generator programs run
              # under Wine during the build, and the resulting executor.exe
              # should not need libstdc++/libgcc/libwinpthread DLLs either.
              "-DCMAKE_EXE_LINKER_FLAGS=-static"
              "-DCMAKE_CROSSCOMPILING_EMULATOR=${pkgs.wine64}/bin/wine64"
            ] ++ extraCmakeFlags;

            # Wine runs the cross-compiled generator programs (`syngen`,
            # `powergenerator`) during the build.  It needs a writable prefix,
            # which the sandbox's HOME (/homeless-shelter) cannot provide, and
            # the prefix must be created once up front: ninja starts several
            # generator processes in parallel and concurrent first runs race.
            preBuild = ''
              export HOME="$NIX_BUILD_TOP"
              export XDG_CACHE_HOME="$NIX_BUILD_TOP/.cache"
              export XDG_RUNTIME_DIR="$NIX_BUILD_TOP/xdg-runtime"
              mkdir -p "$XDG_CACHE_HOME" "$XDG_RUNTIME_DIR"
              chmod 700 "$XDG_RUNTIME_DIR"
              export WINEPREFIX="$NIX_BUILD_TOP/wineprefix"
              export WINEDEBUG=err+all
              export WINEDLLOVERRIDES="mscoree,mshtml="
              wineboot -u
            '';

            # Build just the emulator.  The native test suite's
            # gtest_discover_tests() step wants to run the freshly built test
            # binary at build time, which does not work for a cross build.
            ninjaFlags = [ "executor-${frontEnd}" ];

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp executor.exe $out/bin/
              ${pkgs.lib.concatMapStrings
                ({ src, dest ? "bin" }: ''
                  mkdir -p "$out/${dest}"
                  cp ${src} "$out/${dest}/"
                '')
                runtimeFiles}
              runHook postInstall
            '';
          };
      in {
        packages = {
          default = pkgs.stdenv.mkDerivation {
            name = "executor2000";
            nativeBuildInputs = with pkgs; [
              cmake
              bison
              perl
              ruby
              pkg-config
              qt6.wrapQtAppsHook
              ninja
            ];
            buildInputs =
              let
                patchedWaylandPP = pkgs.waylandpp.overrideAttrs (oldAttrs: {
                  postPatch = ''
                    # update wayland.xml and xdg-shell.xml to the versions used by the wayland and wayland-protocols packages
                    tar --wildcards -O -xf ${pkgs.buildPackages.wayland.src} 'wayland*/protocol/wayland.xml' > protocols/wayland.xml
                    cp -r ${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/*.xml  protocols/extra/
                  '';
                });
              in
              with pkgs;
              [ qt6.qtbase boost readline SDL2 ]
              ++ lib.optionals stdenv.isLinux [ SDL wayland patchedWaylandPP ];
            src = ./.;
            hardeningDisable = [ "all" ];
            cmakeFlags = [ "-DRUN_FIXUP_BUNDLE=NO" "-DNO_STATIC_BOOST=YES" ];
          };
          headless = pkgs.stdenv.mkDerivation {
            name = "executor2000";
            nativeBuildInputs = with pkgs; [ cmake bison perl ruby pkg-config ninja ];
            buildInputs = with pkgs;
              [ boost readline ];
            src = ./.;
            cmakeFlags = [ "-DFRONT_ENDS=headless" "-DNO_STATIC_BOOST=YES" ];
            hardeningDisable = [ "all" ];
          };
        }
        # The Wine-based cross build only works on an x86_64 Linux host.
        // pkgs.lib.optionalAttrs (system == "x86_64-linux") {
          windows-headless = mkWindowsPackage {
            frontEnd = "headless";
          };
          windows-sdl2 = mkWindowsPackage {
            frontEnd = "sdl2";
            buildInputs = [ winPkgs.SDL2 ];
            runtimeFiles = [ { src = "${winSdl3.out}/bin/SDL3.dll"; } ];
          };
            windows-qt6 = mkWindowsPackage {
              frontEnd = "qt";
              buildInputs = [ winQtBase ];
              extraCmakeFlags = [
                # moc/rcc/uic etc. must run on the build machine.
                "-DQT_HOST_PATH=${pkgs.qt6.qtbase}"
              ];
              runtimeFiles = [
                # Qt modules used by the front end.
                { src = "${winQtBase}/bin/Qt6Core.dll"; }
                { src = "${winQtBase}/bin/Qt6Gui.dll"; }
                # Their non-system dependencies.
                { src = "${winQtBase}/bin/libmcfgthread-2.dll"; }
                { src = "${winQtBase}/bin/libstdc++-6.dll"; }
                { src = "${winQtBase}/bin/libgcc_s_seh-1.dll"; }
                { src = "${winQtBase}/bin/libzstd.dll"; }
                { src = "${winQtBase}/bin/libb2-1.dll"; }
                { src = "${winQtBase}/bin/libpcre2-16-0.dll"; }
                { src = "${winQtBase}/bin/zlib1.dll"; }
                { src = "${winQtBase}/bin/libdouble-conversion.dll"; }
                { src = "${winQtBase}/bin/libpng16-16.dll"; }
                # Qt appends "platforms" to the application directory (which it
                # adds to its library paths), so these belong in <exe dir>/platforms.
                # qoffscreen/qminimal let the build be smoke-tested without a
                # display.
                { src = "${winQtBase}/lib/qt-6/plugins/platforms/qwindows.dll";
                  dest = "bin/platforms"; }
                { src = "${winQtBase}/lib/qt-6/plugins/platforms/qoffscreen.dll";
                  dest = "bin/platforms"; }
                { src = "${winQtBase}/lib/qt-6/plugins/platforms/qminimal.dll";
                  dest = "bin/platforms"; }
              ];
            };
            windows = self'.packages.windows-qt6;
        };

        devShells.default = pkgs.mkShell {
          inputsFrom = [ self'.packages.default ];
          packages = [ pkgs.tracy ];
          TRACY_SOURCE_DIR = "${pkgs.tracy.src}";
        };
      };
    };
}
