#!/usr/bin/env bash
# KDMAPI backend test without OmniMIDI: mock OmniMIDI.dll in the Wine
# prefix's syswow64 (what LOAD_LIBRARY_SEARCH_SYSTEM32 finds), midiplay
# --backend kdmapi; its stream must equal the log backend's, with SysEx
# sent prepared. Run inside `nix develop`.
set -euo pipefail
cd "$(dirname "$0")/../.."
export WINEPREFIX=${WINEPREFIX:-$HOME/.cache/fmodex-midi-wine} WINEDEBUG=-all
W="nix shell nixpkgs#wineWow64Packages.stable -c"
mkdir -p build/run
$CC -shared -O2 -Wall -static-libgcc -o build/OmniMIDI.dll test/kdmapi/mock_omnimidi.c test/kdmapi/mock_omnimidi.def -lwinmm
make -s midiplay
SYSWOW=$WINEPREFIX/drive_c/windows/syswow64
cp build/OmniMIDI.dll "$SYSWOW/OmniMIDI.dll"
trap 'rm -f "$SYSWOW/OmniMIDI.dll"' EXIT
$W wine build/midiplay.exe test/midi/sysex_rpn.mid --backend kdmapi 2>/dev/null
$W wine build/midiplay.exe test/midi/sysex_rpn.mid --backend log --out build/run/kd-ref.events 2>/dev/null
TMPD=$(ls -d "$WINEPREFIX"/drive_c/users/*/AppData/Local/Temp | head -1)
tr -d "\r" < "$TMPD/omni-mock.log" > build/run/omni-mock.log
L=build/run/omni-mock.log
grep -q '^ERROR' "$L" && { grep '^ERROR' "$L"; exit 1; }
grep -A2 '^PrepareLongData' "$L" | grep -q '^UnprepareLongData' || { echo "SysEx not prepare/send/unprepare"; exit 1; }
tail -2 "$L" | tr '\n' ' ' | grep -q 'ResetKDMAPIStream TerminateKDMAPIStream' || { echo "missing reset/terminate"; exit 1; }
diff <(grep -vE '^(Is|Init|Term|Reset|Prepare|Unprepare)' "$L") <(grep -v "^#" build/run/kd-ref.events | tr -d "\r" | cut -d" " -f2-)
echo "kdmapi: ok (stream identical to log backend, SysEx prepared, clean teardown)"
