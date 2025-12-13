{ pkgs ? import <nixpkgs> {} }:

{
  wheel-hid-emulator = pkgs.stdenv.mkDerivation {
    pname = "wheel-hid-emulator";
    version = "local";
    src = ./.;
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [ pkgs.hidapi ];
    buildPhase = "make";
    installPhase = ''
      mkdir -p $out/bin
      install -m755 wheel-emulator $out/bin/
    '';
  };
}

