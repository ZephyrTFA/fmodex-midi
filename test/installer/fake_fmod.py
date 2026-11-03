#!/usr/bin/env python3
"""Write a .def for a fake "original" fmodex.dll: every name from the export
list (minus --drop names, plus --add names) implemented by one stub.
usage: fake_fmod.py <exports.txt> <out.def> [--drop NAME]... [--add NAME]..."""
import sys

args = sys.argv[1:]
exports_path, out = args[0], args[1]
drop, add, i = set(), [], 2
while i < len(args):
    (drop.add if args[i] == "--drop" else add.append)(args[i + 1])
    i += 2
names = [l.strip() for l in open(exports_path) if l.strip() and l.strip() not in drop] + add
with open(out, "w") as f:
    f.write("LIBRARY fmodex.dll\nEXPORTS\n")
    for n in names:
        f.write(f'"{n}"=stub\n')
