{
  description = "fmodex-midi: drop-in fmodex.dll that sends FMOD Ex MIDI playback to KDMAPI/winmm";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      mingw = pkgs.pkgsCross.mingw32;
      py = pkgs.python3.withPackages (p: [ p.mido ]);
    in {
      # `nix develop`: $CC is i686-w64-mingw32-gcc; `cc` is the native host compiler
      # (used for host-side parser/sequencer tests and fuzzing).
      devShells.${system}.default = mingw.mkShell {
        buildInputs = [ mingw.windows.mcfgthreads ];
        nativeBuildInputs = [ py pkgs.gnumake pkgs.binutils pkgs.gcc ];
      };
    };
}
