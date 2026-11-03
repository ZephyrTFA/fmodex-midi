#!/usr/bin/env python3
"""Reference event dump of a MIDI file using mido, in test/smf_dump's format:
"t_us HEX..." per non-meta message in merged order, then "length t_us"."""
import sys

import mido


def main():
    mid = mido.MidiFile(sys.argv[1])
    t = 0.0
    for msg in mid:   # merged tracks, delta times in seconds via the tempo map
        t += msg.time
        if msg.is_meta:
            continue
        print(round(t * 1e6), " ".join(f"{b:02X}" for b in msg.bytes()))
    print("length", round(mid.length * 1e6))


if __name__ == "__main__":
    main()
