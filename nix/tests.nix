# Builds and runs the test suite
{ pkgs, common, src }:

pkgs.stdenv.mkDerivation {
  pname = "${common.pname}-tests";
  version = common.version;

  inherit src;

  nativeBuildInputs = common.nativeBuildInputs;
  buildInputs = common.buildInputs ++ [ pkgs.gtest ];
  cmakeFlags = common.cmakeFlags;

  dontUseCmakeConfigure = true;

  buildPhase = ''
    runHook preBuild

    mkdir -p build-tests
    cd build-tests
    cmake ../tests -GNinja $cmakeFlags
    ninja
    cd ..

    runHook postBuild
  '';

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    cd build-tests
    export QT_QPA_PLATFORM=offscreen
    ctest --output-on-failure
    cd ..
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall

    mkdir -p $out/bin
    cp build-tests/protocol/protocol_tests $out/bin/
    # The Qt-LOOP-FREE half. A separate binary because "this process has no
    # QCoreApplication" is a property of the process, and protocol_tests' main()
    # constructs one before the first test runs. See tests/protocol/CMakeLists.txt.
    cp build-tests/protocol/protocol_noqt_tests $out/bin/
    # The wasm subset linked ALONE, natively. Its value is as much in the LINK
    # as in the cases: nothing but nlohmann_json and gtest is on its line, so a
    # Qt or Boost include creeping into LOGOS_PROTOCOL_WASM_SOURCES fails here
    # rather than in the emcc build. See tests/protocol/CMakeLists.txt.
    cp build-tests/protocol/protocol_wasm_tests $out/bin/

    runHook postInstall
  '';

  inherit (common) meta;
}
