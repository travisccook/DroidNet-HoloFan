#!/usr/bin/env python3
"""Refuse private data in this repo: home-network IPv4 addresses, real
(universally administered) MAC addresses, personal home paths, e-mail
addresses, and the literals of a local pattern file that is never committed.

    python3 tools/check_private.py            every tracked and untracked, not ignored, file
    python3 tools/check_private.py --staged   what is staged (the pre-commit hook)
    python3 tools/check_private.py FILE ...   these files

The pattern file (default ~/.config/droidnet-private/patterns.txt, or
$DROIDNET_PRIVATE_PATTERNS) holds one literal per line, matched ignoring case;
lines starting with '#' are comments. Its matches are reported by pattern
number, never by value. Exit 0 = clean, 1 = findings, 2 = usage error.
"""
import os
import re
import subprocess
import sys

DEFAULT_PATTERNS = os.path.join(os.path.expanduser("~"), ".config", "droidnet-private", "patterns.txt")
ALLOWED_IPS = {"192.168.4.1", "192.168.4.2"}          # the stock fan's own access point and its client
IPV4 = re.compile(r"(?<![\w.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?!\w|\.\d)")
MAC = re.compile(r"(?<![0-9A-Fa-f:-])([0-9A-Fa-f]{2})([:-])[0-9A-Fa-f]{2}(?:\2[0-9A-Fa-f]{2}){4}(?![0-9A-Fa-f:-])")
HOME_PATH = re.compile(r"/(?:Users|home)/[A-Za-z0-9._-]+")
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,})")
ALLOWED_EMAIL_DOMAINS = {"example.com", "example.org", "anthropic.com"}


def private_ipv4(parts):
    a, b = parts[0], parts[1]
    return a == 10 or (a == 172 and 16 <= b <= 31) or (a == 192 and b == 168) or (a == 169 and b == 254)


def scan_text(text, local):
    found = []
    lowered = [p.lower() for p in local]
    for n, line in enumerate(text.splitlines(), 1):
        for m in IPV4.finditer(line):
            parts = [int(g) for g in m.groups()]
            if all(p <= 255 for p in parts) and private_ipv4(parts) and m.group(0) not in ALLOWED_IPS:
                found.append((n, f"private IPv4 {m.group(0)}"))
        for m in MAC.finditer(line):
            first = int(m.group(1), 16)
            mac = m.group(0).lower().replace("-", ":")
            if mac in ("ff:ff:ff:ff:ff:ff", "00:00:00:00:00:00") or first & 0x02:
                continue                                   # broadcast, zero, or locally administered
            found.append((n, f"hardware MAC {m.group(0)}"))
        for m in HOME_PATH.finditer(line):
            found.append((n, f"home path {m.group(0)}"))
        for m in EMAIL.finditer(line):
            if m.group(1).lower() not in ALLOWED_EMAIL_DOMAINS:
                found.append((n, f"e-mail address {m.group(0)}"))
        low = line.lower()
        for i, p in enumerate(lowered, 1):
            if p and p in low:
                found.append((n, f"local pattern #{i}"))
    return found


def load_local(path):
    try:
        with open(path, encoding="utf-8") as f:
            return [s.strip() for s in f if s.strip() and not s.strip().startswith("#")]
    except FileNotFoundError:
        return []


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, check=True).stdout


def repo_files():
    out = git("ls-files", "-z", "--cached", "--others", "--exclude-standard")
    return sorted({p for p in out.decode("utf-8", "replace").split("\0") if p})


def staged_files():
    out = git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z")
    return [p for p in out.decode("utf-8", "replace").split("\0") if p]


def main(argv=None):
    args = sys.argv[1:] if argv is None else argv
    staged = "--staged" in args
    files = [a for a in args if a != "--staged"]
    if any(a.startswith("-") for a in files):
        print(__doc__)
        return 2
    pat_path = os.environ.get("DROIDNET_PRIVATE_PATTERNS", DEFAULT_PATTERNS)
    local = load_local(pat_path)
    if not local:
        print(f"note: no local pattern file at {pat_path}; generic checks only", file=sys.stderr)
    if staged:
        names = staged_files()
        read = lambda p: git("show", f":{p}")             # noqa: E731  the staged blob
    else:
        names = files or repo_files()
        read = lambda p: open(p, "rb").read()             # noqa: E731
    count = 0
    for name in names:
        if not staged and not os.path.isfile(name):
            continue
        text = read(name).decode("latin-1")
        for line, msg in scan_text(text, local):
            print(f"{name}:{line}: {msg}")
            count += 1
    if count:
        print(f"{count} finding(s): remove them before committing", file=sys.stderr)
    return 1 if count else 0


if __name__ == "__main__":
    sys.exit(main())
