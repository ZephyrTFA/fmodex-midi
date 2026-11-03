# fmodex-midi

A drop-in replacement `fmodex.dll` (FMOD Ex 4.32.1, x86) that sends a host application's MIDI playback to KDMAPI (OmniMIDI) or winmm, and forwards everything else to the original DLL, which is renamed `fmodex_real.dll`. Start with `README.md` and `docs/design.md`.

## Rules

- This repo is meant to be **publishable**. Never commit third-party SDK files or docs (e.g. FMOD headers), secrets, or binaries (DLLs, EXEs, VM images, SoundFonts). Build outputs go to `build/` (gitignored).
- Keep it **host-agnostic**: no names of specific host applications in code, docs, tests, commit messages or GitHub metadata. Host-specific test material (and the FMOD headers kept for reference) lives in a separate private companion repo checked out next to this one.
- `src/fmod_min.h` is our own minimal subset of the public FMOD Ex API; the build uses no FMOD SDK files.
- It's "buyer beware": the installer gates on export-table compatibility, not on a specific DLL hash. Keep it that way, and keep the issue link (`ISSUES_URL`, stamped at build time) working.

## Build, test, ship

All inside `nix develop` (flake devShell: mingw32 cross + native gcc + python/mido):

- `make`: `build/fmodex.dll` (the proxy) and `build/midiplay.exe` (standalone player).
- `make check`: export set vs `references/fmodex-4.32.1/exports.txt`, forwarders, import whitelist, stdcall sizes vs mangled names.
- `make test`: native parser-vs-mido, player unit tests, fuzz smoke (ASan/UBSan).
- `test/integ/run.sh`: proxy + mock FMOD + host-like harness, 11 scenarios, under Wine. `RUNNER=vm VM_RUN=<helper>` runs them in a Windows VM.
- `test/kdmapi/run.sh`: KDMAPI backend vs a mock OmniMIDI under Wine. `test/fuzz.sh [secs] [jobs]`: libFuzzer.
- `make dist`: `build/fmodex-midi.ps1`, the self-contained one-click installer (proxy embedded as base64). `test/installer/run.sh` checks its compatibility verdicts against fake FMOD DLLs (needs `pwsh`; locally use `PWSH=$(nix build --no-link --print-out-paths nixpkgs#powershell)/bin/pwsh`).
- `make release` → `build/release/`: `fmodex-midi-<ver>-src.zip` (git archive of HEAD), `fmodex.dll` (manual install), `fmodex-midi.ps1`, `SHA256SUMS`, `NOTES.md` (from `tools/release-notes.md.in`). CI runs it on every push; a `v*` tag publishes those as a GitHub release.

## Notes

- On this NixOS box, run anything missing with `nix shell nixpkgs#<pkg>`. Run 32-bit Windows binaries with `nix shell nixpkgs#wineWow64Packages.stable -c wine` (default prefix `~/.cache/fmodex-midi-wine`).
- Real-host verification uses a QEMU/KVM Windows VM. Its tooling is in the companion repo. Keep VM images outside both repos.
- Hosts call the MSVC-mangled C++ API (`__stdcall`, `this` on the stack), so the mangled names are what get hooked (`src/hooks.txt` → `hk_<Class>_<method>`).
