# fmodex-midi

A drop-in replacement `fmodex.dll` for 32-bit applications built on **FMOD Ex 4.32.1**. It sends their MIDI music to a real MIDI synth instead of FMOD's built-in software synth:

- **OmniMIDI** through **KDMAPI**, for very low latency, if it's installed;
- otherwise the **Windows MIDI mapper** (winmm), or a MIDI out device you choose.

Everything else (WAV, OGG, MP3, effects, 3D) goes straight to the original FMOD, untouched. If anything goes wrong (no MIDI device, a file it can't parse, reverse playback), that sound just plays through FMOD as before.

> [!WARNING]
> **Buyer beware.** This is built and tested against **FMOD Ex 4.32.1 (32-bit)** only. It may not work with your application or your FMOD version. Every application uses FMOD differently, and other FMOD Ex releases export different functions.
>
> - The installer compares your `fmodex.dll`'s exports with the tested build. It refuses ones that look incompatible (override with `-Force` at your own risk), and installs compatible-but-untested ones with a warning.
> - It's fully reversible: `-Action uninstall` puts your original `fmodex.dll` back.
> - **If it doesn't work for you, [open an issue](../../issues/new?template=compatibility.yml).** Include a link to the application (download or store page), your `fmodex.dll` version (the installer's `-Action status` prints it) and what happened. A log helps a lot: set `log_file=fmodex-midi.log` in `fmodex-midi.ini`.

## Install

1. Download `fmodex-midi.ps1` from the [latest release](../../releases/latest).
2. Close the application and copy the script into the folder that contains its `fmodex.dll`.
3. Right-click the script and choose **Run with PowerShell**.

From a prompt, for any folder:

```powershell
powershell -ExecutionPolicy Bypass -File fmodex-midi.ps1 -Path "C:\Some App"            # install
powershell -ExecutionPolicy Bypass -File fmodex-midi.ps1 -Path "C:\Some App" -Action status
powershell -ExecutionPolicy Bypass -File fmodex-midi.ps1 -Path "C:\Some App" -Action uninstall
```

- The installer renames the original to `fmodex_real.dll` and puts the replacement in its place. Uninstall restores the original.
- It checks the original's export table first (see the warning above).
- It refuses while the application is running.
- Folders under Program Files need an elevated PowerShell.

**Manual install:** each release also has the bare `fmodex.dll`. Rename the application's original `fmodex.dll` to `fmodex_real.dll` (it must stay there), drop ours in, and optionally add `fmodex-midi.ini`. To undo, delete ours and rename the original back.

For KDMAPI, install [OmniMIDI](https://github.com/KaleidonKep99/OmniMIDI) (its 32-bit part) and give it a SoundFont.

## Configure

`fmodex-midi.ini` sits next to the DLL. It's created on install, and every key is optional:

```ini
[midi]
enabled=1
backend=auto        ; auto | kdmapi | winmm | log | off
winmm_device=mapper ; mapper | <index> | <part of the device name>
volume_curve=gm     ; gm (cc * sqrt(volume)) | linear
log_file=           ; diagnostic log, empty = off
log_level=info      ; info | trace
event_log=          ; output of backend=log (testing)
```

## How it works

The replacement forwards all 702 FMOD Ex exports to `fmodex_real.dll` and intercepts 26 C++ member functions. When a host creates a sound from in-memory data that is a Standard MIDI File, fmodex-midi parses it itself. When the host plays it, FMOD keeps playing the channel muted, so the host's end-of-sound callbacks, `isPlaying`, channel reuse and the rest stay exactly as before. Meanwhile, fmodex-midi's own sequencer plays the notes to the synth and mirrors pause, stop, volume, mute, frequency (as tempo), position and looping. See [`docs/design.md`](docs/design.md).

## Build

On NixOS or anything with Nix (flakes):

```sh
nix develop
make            # build/fmodex.dll + build/midiplay.exe (standalone player)
make check      # export set, forwarders, import whitelist, stdcall sizes
make test       # native tests: parser vs mido, player, fuzz smoke
make dist       # build/fmodex-midi.ps1, the one-click installer
make release    # build/release/: source zip, fmodex.dll, installer, SHA256SUMS, notes
test/installer/run.sh  # installer verdicts vs fake FMOD DLLs (needs pwsh)
test/integ/run.sh   # proxy + mock FMOD + harness, under Wine
test/kdmapi/run.sh  # KDMAPI backend against a mock OmniMIDI, under Wine
test/fuzz.sh 600 8  # libFuzzer, 10 minutes, 8 workers
```

CI (`.github/workflows/build.yml`) runs the checks and tests and `make release` on every push, and uploads the assets. Pushing a `v*` tag publishes a GitHub release with the source zip, `fmodex.dll`, `fmodex-midi.ps1` and `SHA256SUMS`.

## License

MIT for this project's own code. See `LICENSE` and, for third-party material in `references/`, `NOTICE`.
