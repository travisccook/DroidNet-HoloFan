#!/usr/bin/env python3
"""Refuse private data in this repo: home-network IPv4 addresses, real
(universally administered) MAC addresses, personal home paths, e-mail
addresses, and the literals of a local pattern file that is never committed.
File names are scanned as well as file contents.

    python3 tools/check_private.py            every tracked and untracked, not ignored, file
                                              of the whole repository, from any directory in it
    python3 tools/check_private.py --staged   what is staged (the pre-commit hook;
                                              tools/install_hooks.sh installs it)
    python3 tools/check_private.py FILE ...   these files (not together with --staged)

The pattern file (default ~/.config/droidnet-private/patterns.txt, or
$DROIDNET_PRIVATE_PATTERNS) holds one literal per line, UTF-8, matched
ignoring case; lines starting with '#' are comments. Its matches are reported
by pattern number, never by value. With $DROIDNET_PRIVATE_PATTERNS set, a
file that cannot be read is an error; with it unset, a missing default file
is only a note.

MAC addresses are found as six pairs joined by ':' or '-', as a C array of
exactly six 0xNN bytes, and, when they hold at least one of the letters a-f,
as six pairs joined by spaces or as Cisco's hhhh.hhhh.hhhh. Not found, by
design: twelve bare hex digits (hashes look the same), locally administered
MACs and the RFC 7042 documentation range 00:00:5E:00:53:xx (the
documentation and the tests use them), host names and passwords (use the
pattern file for those).

Exit 0 = clean, 1 = findings, 2 = usage error or unreadable pattern file.
"""
import os
import re
import subprocess
import sys

DEFAULT_PATTERNS = os.path.join(os.path.expanduser("~"), ".config", "droidnet-private", "patterns.txt")
ALLOWED_IPS = {"192.168.4.1", "192.168.4.2"}          # the stock fan's own access point and its client
IPV4 = re.compile(r"(?<![\w.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?!\w|\.\d)")
H2 = "[0-9A-Fa-f]{2}"
# Six pairs joined by ':' or '-'. A label may touch the address ("mac:00:..."),
# so what must not precede it is another pair of the same run, not any colon.
MAC = re.compile(rf"(?<![0-9A-Za-z])(?<!(?<![0-9A-Za-z]){H2}[:-])({H2})([:-]){H2}(?:\2{H2}){{4}}"
                 rf"(?![0-9A-Za-z])(?![:-]{H2}(?![0-9A-Za-z]))")
# Six pairs joined by single spaces, and not part of a longer hex dump.
MAC_SPACED = re.compile(rf"(?<![0-9A-Za-z])(?<!{H2} )({H2})(?: {H2}){{5}}(?![0-9A-Za-z])(?! {H2}(?![0-9A-Za-z]))")
MAC_CISCO = re.compile(r"(?<![0-9A-Za-z.])([0-9A-Fa-f]{2})[0-9A-Fa-f]{2}(?:\.[0-9A-Fa-f]{4}){2}(?![0-9A-Za-z])(?!\.[0-9A-Fa-f])")
# Exactly six 0xNN bytes separated by commas: a C array, not part of a longer one.
MAC_BYTES = re.compile(rf"(?<![0-9A-Za-z])(?<!,)(?<!, )0[xX]({H2})(?:\s*,\s*0[xX]{H2}){{5}}(?![0-9A-Za-z])(?!\s*,\s*0[xX])")
DOC_MAC_PREFIX = "00005e0053"                         # 00:00:5E:00:53:xx, reserved for documentation
HOME_PATH = re.compile(r"/(?:Users|home)/[A-Za-z0-9._-]+")
WINDOWS_HOME = re.compile(r"(?<![A-Za-z])[A-Za-z]:\\{1,2}Users\\{1,2}[A-Za-z0-9._-]+", re.IGNORECASE)
ROOT_HOME = re.compile(r"(?<![A-Za-z0-9_.$~{}<>-])/root(?=/)")
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
        for pattern, need_letter in ((MAC, False), (MAC_BYTES, False), (MAC_SPACED, True), (MAC_CISCO, True)):
            for m in pattern.finditer(line):
                digits = re.sub(r"0[xX]|[^0-9A-Fa-f]", "", m.group(0)).lower()
                if digits in ("f" * 12, "0" * 12) or int(m.group(1), 16) & 0x02:
                    continue                               # broadcast, zero, or locally administered
                if digits.startswith(DOC_MAC_PREFIX):
                    continue                               # RFC 7042 documentation range
                if need_letter and digits.isdigit():
                    continue                               # six numbers, or a version: too common to flag
                found.append((n, f"hardware MAC {m.group(0)}"))
        for pattern in (HOME_PATH, WINDOWS_HOME, ROOT_HOME):
            for m in pattern.finditer(line):
                found.append((n, f"home path {m.group(0)}/" if pattern is ROOT_HOME else f"home path {m.group(0)}"))
        for m in EMAIL.finditer(line):
            if m.group(1).lower() not in ALLOWED_EMAIL_DOMAINS:
                found.append((n, f"e-mail address {m.group(0)}"))
        low = line.lower()
        for i, p in enumerate(lowered, 1):
            if p and p in low:
                found.append((n, f"local pattern #{i}"))
    return found


class Unusable(Exception):
    pass


def load_local(path, required=False):
    """The literals of the pattern file. A missing file is [] unless it is
    `required`; a file that exists but cannot be read is always an error."""
    try:
        with open(path, encoding="utf-8") as f:
            return [s.strip() for s in f if s.strip() and not s.strip().startswith("#")]
    except FileNotFoundError:
        if required:
            raise Unusable(f"the pattern file {path} does not exist")
        return []
    except (OSError, UnicodeDecodeError) as e:
        raise Unusable(f"the pattern file {path} cannot be read ({type(e).__name__})")


def git(*args):
    return subprocess.run(["git", *args], capture_output=True, check=True).stdout


def repo_files():
    out = git("ls-files", "-z", "--cached", "--others", "--exclude-standard")
    return sorted({p for p in out.decode("utf-8", "replace").split("\0") if p})


def staged_files():
    # T: a type change (file to symlink and back) is new content too.
    out = git("diff", "--cached", "--name-only", "--diff-filter=ACMRT", "-z")
    return [p for p in out.decode("utf-8", "replace").split("\0") if p]


def usage(message):
    print(f"error: {message}\n", file=sys.stderr)
    print(__doc__)
    return 2


def main(argv=None):
    args = sys.argv[1:] if argv is None else argv
    staged = "--staged" in args
    files = [a for a in args if a != "--staged"]
    if any(a.startswith("-") for a in files):
        return usage(f"unknown option {next(a for a in files if a.startswith('-'))}")
    if staged and files:
        return usage("--staged scans the index; it takes no FILE arguments")
    configured = os.environ.get("DROIDNET_PRIVATE_PATTERNS")
    pat_path = configured or DEFAULT_PATTERNS
    try:
        local = load_local(pat_path, required=bool(configured))
    except Unusable as e:
        print(f"error: {e}; nothing was scanned", file=sys.stderr)
        return 2
    if not configured and not os.path.exists(pat_path):
        print(f"note: no local pattern file at {pat_path}; generic checks only", file=sys.stderr)
    if not files:
        # git lists paths below the current directory only: always work from the top.
        try:
            os.chdir(git("rev-parse", "--show-toplevel").decode("utf-8", "replace").rstrip("\n"))
        except (subprocess.CalledProcessError, OSError):
            print("error: not inside a git work tree; give FILE arguments instead", file=sys.stderr)
            return 2
    if staged:
        names = staged_files()
        read = lambda p: git("show", f":{p}")             # noqa: E731  the staged blob
    else:
        names = files or repo_files()
        read = lambda p: open(p, "rb").read()             # noqa: E731
    count = 0
    for name in names:
        for _, msg in scan_text(name, local):
            print(f"{name}: file name: {msg}")
            count += 1
        if not staged and os.path.islink(name):
            for _, msg in scan_text(os.readlink(name), local):
                print(f"{name}: link target: {msg}")
                count += 1
        if not staged and not os.path.isfile(name):
            continue
        try:
            data = read(name)
        except subprocess.CalledProcessError:
            continue                                       # a staged submodule: no blob to read
        # UTF-8, so that a non-ASCII local pattern can match; bytes that are
        # not UTF-8 become U+FFFD and the ASCII checks still see the rest.
        for line, msg in scan_text(data.decode("utf-8", "replace"), local):
            print(f"{name}:{line}: {msg}")
            count += 1
    if count:
        print(f"{count} finding(s): remove them before committing", file=sys.stderr)
    return 1 if count else 0


if __name__ == "__main__":
    sys.exit(main())
