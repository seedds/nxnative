# The upstream release binary: nixpkgs' xcodegen is not in the binary cache for aarch64-darwin and builds SwiftPM from source
{
  lib,
  stdenvNoCC,
  fetchurl,
  unzip,
}:

stdenvNoCC.mkDerivation (finalAttrs: {
  pname = "xcodegen";
  version = "2.46.0";

  src = fetchurl {
    url = "https://github.com/yonaskolb/XcodeGen/releases/download/${finalAttrs.version}/xcodegen.zip";
    sha256 = "4d9e34b62172d645eed6457cac13fc222569974098ef4ee9c3368bedf0196806";
  };

  nativeBuildInputs = [ unzip ];
  sourceRoot = "xcodegen";
  dontConfigure = true;
  dontBuild = true;
  dontFixup = true; # stripping would void the signature

  # SettingPresets must stay at ../share/xcodegen beside the binary
  installPhase = ''
    runHook preInstall
    mkdir -p $out
    cp -R bin share $out/
    runHook postInstall
  '';

  meta = {
    homepage = "https://github.com/yonaskolb/XcodeGen";
    license = lib.licenses.mit;
    platforms = lib.platforms.darwin;
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    mainProgram = "xcodegen";
  };
})
