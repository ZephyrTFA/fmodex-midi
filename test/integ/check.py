#!/usr/bin/env python3
"""Check a scenario's log-backend event log (see harness.c).
usage: check.py <scenario> <events.log>"""
import sys

sc, path = sys.argv[1], sys.argv[2]
starts, ev = [], []
for line in open(path):
    w = line.split()
    if line.startswith("#"):
        if w[2] == "start":
            starts.append(int(w[1]))
        continue
    ev.append((int(w[0]), " ".join(w[1:])))

fails = []
def check(cond, msg):
    if not cond:
        fails.append(msg)

def times(b, after=0):
    return [t for t, x in ev if x == b and t >= after]

def near(ts, want_ms, tol_ms, what):
    ok = any(abs((t - t0) / 1000 - want_ms) <= tol_ms for t in ts)
    check(ok, f"{what}: want ~{want_ms} ms after start, got "
               f"{[round((t - t0) / 1000, 1) for t in ts]}")

ON, OFF, KILL = "90 30 64", "80 30 40", "80 30 00"
if sc in ("corrupt", "wav", "backendfail"):
    check(not starts, "no voice may start")
else:
    check(starts, "a voice must start")
if starts:
    t0 = starts[0]
    if sc == "basic":
        check(times("B0 07 47"), "CC7 scaled to 71 for volume 0.5")
        near(times(ON), 0, 15, "note on")
        near(times(OFF), 461.5, 15, "note off")
    elif sc == "pause":
        near(times(KILL), 200, 30, "pause note-off")
        near(times(OFF), 961.5, 40, "resumed note off")
    elif sc in ("stop", "reverse"):
        near(times(KILL), 100, 30, "stop note-off")
        check(not times(OFF), "song note-off must not play after stop")
    elif sc == "cutover":
        check(len(starts) == 2, f"two voice starts, got {len(starts)}")
        if len(starts) == 2:
            check(any(t0 <= t <= starts[1] for t in times(KILL)), "first voice silenced before the second starts")
            check(len(times(OFF)) == 1, "only the second voice's note-off plays")
    elif sc == "master":
        near(times(KILL), 100, 30, "master pause note-off")
        near(times(OFF), 761.5, 40, "note off after master resume")
        check(times("B0 07 32"), "CC7 scaled to 50 for master volume 0.25")
    elif sc == "loop":
        ons = times(ON)
        check(len(ons) == 3, f"3 loop iterations, got {len(ons)}")
        if len(ons) == 3:
            near(ons[1:2], 1851, 15, "2nd iteration")
            near(ons[2:3], 3702, 15, "3rd iteration")
    elif sc == "seek":
        check(times("C0 13"), "program 19 chased")
        check(times("B0 00 5A") or times("B0 07 5A"), "CC7 90 chased")
        check(not [t for t, x in ev if x.startswith("90 3C")], "notes before 1.5 s must not sound")

print(f"{sc}: {'ok' if not fails else 'FAIL'}")
for f in fails:
    print("   ", f)
sys.exit(1 if fails else 0)
