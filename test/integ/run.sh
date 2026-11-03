#!/usr/bin/env bash
# Integration tests: proxy fmodex.dll + mock fmodex_real.dll + harness.exe,
# log backend. Runs under Wine by default. RUNNER=vm runs them in a Windows VM
# through $VM_RUN, a helper with `ssh <cmd>` and `scp <src> <dst>` subcommands
# (vm:<path> = guest path), e.g. the companion repo's tools/vm/run.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."
D=build/integ
SCENARIOS=${SCENARIOS:-"basic pause stop cutover reverse corrupt wav master loop seek backendfail"}
make -s proxy
mkdir -p "$D"
$CC -shared -O2 -o "$D/fmodex_real.dll" test/integ/mock_fmod.c test/integ/mock_fmod.def -static-libgcc
$CC -O2 -Wall -o "$D/harness.exe" test/integ/harness.c -static-libgcc
cp build/fmodex.dll "$D/"
cp test/midi/one_single_piano_note.mid test/midi/tempo_changes.mid "$D/"

fail=0
run_one() {
    local sc=$1
    rm -f "$D/events.log" "$D/proxy.log"
    local ev=events.log
    [[ $sc == backendfail ]] && ev=   # log backend without a path: open fails
    printf '[midi]\nbackend=log\nevent_log=%s\nlog_file=proxy.log\nlog_level=info\n' "$ev" > "$D/fmodex-midi.ini"
    [[ ${RUNNER:-wine} == vm ]] && "$VM_RUN" scp "$D/fmodex-midi.ini" vm:C:/work/integ/ >/dev/null
    case ${RUNNER:-wine} in
    wine)
        (cd "$D" && WINEDEBUG=-all WINEPREFIX=${WINEPREFIX:-$HOME/.cache/fmodex-midi-wine} \
            nix shell nixpkgs#wineWow64Packages.stable -c wine harness.exe "$sc" one_single_piano_note.mid tempo_changes.mid 2>/dev/null) ;;
    vm)
        "$VM_RUN" ssh "Set-Location C:\\work\\integ; Remove-Item -ErrorAction SilentlyContinue events.log,proxy.log; .\\harness.exe $sc one_single_piano_note.mid tempo_changes.mid" | tr -d '\r'
        "$VM_RUN" scp vm:C:/work/integ/events.log "$D/events.log" >/dev/null 2>&1 || true
        "$VM_RUN" scp vm:C:/work/integ/proxy.log "$D/proxy.log" >/dev/null 2>&1 || true ;;
    esac
    cp "$D/proxy.log" "$D/$sc.proxy.log" 2>/dev/null || true
    if [[ -f $D/events.log ]]; then
        cp "$D/events.log" "$D/$sc.events"
        python3 test/integ/check.py "$sc" "$D/$sc.events" || fail=1
    else
        : > "$D/$sc.events"   # engine never started (no MIDI seen)
        python3 test/integ/check.py "$sc" "$D/$sc.events" || fail=1
    fi
}
if [[ ${RUNNER:-wine} == vm ]]; then
    : "${VM_RUN:?RUNNER=vm needs VM_RUN (path to the VM helper script)}"
    "$VM_RUN" ssh 'New-Item -Force -ItemType Directory C:\work\integ | Out-Null'
    for f in fmodex.dll fmodex_real.dll harness.exe one_single_piano_note.mid tempo_changes.mid; do
        "$VM_RUN" scp "$D/$f" vm:C:/work/integ/ >/dev/null
    done
fi
for sc in $SCENARIOS; do run_one "$sc"; done
exit $fail
