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
    packages.${system}.wheel-hid-emulator =
      pkgs.stdenv.mkDerivation {
        pname = "wheel-hid-emulator";
        version = "main";

        src = pkgs.fetchFromGitHub {
          owner = "dewdgi";
          repo = "wheel-hid-emulator";
          rev = "main";
          sha256 = "LObm+drPwczHEYshGHr2BZORbgODKG6aJ0ytY5t+Hwg=";
        };

        nativeBuildInputs = [ pkgs.pkg-config ];
        buildInputs = [ pkgs.hidapi ];

        buildPhase = "make";

        installPhase = ''
          mkdir -p $out/bin
          install -m755 wheel-emulator $out/bin/
        '';
      };
  };
}
