{
  description = "nxnative development shells for Apple silicon Macs";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      systems = [ "aarch64-darwin" ];
      eachSystem = f: lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Read from the workflow so the LLVM release is pinned in one place
      workflowEnv =
        file: key:
        let
          m = lib.findFirst (x: x != null) null (
            map (builtins.match " *${key}: *([^ #]+).*") (lib.splitString "\n" (builtins.readFile file))
          );
        in
        if m == null then throw "${baseNameOf (toString file)}: no ${key} line" else builtins.head m;
      lintC = ./.github/workflows/lint-c.yml;

      # atheris 3.1.0 has no macOS wheel and no sdist on PyPI
      atheris = "git+https://github.com/google/atheris@e36da74cf42b5834c4e31d7cabb58d796695d29f";
    in
    {
      packages = eachSystem (pkgs: {
        xcodegen = pkgs.callPackage ./dm/nix/xcodegen.nix { };
        llvm = pkgs.callPackage ./dm/nix/llvm.nix {
          version = workflowEnv lintC "LLVM_VERSION";
          sha256 = workflowEnv lintC "LLVM_SHA256";
        };
      });

      formatter = eachSystem (pkgs: pkgs.nixfmt-tree);

      devShells = eachSystem (
        pkgs:
        let
          own = self.packages.${pkgs.stdenv.hostPlatform.system};
          python = pkgs.python314;
          dotnet = pkgs.dotnetCorePackages.sdk_10_0;
          build = [
            python
            dotnet
            own.xcodegen
            pkgs.zstd
            pkgs.glslang
            pkgs.spirv-tools
          ];
          # The SDK wrapper does not set it, and the translator's apphost cannot find the runtime without it
          dotnetRoot = {
            DOTNET_ROOT = "${dotnet.unwrapped}/share/dotnet";
          };

          # mkShellNoCC: Nix's cc wrapper and SDK would replace Xcode's clang
          shell =
            name:
            {
              tools,
              requirements,
              env ? { },
            }:
            pkgs.mkShellNoCC (
              {
                name = "nxnative-${name}";
                packages = tools;
                NXNATIVE_SHELL = name;
                NXNATIVE_TOOLS = lib.makeBinPath tools;
                NXNATIVE_STDENV_PATH = lib.makeBinPath pkgs.stdenvNoCC.initialPath;
                NXNATIVE_PYTHON = python.interpreter;
                NXNATIVE_REQUIREMENTS = lib.concatMapStringsSep " " (r: ".github/requirements/${r}") requirements;
                shellHook = "source ${./dm/nix/shell_hook.sh}";
              }
              // env
            );
        in
        {
          default = shell "default" {
            tools = build;
            requirements = [ "tools.txt" ];
            env = dotnetRoot;
          };

          coverage = shell "coverage" {
            tools = build;
            requirements = [ "coverage.txt" ];
            env = dotnetRoot;
          };

          lint = shell "lint" {
            tools = [
              python
              pkgs.nodejs
            ];
            requirements = [ "lint.txt" ];
            env = {
              LLVM = "${own.llvm}";
            };
          };

          # Apple's clang has no libFuzzer
          fuzz = shell "fuzz" {
            tools = [ python ];
            requirements = [ "fuzz.txt" ];
            env = {
              NXNATIVE_PIP_SKIP = "atheris";
              NXNATIVE_PIP_EXTRA = atheris;
              NXNATIVE_PIP_ENV = "CLANG_BIN=${own.llvm}/bin/clang CC=${own.llvm}/bin/clang CXX=${own.llvm}/bin/clang++";
            };
          };
        }
      );
    };
}
