{
  description = "Executor 2000 - classic mac emulator";

  inputs = { nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11"; };

  outputs = inputs@{ flake-parts, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems =
        [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      perSystem = { config, self', inputs', pkgs, system, ... }: {
        packages.default = pkgs.stdenv.mkDerivation {
          name = "executor2000";
          nativeBuildInputs = with pkgs; [
            cmake
            bison
            perl
            ruby
            pkg-config
            qt5.wrapQtAppsHook
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
            [ qt5.qtbase boost readline SDL2 ]
            ++ lib.optionals stdenv.isLinux [ SDL wayland patchedWaylandPP ]
            ++ lib.optionals stdenv.isDarwin
              (with darwin.apple_sdk.frameworks; [ Carbon Cocoa ]);
          src = ./.;
          hardeningDisable = [ "all" ];
          cmakeFlags = [ "-DRUN_FIXUP_BUNDLE=NO" "-DNO_STATIC_BOOST=YES" ];
        };
        packages.headless = pkgs.stdenv.mkDerivation {
          name = "executor2000";
          nativeBuildInputs = with pkgs; [ cmake bison perl ruby pkg-config ];
          buildInputs = with pkgs;
            [ boost readline ] ++ lib.optionals stdenv.isDarwin
              (with darwin.apple_sdk.frameworks; [ Carbon Cocoa ]);
          src = ./.;
          cmakeFlags = [ "-DFRONT_ENDS=headless" "-DNO_STATIC_BOOST=YES" ];
          hardeningDisable = [ "all" ];
        };
      };
    };
}
