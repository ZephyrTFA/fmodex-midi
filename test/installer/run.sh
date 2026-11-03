#!/usr/bin/env bash
# Installer tests: run the generated fmodex-midi.ps1 with PowerShell against
# fake "original" fmodex.dll files built from the export list, and check its
# compatibility verdicts and file handling. Run inside `nix develop`;
# PWSH defaults to `pwsh` (CI runners have it; locally: nix shell nixpkgs#powershell).
set -euo pipefail
cd "$(dirname "$0")/../.."
PWSH=${PWSH:-pwsh}
D=build/installer-test
EXPORTS=references/fmodex-4.32.1/exports.txt
make -s dist
rm -rf "$D"; mkdir -p "$D"
echo 'int __stdcall stub(void) { return 0; }' > "$D/stub.c"

fake() {   # fake <name> [fake_fmod.py options...]
    local n=$1; shift
    python3 test/installer/fake_fmod.py "$EXPORTS" "$D/$n.def" "$@"
    $CC -shared -O2 -static-libgcc -Wl,--enable-stdcall-fixup -o "$D/$n.dll" "$D/stub.c" "$D/$n.def"
}
HOOK='?playSound@System@FMOD@@QAG?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PAVSound@2@_NPAPAVChannel@2@@Z'
fake exact
fake fewer --drop FMOD_System_CreateGeometry --drop FMOD_Geometry_Release
fake extra --add FMOD_Brand_New_Function
fake nohook --drop "$HOOK"
$CC -shared -O2 -static-libgcc -o "$D/mock.dll" test/integ/mock_fmod.c test/integ/mock_fmod.def

fail=0
ps() { "$PWSH" -NoProfile -NonInteractive -File build/fmodex-midi.ps1 -NoPause "$@" 2>&1; }
expect() {   # expect <case> <regex> <output>
    if grep -qE "$2" <<<"$3"; then echo "ok   $1"; else echo "FAIL $1: wanted /$2/"; sed 's/^/     /' <<<"$3"; fail=1; fi
}
setup() { rm -rf "$D/app"; mkdir -p "$D/app"; cp "$D/$1.dll" "$D/app/fmodex.dll"; }

setup exact
expect "exact: status"       'not installed; original: .*exports match the tested'  "$(ps -Action status -Path "$D/app")"
orig=$(sha256sum < "$D/app/fmodex.dll")
out=$(ps -Path "$D/app")
expect "exact: install"      'app: installed$'                                    "$out"
expect "exact: issue hint"   'github.com/.*/issues'                               "$out"
expect "exact: renamed"      'yes'                                                "$( [[ -f $D/app/fmodex_real.dll && -f $D/app/fmodex-midi.ini ]] && echo yes)"
expect "exact: status after" 'installed \(.*\); original: .*match the tested'     "$(ps -Action status -Path "$D/app")"
expect "exact: reinstall"    'already installed'                                  "$(ps -Path "$D/app")"
expect "exact: uninstall"    'uninstalled'                                        "$(ps -Action uninstall -Path "$D/app")"
expect "exact: restored"     'yes'                                                "$( [[ $(sha256sum < "$D/app/fmodex.dll") == "$orig" && ! -e $D/app/fmodex_real.dll ]] && echo yes)"

setup fewer
expect "fewer: untested ok"  'installed \(UNTESTED FMOD version: buyer beware\)'  "$(ps -Path "$D/app")"

for c in extra nohook mock; do
    setup $c
    before=$(sha256sum < "$D/app/fmodex.dll")
    out=$(ps -Path "$D/app" || true)
    expect "$c: refused"     'looks incompatible.*|not installing'                 "$out"
    expect "$c: untouched"   'yes'                                                 "$( [[ $(sha256sum < "$D/app/fmodex.dll") == "$before" && ! -e $D/app/fmodex_real.dll ]] && echo yes)"
done
expect "extra: reason"       "aren't provided \(e.g. FMOD_Brand_New_Function\)"     "$(setup extra; ps -Action status -Path "$D/app")"
expect "nohook: reason"      'lacks 1 hooked function'                             "$(setup nohook; ps -Action status -Path "$D/app")"
setup extra
expect "extra: -Force"       'app: installed'                                      "$(ps -Path "$D/app" -Force)"

exit $fail
