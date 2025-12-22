{
  description = "wheel-hid-emulator";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }:
    let pkgsFor = system: import nixpkgs { inherit system; };
    in {
      packages.x86_64-linux.wheel-hid-emulator = (pkgsFor "x86_64-linux").stdenv.mkDerivation {
        pname = "wheel-hid-emulator";
        version = builtins.getEnv "GIT_COMMIT";
        src = ./.;
        nativeBuildInputs = [ (pkgsFor "x86_64-linux").pkg-config ];
        buildInputs = [ (pkgsFor "x86_64-linux").hidapi ];
        buildPhase = "make";
        installPhase = ''
          mkdir -p $out/bin
          install -Dm755 wheel-emulator $out/bin/wheel-emulator
        '';
        meta = {
          license = (pkgsFor "x86_64-linux").lib.licenses.gpl2Only;
          description = "wheel-hid-emulator";
        };
      };
      defaultPackage.x86_64-linux = self.packages.x86_64-linux.wheel-hid-emulator;
      devShells.x86_64-linux.default = (pkgsFor "x86_64-linux").mkShell { buildInputs = [ (pkgsFor "x86_64-linux").pkg-config ]; };
    };
}
