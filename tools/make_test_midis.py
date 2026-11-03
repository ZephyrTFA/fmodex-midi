#!/usr/bin/env python3
"""Generate the synthetic test MIDI files in test/midi/ (deterministic)."""
import os
import sys

import mido
from mido import Message, MetaMessage, MidiFile, MidiTrack

out = sys.argv[1] if len(sys.argv) > 1 else "test/midi"


def save(mid, name):
    mid.save(os.path.join(out, name))


# Tempo changes across a format-1 file, tempo track separate from notes,
# notes on several channels; mido writes running status.
mid = MidiFile(type=1, ticks_per_beat=480)
tempo = MidiTrack([MetaMessage("set_tempo", tempo=500000, time=0),
                   MetaMessage("set_tempo", tempo=250000, time=960),
                   MetaMessage("set_tempo", tempo=1000000, time=960),
                   MetaMessage("set_tempo", tempo=600000, time=480)])
notes = MidiTrack([Message("program_change", channel=0, program=19, time=0),
                   Message("control_change", channel=0, control=7, value=90, time=0)])
for i in range(16):
    notes.append(Message("note_on", channel=0, note=60 + i, velocity=80, time=0 if i == 0 else 120))
    notes.append(Message("note_on", channel=0, note=60 + i, velocity=0, time=120))
drums = MidiTrack()
for i in range(12):
    drums.append(Message("note_on", channel=9, note=36 + (i % 3), velocity=100, time=0 if i == 0 else 230))
    drums.append(Message("note_off", channel=9, note=36 + (i % 3), velocity=0, time=10))
drums.append(Message("pitchwheel", channel=1, pitch=-4096, time=5))
mid.tracks += [tempo, notes, drums]
save(mid, "tempo_changes.mid")

# GS reset SysEx up front, RPN bend range, pitch bends, sustain.
mid = MidiFile(type=0, ticks_per_beat=96)
t = MidiTrack([
    Message("sysex", data=[0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7][:-1], time=0),
    MetaMessage("set_tempo", tempo=461538, time=0),
    Message("control_change", channel=0, control=101, value=0, time=10),
    Message("control_change", channel=0, control=100, value=0, time=0),
    Message("control_change", channel=0, control=6, value=12, time=0),
    Message("control_change", channel=0, control=38, value=0, time=0),
    Message("control_change", channel=0, control=64, value=127, time=0),
    Message("note_on", channel=0, note=64, velocity=100, time=0),
    Message("pitchwheel", channel=0, pitch=2000, time=48),
    Message("pitchwheel", channel=0, pitch=0, time=48),
    Message("note_off", channel=0, note=64, velocity=64, time=96),
    Message("control_change", channel=0, control=64, value=0, time=0),
    Message("aftertouch", channel=0, value=50, time=10),
    Message("polytouch", channel=0, note=64, value=30, time=0),
])
mid.tracks.append(t)
save(mid, "sysex_rpn.mid")
