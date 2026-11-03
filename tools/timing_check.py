#!/usr/bin/env python3
"""Compare a log-backend event log against mido's reference for the same file.

Every reference event must appear in the log, in order, with the same bytes.
Its expected time is start + t_ref / ratio, where `start` is the
"# <t> start voice N" marker. Prints timing error stats in ms. Exits non-zero
on a missing event or if p99 |error| exceeds --p99-ms (default 2).

usage: timing_check.py events.log file.mid [--ratio r] [--seek-ms ms] [--p99-ms 2]
"""
import argparse
import sys

import mido


def reference(path):
    mid = mido.MidiFile(path)
    t, out = 0.0, []
    for msg in mid:
        t += msg.time
        if not msg.is_meta:
            out.append((t * 1e6, " ".join(f"{b:02X}" for b in msg.bytes())))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("mid")
    ap.add_argument("--ratio", type=float, default=1.0)
    ap.add_argument("--seek-ms", type=float, default=0.0)
    ap.add_argument("--p99-ms", type=float, default=2.0)
    a = ap.parse_args()

    start, events = None, []
    for line in open(a.log):
        if line.startswith("#"):
            parts = line.split()
            if len(parts) >= 3 and parts[2] == "start" and start is None:
                start = int(parts[1])
            continue
        if start is not None:
            t, rest = line.split(" ", 1)
            events.append((int(t), rest.strip()))
    if start is None:
        sys.exit("no start marker in log")

    seek = a.seek_ms * 1000
    ref = [(t, b) for t, b in reference(a.mid) if t >= seek]
    errs, j = [], 0
    for t_ref, b in ref:
        while j < len(events) and events[j][1] != b:
            j += 1
        if j == len(events):
            sys.exit(f"reference event {b} @ {t_ref / 1000:.3f} ms not found in log")
        errs.append((events[j][0] - (start + (t_ref - seek) / a.ratio)) / 1000)
        j += 1

    ab = sorted(abs(e) for e in errs)
    p = lambda q: ab[min(len(ab) - 1, int(q * len(ab)))]
    print(f"{len(errs)} events  mean {sum(errs) / len(errs):+.3f} ms  "
          f"p50 {p(0.5):.3f}  p99 {p(0.99):.3f}  max {ab[-1]:.3f} ms")
    if p(0.99) > a.p99_ms:
        sys.exit(f"p99 {p(0.99):.3f} ms exceeds {a.p99_ms} ms")


if __name__ == "__main__":
    main()
