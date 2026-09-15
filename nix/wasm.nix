# logos-protocol COMPILED FOR wasm32, with the web transport as its only
# transport (slice 26).
#
# NOT A CROSS PACKAGE SET, and that is the shape decision worth stating. iOS,
# Android and Windows are nixpkgs cross targets: a `pkgs` whose hostPlatform is
# the phone, with a Qt and an OpenSSL built for it. Emscripten is not that. It
# brings its own sysroot, its own libc and its own libc++, and nothing in
# nixpkgs' wasm story builds Qt for it — so there is no target package set to
# ask for, and this is an ordinary NATIVE derivation that drives emcc.
#
# It follows that this cannot be a `packages.<pseudo-system>` entry the way
# `packages.aarch64-ios.bare` is; it is `packages.<buildSystem>.wasm`, produced
# on whatever machine runs it, and its output is the same bytes everywhere
# because emscripten's toolchain is the pin (logos-nix's nix/wasm/overlay.nix).
#
# WHAT COMES OUT: a static archive of wasm objects plus the headers a Wasm host
# outside this repo has to name. No CMake package config — see the note on the
# install in cpp/CMakeLists.txt.
{ pkgs, common, src }:

let
  # ── THE OUTBOUND DOOR, as a fact a CONSUMER can read before building ───────
  #
  # A wasm image can MAKE a call now, not merely serve one:
  # implementations/wasm/wasm_lp_abi.cpp defines lp_client_create,
  # lp_client_destroy and lp_invoke_async over the connection the host installs
  # (wasm_outbound_door.h). Before that, a module calling a dependency linked
  # against nothing and wasm-ld reported an undefined symbol forty lines into an
  # emcc command, naming neither the module nor the reason.
  #
  # logos-module-builder has to know that BEFORE it decides whether a module
  # with dependencies gets a `web` output at all (ADR 0009, gate 2), and "before"
  # means at EVAL time, off the package, without realising it. Hence a passthru
  # boolean rather than anything read out of the archive.
  #
  # A consumer reads it as `pkg.hasOutboundDoor or false`: a pin that predates
  # this attribute is a pin without the door, which is exactly what it means.
  hasOutboundDoor = true;

  # The symbols the claim is about, named once and checked below.
  #
  # lp_invoke — the SYNCHRONOUS twin — is NOT among them and is asserted ABSENT
  # further down. A Worker is one event loop and this image has no ASYNCIFY
  # (ADR 0004), so a call that blocked for its reply would deadlock the loop
  # that delivers it; the sync spelling is refused by being missing, which turns
  # a module that uses it into a build failure instead of a phone failure.
  outboundDoorSymbols = [ "lp_client_create" "lp_client_destroy" "lp_invoke_async" ];

  # The claim as the installCheck's shell sees it, so the check compares against
  # the same word it prints.
  doorClaim = pkgs.lib.boolToString hasOutboundDoor;
in

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-wasm";
  version = common.version;

  # Read at EVAL time by logos-module-builder's `web` gate.
  passthru = { inherit hasOutboundDoor; };

  inherit src;

  nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
  # Header-only, and the only dependency the subset has. It is a BUILD input
  # rather than a target one for the same reason: there is nothing in it to
  # compile for wasm32.
  buildInputs = [ pkgs.nlohmann_json ];

  dontUseCmakeConfigure = true;
  # No Qt in this derivation at all, so qtbase's setup hook never runs and
  # nothing would wrap. Set anyway: `common.nativeBuildInputs` is deliberately
  # NOT used here and a future editor reaching for it would trip qtPreHook.
  dontWrapQtApps = true;

  # A wasm object carries no fixup-able load commands and `strip` on Darwin
  # cannot read one.
  dontStrip = true;
  dontFixup = true;

  buildPhase = ''
    runHook preBuild

    ${pkgs.logosEmscriptenSetup}

    mkdir -p build-wasm
    cd build-wasm

    # CMAKE_FIND_ROOT_PATH as well as CMAKE_PREFIX_PATH: the Emscripten
    # toolchain file re-roots find_package at its own sysroot, so a store path
    # named only in the prefix path is not searched. Same rule, same reason, as
    # the mobile Bare builds in logos-module-builder.
    cmake ../cpp -GNinja ${pkgs.lib.escapeShellArgs pkgs.logosWasmCmakeFlags} \
      -DLOGOS_PROTOCOL_WASM_ONLY=ON \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=$out \
      -DCMAKE_PREFIX_PATH=${pkgs.nlohmann_json} \
      -DCMAKE_FIND_ROOT_PATH=${pkgs.nlohmann_json}
    ninja
    cd ..

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    cmake --install build-wasm
    runHook postInstall
  '';

  # ── the gate ───────────────────────────────────────────────────────────────
  #
  # Two claims, and neither is implied by the build succeeding.
  #
  #   1. THESE ARE WASM OBJECTS. A toolchain file that failed to take leaves a
  #      cmake build that configures, compiles and installs a perfectly good
  #      NATIVE archive — the exact failure mode logos-nix's emscripten-pin
  #      check exists for one level down, and it is worth re-asserting on the
  #      shipped bytes.
  #   2. NOTHING QT, BOOST OR OPENSSL SURVIVED. The subset is a source LIST, and
  #      a list is edited. An added file that pulls in QString would fail the
  #      native protocol_wasm_tests link first — but only if someone ran it, and
  #      only on a platform that has Qt to fail against. Reading the undefined
  #      symbols off the archive is the claim stated where the artifact is.
  #
  # doInstallCheck rather than postFixup: dontFixup is set above, and this is a
  # native derivation, so nixpkgs does not switch the phase off the way it does
  # under cross.
  doInstallCheck = true;
  installCheckPhase = ''
    runHook preInstallCheck

    archive=$out/lib/liblogos_protocol_wasm.a
    test -f "$archive" || { echo "no archive at $archive"; exit 1; }

    nm=${pkgs.logosEmscriptenLlvm}/bin/llvm-nm

    # `llvm-nm` refuses a whole archive of the wrong format, so a native archive
    # fails here loudly. Ask for the format explicitly as well.
    if ! "$nm" --print-file-name "$archive" > syms.txt 2> nm.err; then
      echo "llvm-nm could not read $archive as a wasm archive:"
      cat nm.err
      exit 1
    fi

    # Every member is wasm. `llvm-nm --print-file-name` names each object it
    # read; the format check is that llvm-nm succeeded above plus this magic
    # test on the first member.
    ${pkgs.logosEmscriptenLlvm}/bin/llvm-ar x "$archive" --output=. 2>/dev/null \
      || ${pkgs.logosEmscriptenLlvm}/bin/llvm-ar x "$archive"
    for o in *.o; do
      head -c 4 "$o" | od -An -c | grep -q '\\0   a   s   m' \
        || { echo "$o is not a wasm object"; exit 1; }
    done

    # The forbidden neighbours, by symbol. Qt mangles as `<len>Q<Upper>` inside
    # an Itanium name (anchored to a length digit, as the Bare gate learned to
    # do), Boost and OpenSSL are plain prefixes.
    if grep -Eq '[0-9]Q[A-Z][A-Za-z0-9_]|_ZN5boost|(^| )SSL_|(^| )EVP_' syms.txt; then
      echo "the wasm subset reached a forbidden dependency:"
      grep -E '[0-9]Q[A-Z][A-Za-z0-9_]|_ZN5boost|(^| )SSL_|(^| )EVP_' syms.txt | head -20
      exit 1
    fi

    # ...and it really did compile something: the doors a module's generated
    # glue calls have to be DEFINED in here, or a Wasm host would link against
    # an empty archive and fail three steps later.
    for sym in lp_token_save lp_token_save_inbound lp_token_get lp_token_keys \
               lp_grant_host_services lp_string_free lp_protocol_version; do
      grep -Eq "[TD] $sym\$" syms.txt \
        || { echo "$sym is not defined in the wasm archive"; exit 1; }
    done

    # ── ...and the outbound-door claim is not allowed to go stale ───────────
    #
    # `hasOutboundDoor` is a hand-written boolean read at EVAL time, so nothing
    # forces it to describe these bytes. Both directions are asserted here:
    # claiming a door the archive does not define would open a consumer's gate
    # onto a link failure, and NOT claiming one it does define would keep every
    # dependent module's `web` output switched off after the door landed, with
    # no diagnostic anywhere. Whoever adds the client side is told, by name,
    # that the flag above is the other half of the change.
    for sym in ${pkgs.lib.concatStringsSep " " outboundDoorSymbols}; do
      if grep -Eq "[TDW] $sym\$" syms.txt; then defined=true; else defined=false; fi
      if [ "$defined" != "${doorClaim}" ]; then
        echo "logos-protocol wasm: hasOutboundDoor is ${doorClaim}," \
             "but $sym is defined=$defined in the archive."
        echo "  The flag is read at EVAL time by logos-module-builder's \`web\` gate"
        echo "  (ADR 0009): a module with dependencies gets a \`web\` output only when"
        echo "  the pinned protocol can make an outbound call. Flip"
        echo "  hasOutboundDoor in nix/wasm.nix to match these bytes."
        exit 1
      fi
    done

    # ── ...and the SYNC twin stays absent ───────────────────────────────────
    #
    # The other half of the door's decision, and the one nothing else would
    # notice. Defining lp_invoke — even as an always-error stub — lets a module
    # that calls a synchronous dependency wrapper LINK, and it then deadlocks or
    # errors on a phone instead of failing on the machine that built it. The
    # absence is the diagnostic, so it is asserted rather than assumed.
    if grep -Eq "[TDW] lp_invoke\$" syms.txt; then
      echo "logos-protocol wasm: lp_invoke is DEFINED in the archive."
      echo "  It must not be. A wasm image is a single event loop without"
      echo "  ASYNCIFY (ADR 0004): a synchronous call would block the loop that"
      echo "  delivers its reply. Leaving the symbol undefined is what makes a"
      echo "  module calling the sync twin fail to BUILD, naming it. Use"
      echo "  lp_invoke_async."
      exit 1
    fi

    echo "logos-protocol wasm subset: $(wc -l < syms.txt) symbols," \
         "outbound door ${doorClaim}, sync lp_invoke absent, gate OK"
    runHook postInstallCheck
  '';

  meta = with pkgs.lib; {
    description = "${common.meta.description} (wasm32, web transport only)";
    platforms = platforms.unix;
  };
}
