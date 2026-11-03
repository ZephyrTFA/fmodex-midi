#!/usr/bin/env python3
"""Check a built proxy fmodex.dll against the real export list.

- the export name set must equal the real DLL's exactly;
- hooked names must be real exports (not forwarders), all others must
  forward to fmodex_real.<same name>;
- imported DLLs must be a subset of the allowed set.

usage: check_exports.py <fmodex.dll> <exports.txt> <hooks.txt>
"""
import re
import subprocess
import sys

ALLOWED_IMPORTS = {"kernel32.dll", "msvcrt.dll", "winmm.dll"}


def main():
    dll, exports_path, hooks_path = sys.argv[1:4]
    want = {l.strip() for l in open(exports_path) if l.strip()}
    hooks = {l.strip() for l in open(hooks_path) if l.strip() and not l.startswith("#")}
    dump = subprocess.run(["objdump", "-p", dll], check=True, capture_output=True, text=True).stdout

    # Export Address Table: index -> (kind, forward target)
    eat = {}
    for m in re.finditer(r"^\s*\[\s*(\d+)\] \+base\[\s*\d+\] [0-9a-f]+ (Export|Forwarder) RVA(?: -- (\S+))?$",
                         dump, re.M):
        eat[int(m.group(1))] = (m.group(2), m.group(3))
    # Name table: index -> name
    names = {}
    sect = dump.split("[Ordinal/Name Pointer] Table", 1)[1]
    for m in re.finditer(r"^\s*\[\s*(\d+)\] \+base\[\s*\d+\]\s+[0-9a-f]+ (\S+)$", sect, re.M):
        names[m.group(2)] = int(m.group(1))

    errors = []
    got = set(names)
    for n in sorted(want - got):
        errors.append(f"missing export: {n}")
    for n in sorted(got - want):
        errors.append(f"unexpected export: {n}")
    for n, idx in names.items():
        kind, target = eat.get(idx, (None, None))
        if n in hooks:
            if kind != "Export":
                errors.append(f"hook {n} is {kind}, expected implemented export")
        elif kind != "Forwarder" or target != f"fmodex_real.{n}":
            errors.append(f"{n}: {kind} -> {target}, expected forwarder to fmodex_real.{n}")

    imports = {m.group(1).lower() for m in re.finditer(r"DLL Name: (\S+)", dump)}
    for d in sorted(imports - ALLOWED_IMPORTS):
        errors.append(f"unexpected import DLL: {d}")

    if errors:
        print("\n".join(errors))
        sys.exit(1)
    print(f"ok: {len(got)} exports ({len(hooks)} hooked, {len(got) - len(hooks)} forwarded), "
          f"imports {sorted(imports)}")


if __name__ == "__main__":
    main()
