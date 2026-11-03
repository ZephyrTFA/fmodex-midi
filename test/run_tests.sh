#!/usr/bin/env bash
# Native host tests (run inside `nix develop`): parser vs mido, player unit
# tests, and a truncation/bit-flip fuzz smoke test under ASan/UBSan.
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/native
mkdir -p "$B"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -static-libasan -static-libubsan"
cc -std=gnu11 -O1 -g -Wall -Wextra $SAN -Isrc -o "$B/smf_dump" test/smf_dump.c src/smf.c
cc -std=gnu11 -O1 -g -Wall -Wextra $SAN -Isrc -o "$B/test_player" test/test_player.c src/player.c src/smf.c -lm

fail=0

# RMID-wrapped copy of the piano file must parse identically.
python3 - test/midi/one_single_piano_note.mid "$B/piano.rmi" <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
body = b"RMID" + b"data" + struct.pack("<I", len(d)) + d + (b"\0" if len(d) & 1 else b"")
open(sys.argv[2], "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
EOF

# Parser vs mido. Times may differ by 1us (integer floor vs float rounding).
for f in test/midi/*.mid "$B/piano.rmi"; do
    ref=$f; [[ $f == *.rmi ]] && ref=test/midi/one_single_piano_note.mid
    if python3 - "$B/smf_dump" "$f" "$ref" <<'EOF'
import subprocess, sys
ours = subprocess.run([sys.argv[1], sys.argv[2]], capture_output=True, text=True, check=True).stdout.split("\n")
ref = subprocess.run([sys.executable, "tools/mido_dump.py", sys.argv[3]], capture_output=True, text=True, check=True).stdout.split("\n")
if len(ours) != len(ref):
    sys.exit(f"{sys.argv[2]}: {len(ours)} lines vs mido {len(ref)}")
for i, (a, b) in enumerate(zip(ours, ref)):
    if not a and not b:
        continue
    wa, wb = a.split(), b.split()
    if wa[0] == "length" or wb[0] == "length":
        same = wa[0] == wb[0] and abs(int(wa[1]) - int(wb[1])) <= 1
    else:
        same = wa[1:] == wb[1:] and abs(int(wa[0]) - int(wb[0])) <= 1
    if not same:
        sys.exit(f"{sys.argv[2]} line {i}: ours '{a}' vs mido '{b}'")
print(f"ok   parse {sys.argv[2]} ({len(ours) - 2} events)")
EOF
    then :; else fail=1; fi
done

"$B/test_player" test/midi/one_single_piano_note.mid test/midi/tempo_changes.mid || fail=1

# Fuzz smoke: every truncation and 2000 random byte corruptions of each file
# must parse or fail cleanly (sanitizers abort on any memory error).
python3 - "$B" test/midi/*.mid <<'EOF' || fail=1
import os, random, subprocess, sys
b, files = sys.argv[1], sys.argv[2:]
rng = random.Random(1234)
path = os.path.join(b, "fuzz.mid")
n = 0
for f in files:
    d = open(f, "rb").read()
    cases = [d[:i] for i in range(len(d))]
    for _ in range(2000):
        x = bytearray(d)
        for _ in range(rng.randint(1, 4)):
            x[rng.randrange(len(x))] = rng.randrange(256)
        cases.append(bytes(x))
    for c in cases:
        open(path, "wb").write(c)
        r = subprocess.run([os.path.join(b, "smf_dump"), path], capture_output=True)
        if r.returncode not in (0, 1):
            open(os.path.join(b, "crash.mid"), "wb").write(c)
            sys.exit(f"crash (rc={r.returncode}) on mutation of {f}; saved {b}/crash.mid\n{r.stderr.decode()[-2000:]}")
        n += 1
print(f"ok   fuzz smoke ({n} inputs)")
EOF

exit $fail
