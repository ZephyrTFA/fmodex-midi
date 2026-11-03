#!/usr/bin/env python3
"""Build the self-contained installer script: tools/installer.ps1.in with the
proxy fmodex.dll (base64) and the default ini embedded.

usage: make_installer.py <installer.ps1.in> <fmodex.dll> <ini> <version> <issues-url> <out.ps1>
"""
import base64
import hashlib
import sys


def main():
    tmpl_path, dll_path, ini_path, version, issues_url, out_path = sys.argv[1:7]
    dll = open(dll_path, "rb").read()
    ini = open(ini_path, encoding="ascii").read()
    b64 = base64.b64encode(dll).decode("ascii")
    lines = "\n".join(b64[i:i + 100] for i in range(0, len(b64), 100))
    if "'@" in ini:
        raise SystemExit("ini must not contain '@")
    data = (f"$DefaultIni = @'\n{ini.rstrip()}\n'@\n"
            f"$ProxyB64 = @'\n{lines}\n'@")
    s = open(tmpl_path, encoding="ascii").read()
    s = (s.replace("@VERSION@", version)
          .replace("@ISSUES_URL@", issues_url)
          .replace("@PROXY_SHA256@", hashlib.sha256(dll).hexdigest())
          .replace("@DATA@", data))
    left = [t for t in ("@VERSION@", "@PROXY_SHA256@", "@ISSUES_URL@", "@DATA@") if t in s]
    if left:
        raise SystemExit(f"unreplaced template placeholders: {left}")
    # CRLF + ASCII: what Windows PowerShell 5.1 handles best.
    with open(out_path, "w", encoding="ascii", newline="\r\n") as f:
        f.write(s)


if __name__ == "__main__":
    main()
