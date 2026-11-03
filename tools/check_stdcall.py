#!/usr/bin/env python3
"""Check each hk_* symbol's __stdcall byte count (@N) against the argument
count implied by its MSVC-mangled name (+4 for `this`). A mismatch would
make the hook pop the wrong number of bytes and corrupt the host's stack.

usage: check_stdcall.py <nm output of hooks.o> <hooks.txt>
"""
import re
import sys


def skip_name(s, i):
    """Skip a qualified name `A@B@...@@` or one ending in a namespace backref `A@2@`."""
    while True:
        j = s.index("@", i)
        if s[j + 1] == "@":
            return j + 2
        if s[j + 1].isdigit() and s[j + 2] == "@":
            return j + 3
        i = j + 1


def skip_type(s, i):
    c = s[i]
    if c in "HIMX" or c.isdigit():   # int, unsigned, float, void, backref
        return i + 1
    if s.startswith("_N", i):        # bool
        return i + 2
    if c == "W":                     # W4Enum@@
        return skip_name(s, i + 2)
    if c in "UV":                    # struct / class
        return skip_name(s, i + 1)
    if c == "P":
        if s[i + 1] == "6":          # function pointer: callconv, ret, args, @Z
            return s.index("@Z", i) + 2
        return skip_type(s, i + 2)   # PA / PB + pointee
    if c == "D":                     # char
        return i + 1
    raise SystemExit(f"cannot parse type code {s[i:]!r}")


def count_args(mangled):
    # ?name@Class@FMOD@@QAG?AW4FMOD_RESULT@@<args>@Z, or ...@@XZ for none
    m = re.match(r"^\?\w+@\w+@FMOD@@QAG\?AW4FMOD_RESULT@@(.*)Z$", mangled)
    s = m.group(1)
    if s == "X":
        return 0
    n, i = 0, 0
    while s[i] != "@":
        i = skip_type(s, i)
        n += 1
    return n


def main():
    nm, hooks_path = sys.argv[1:3]
    have = {}
    for line in open(nm):
        m = re.search(r"\b_?hk_(\w+)@(\d+)$", line.strip())
        if m:
            have[m.group(1)] = int(m.group(2))
    bad = 0
    for h in (l.strip() for l in open(hooks_path)):
        if not h or h.startswith("#"):
            continue
        method, cls = re.match(r"^\?(\w+)@(\w+)@FMOD", h).groups()
        sym = f"{cls}_{method}"
        want = 4 * (1 + count_args(h))
        got = have.get(sym)
        if got != want:
            print(f"MISMATCH {sym}: symbol @{got}, mangled name implies @{want}")
            bad += 1
    if bad:
        sys.exit(1)
    print(f"ok: all hook stdcall sizes match their mangled signatures")


if __name__ == "__main__":
    main()
