{
  description = "wheel-hid-emulator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs";
  };

  outputs = { self, nixpkgs }:
  let
    system = "x86_64-linux";
    pkgs = import nixpkgs { inherit system; };
  in {
    packages.${system}.wheel-hid-emulator = pkgs.stdenv.mkDerivation {
      pname = "wheel-hid-emulator";
      version = "main";

      src = ./.;

      nativeBuildInputs = [ pkgs.pkg-config ];
      buildInputs = [ pkgs.hidapi ];

      buildPhase = "make";

      installPhase = ''
        mkdir -p $out/bin
        install -m755 wheel-emulator $out/bin/
      '';
    };

    defaultPackage.${system} = packages.${system}.wheel-hid-emulator;
  };
}
