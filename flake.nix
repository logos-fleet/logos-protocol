{
  description = "Logos Protocol - transports, token exchange and the language-neutral lp_* C ABI";

  inputs.logos-nix.url = "github:logos-co/logos-nix";
  inputs.nixpkgs.follows = "logos-nix/nixpkgs";

  outputs = { self, nixpkgs, logos-nix }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # logos-nix's overlays, not a bare `import nixpkgs`. `packages` has always
      # had them (forAllTargets applies them); `checks` did not, so the wasm
      # check could not see the Emscripten pin -- which is an attribute the
      # overlay adds. Naming lib.nativeOverlays rather than the individual
      # entries is what logos-nix's own drift guard asks consumers to do.
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; overlays = logos-nix.lib.nativeOverlays; };
      });

      # Adds the "x86_64-windows" pseudo-system. A cross derivation's `system`
      # attribute is its BUILD platform, so packages.x86_64-windows.* evaluates
      # anywhere but realises on x86_64-linux.
      forAllTargets = logos-nix.lib.forAllTargets;
    in
    {
      packages = forAllTargets ({ pkgs, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;

          lib = import ./nix/lib.nix { inherit pkgs common src; };
          include = import ./nix/include.nix { inherit pkgs common src; };
          tests = import ./nix/tests.nix { inherit pkgs common src; };

          # The module-impl C ABI as data, for the language backends to check
          # themselves against. See nix/module-impl-abi.nix.
          module-impl-abi = import ./nix/module-impl-abi.nix { inherit pkgs common src; };

          # logos-protocol for wasm32: the web transport as the only transport,
          # no Qt, no Boost, no OpenSSL. Reached from a native package set --
          # emscripten is a toolchain, not a nixpkgs cross target. See
          # nix/wasm.nix.
          wasm = import ./nix/wasm.nix { inherit pkgs common src; };

          # The qt-host/protocol pairing rule, for consumers to run against
          # their own closure. See nix/abi-closure-check.nix.
          abi-closure-check = import ./nix/abi-closure-check.nix { inherit pkgs common src; };

          # Combined package: static lib + cmake config + source-export
          # headers. propagatedBuildInputs re-declared on the join because
          # symlinkJoin doesn't forward propagation from `paths`. Qt is
          # excluded for the same setup-hook ordering reason as in
          # nix/default.nix; consumers list qt6.qtbase +
          # qt6.wrapQtAppsNoGuiHook themselves.
          protocol = pkgs.symlinkJoin {
            name = "logos-protocol";
            paths = [ lib include ];
            propagatedBuildInputs = common.propagatedBuildInputs;
          };
        in
        {
          logos-protocol-lib = lib;
          logos-protocol-include = include;
          inherit tests module-impl-abi abi-closure-check;

          logos-protocol = protocol;
          default = protocol;
        }
        # WASM IS NATIVE-ONLY here, and the guard is why. forAllTargets also
        # yields the "x86_64-windows" pseudo-system, whose cross package set
        # never sees logos-nix's native overlays and therefore has no
        # logosEmscriptenSetup -- naming it there is an EVAL failure for the
        # whole attribute set, not a missing package. There is nothing to lose:
        # a wasm artifact does not depend on which machine emitted it.
        // nixpkgs.lib.optionalAttrs (pkgs ? logosEmscriptenSetup) {
          logos-protocol-wasm = wasm;
        }
      );

      checks = forAllSystems ({ pkgs, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs; };
          src = ./.;
          tests = import ./nix/tests.nix { inherit pkgs common src; };
          module-impl-abi = import ./nix/module-impl-abi.nix { inherit pkgs common src; };
          abi-closure-check = import ./nix/abi-closure-check.nix { inherit pkgs common src; };
        in
        {
          inherit tests;
          # Proves the ABI manifest every backend checks itself against can
          # still fail. See nix/tests-module-impl-abi.nix.
          module-impl-abi-tests = import ./nix/tests-module-impl-abi.nix {
            inherit pkgs common src module-impl-abi;
          };

          # Proves the qt-host/protocol pairing rule can still fail.
          # See nix/tests-abi-closure-check.nix.
          abi-closure-check-tests = import ./nix/tests-abi-closure-check.nix {
            inherit pkgs common src abi-closure-check;
          };

          # The wasm subset, built by emcc and gated on the shipped bytes (no
          # Qt/Boost/OpenSSL symbol, every object a wasm object, every lp_* door
          # the generated module glue calls actually defined). Its NATIVE twin
          # -- the same source list linked alone -- is a case inside `tests`
          # above; this one is the artifact.
          wasm = import ./nix/wasm.nix { inherit pkgs common src; };
        }
      );

      devShells = forAllSystems ({ pkgs, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qtremoteobjects
            pkgs.gtest
            pkgs.boost
            pkgs.openssl
            pkgs.nlohmann_json
          ];
        };
      });
    };
}
