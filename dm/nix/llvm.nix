# The release lint-c.yml downloads, not nixpkgs' clang-tools: the C lint baseline depends on the exact clang-tidy version
{
  lib,
  stdenvNoCC,
  fetchurl,
  zstd,
  version,
  sha256,
}:

let
  name = "LLVM-${version}-macOS-ARM64";
  major = lib.versions.major version;
in
stdenvNoCC.mkDerivation {
  pname = "llvm-release";
  inherit version;

  src = fetchurl {
    url = "https://github.com/llvm/llvm-project/releases/download/llvmorg-${version}/${name}.tar.zst";
    inherit sha256;
  };

  nativeBuildInputs = [ zstd ];
  dontUnpack = true;
  dontFixup = true;

  # The archive needs --long=30 (1 GB window). lib/clang/<major> holds the headers clang-tidy reads and libFuzzer
  installPhase = ''
    runHook preInstall
    mkdir -p $out
    zstd -dc --long=30 $src | tar -x -C $out --strip-components 1 \
      ${name}/bin/clang-tidy ${name}/bin/clang-${major} ${name}/bin/clang ${name}/bin/clang++ ${name}/lib/clang/${major}
    runHook postInstall
  '';

  meta = {
    homepage = "https://github.com/llvm/llvm-project/releases";
    license = with lib.licenses; [
      asl20
      llvm-exception
    ];
    platforms = [ "aarch64-darwin" ];
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
  };
}
