#!/usr/bin/env python3
"""HoloFan bench helper for an operator who drives the bench from a shell.
Commands go to a WCB through the DroidNet Pi given by --host (or
$HFBENCH_HOST); everything that WCB prints is in the meshlog.py log, one line
per entry: "HH:MM:SS.mmm <dir> <text>" (dir rece / sent / raw / "# ...").

    hfbench.py send CMD [CMD ...] [--gap 0.4]
    hfbench.py run CMD [CMD ...] --until REGEX [--fail REGEX] [--timeout 60] [--gap 1]
    hfbench.py query CMD --expect REGEX [--tries 12] [--gap 1]
    hfbench.py mark LABEL [--at HH:MM:SS]
    hfbench.py summary [FILE] [--since HH:MM:SS] [--until HH:MM:SS]

Common options: --log FILE (default $HFLOG, else /tmp/hf_mesh.log), --host
(default $HFBENCH_HOST, resolved once, IPv4 only) or --host-ip A.B.C.D.

CMD gets ';wHoloFan,' in front unless it starts with ';' (another node, e.g.
';wHoloLifter,#HLF0') or '?' (a WCB command: only the read-only ?WDP,DUMP and
?BACKUP pass; never a ?X,<value> setter). --until / --fail / --expect are
matched against received lines and MARKs, never against the WCB's echo of what
was sent. Exit status: 0 done / matched, 1 send or setup error, 2 timeout or no
match, 3 a --fail line (default: 'refused') came first.
"""
import argparse
import json
import os
import re
import socket
import sys
import time
import urllib.request
from collections import namedtuple
from types import SimpleNamespace

PORT = "/dev/ttyUSB0"                        # the WCB's USB port on the Pi
DEFAULT_LOG = os.environ.get("HFLOG", "/tmp/hf_mesh.log")
WCB_READ_ONLY = ("?WDP,DUMP", "?BACKUP")    # the only WCB commands let through
FAN_KEYS = ("TERM:16", "WCB16", "HoloFan", "#HL:FAN", "OFFLINE", "ONLINE", "# MARK", "HoloLifter,#HLF")
TAIL_S = 2.0                                 # run: keep printing this long after the match
TERM = "[TERM:16]"                           # RTERM lines from node 16 (the fan)


# ------------------------------------------------------------------ network

def wcb_sender(host, host_ip=None, timeout=10.0):
    """The only network code. Resolve host once (IPv4: a .local name takes 5 s
    per lookup on macOS otherwise) and return send(cmd): True, or what went
    wrong. Tests replace this function."""
    ip = host_ip or socket.getaddrinfo(host, 80, socket.AF_INET)[0][4][0]
    url = f"http://{ip}/api/wcb/command"

    def send(cmd):
        body = json.dumps({"port": PORT, "command": cmd, "expect_reply": False}).encode()
        req = urllib.request.Request(url, body, {"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                ok = json.load(r).get("success")
            return True if ok is True else f"success={ok!r}"
        except Exception as e:                # report it, never crash a bench run
            return repr(e)

    send.ip = ip
    return send


def fan_command(cmd):
    """The WCB command for CMD: ';wHoloFan,' + CMD unless it already names a
    node (';') or is a read-only WCB command ('?'). ValueError otherwise."""
    cmd = cmd.strip()
    if not cmd:
        raise ValueError("empty command")
    if cmd.startswith(";"):
        return cmd
    if cmd.startswith("?"):
        if cmd.upper() not in WCB_READ_ONLY:
            raise ValueError(f"{cmd}: only {', '.join(WCB_READ_ONLY)} go to the WCB (never ?X,<value> setters)")
        return cmd
    return ";wHoloFan," + cmd


# ---------------------------------------------------------------- log lines

Entry = namedtuple("Entry", "t dir text")    # t: seconds since midnight (+ a day per wrap)
LINE_RE = re.compile(r"^(\d\d):(\d\d):(\d\d(?:\.\d+)?) (\S+)(?: (.*))?$")


def parse_line(line):
    """'HH:MM:SS.mmm <dir> <text>' -> Entry; meshlog's '# ...' lines get dir '#'."""
    m = LINE_RE.match(line.rstrip("\r\n"))
    if not m:
        return None
    t = int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3])
    return Entry(t, m[4], m[5] or "")


def parse_log(lines):
    """Entries of a log in time order. Times that jump back by more than 12 h
    are the next day; a backdated MARK (mark --at) moves to its time."""
    out, day, prev = [], 0.0, None
    for line in lines:
        e = parse_line(line)
        if not e:
            continue
        t = e.t + day
        if prev is not None and t < prev - 43200:
            day += 86400                     # past midnight
            t += 86400
        elif prev is not None and t > prev + 43200:
            t -= 86400                       # backdated to before midnight
        e = e._replace(t=t)
        prev = t if prev is None else max(prev, t)
        out.append(e)
    out.sort(key=lambda e: e.t)              # stable
    return out


def parse_clock(text):
    """'HH:MM', 'HH:MM:SS' or 'HH:MM:SS.s' -> seconds since midnight."""
    m = re.fullmatch(r"(\d{1,2}):(\d\d)(?::(\d\d(?:\.\d+)?))?", text.strip())
    if not m or int(m[1]) > 23 or int(m[2]) > 59:
        raise ValueError(f"time {text!r}: want HH:MM[:SS[.s]]")
    return int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3] or 0)


def fmt_clock(t, places=1):
    t = round(t % 86400, places) % 86400    # round first: 59.96 s must not print as 60.0
    h, rest = divmod(t, 3600)
    m, s = divmod(rest, 60)
    width = 3 + places if places else 2
    return f"{int(h):02d}:{int(m):02d}:{s:0{width}.{places}f}"


def stamp(epoch):
    """Wall clock as meshlog.py writes it: HH:MM:SS.mmm (local time)."""
    return time.strftime("%H:%M:%S", time.localtime(epoch)) + f".{int(epoch * 1000) % 1000:03d}"


def clock_s(epoch):
    lt = time.localtime(epoch)
    return lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec + epoch % 1


def is_fan_line(line):
    return any(k in line for k in FAN_KEYS)


def matchable(e):
    """Lines a regex may end a wait on: received ones and MARKs, not our echo."""
    return e.dir in ("rece", "raw") or (e.dir == "#" and e.text.startswith("MARK"))


# ------------------------------------------------------------ field parsers

def kv_fields(text):
    """'a=1 b=x ...' -> dict; a 'last=' field runs to the end of the line."""
    head, sep, last = text.partition(" last=")
    d = dict(re.findall(r"(\w+)=(\S*)", head))
    if sep:
        d["last"] = last
    return d


def parse_fandiag(text):
    """'#HL:FANDIAG st=READY rxb=.. tx=.. ... rst=3 chg=0 v0=2 last=...' -> dict
    (0.9.6 lines have no chg / v0)."""
    i = text.find("#HL:FANDIAG ")
    return kv_fields(text[i + 12:]) if i >= 0 else None


SETTING_RE = re.compile(r"setting (\S+) (\S+) (\d+) (\d+) (\d+) ([^|\s]+)")
PLAYINFO_RE = re.compile(r'PlayInfo "([^"]*)" (\S+) (\d+) (\d+)')


def parse_setting(text):
    """The MCU's 'setting on one 2 3 164 F-S12' -> dict (ble, loop, br, vol, angle, model)."""
    m = SETTING_RE.search(text)
    if not m:
        return None
    return {"ble": m[1], "loop": m[2], "br": int(m[3]), "vol": int(m[4]), "angle": int(m[5]), "model": m[6]}


def parse_playinfo(text):
    """'PlayInfo "04-Beeps.bin" playing 1 2' -> dict (name, state, prog, total)."""
    m = PLAYINFO_RE.search(text)
    if not m:
        return None
    return {"name": m[1], "state": m[2], "prog": int(m[3]), "total": int(m[4])}


SAG_START = re.compile(r"\[sag\] levels (\d)-(\d) for (\d+) s")
SAG_SUM = re.compile(r"\[sag\] (\d+) s, ms crossed/watched:(.*)")
SAG_LINE = re.compile(r"\[sag\] s (\d+)-(\d+): (\S*)")
SAG_LEVEL = re.compile(r"L(\d)\(([\d.]+)V\)=(\d+)/(\d+)")
PROBE_RES = re.compile(r"\[probe\] (\d+) dBm, (\d+) frames/level, crossed \(idle\):(.*)")
PROBE_LEVEL = re.compile(r"L(\d)\(([\d.]+)V\)=(\d+) \((\d+)\)")
VOLTS = {3: "3.10", 4: "2.92", 5: "2.76", 6: "2.64", 7: "2.51"}


def parse_sag_summary(text):
    """'[sag] 300 s, ms crossed/watched: L3(3.10V)=6/57601 ...; det 0' ->
    {'seconds', 'levels': {level: (crossed_ms, watched_ms)}, 'det' (None before 0.9.7)}."""
    m = SAG_SUM.search(text)
    if not m:
        return None
    det = re.search(r"; det (\d+)", m[2])
    return {"seconds": int(m[1]),
            "levels": {int(l): (int(a), int(b)) for l, _, a, b in SAG_LEVEL.findall(m[2])},
            "det": int(det[1]) if det else None}


def parse_sag_timeline(text):
    """'[sag] s 0-99: 35---4--...' -> {second: deepest level crossed in it}."""
    m = SAG_LINE.search(text)
    if not m:
        return None
    return {int(m[1]) + i: int(c) for i, c in enumerate(m[3]) if c.isdigit()}


def parse_probe(text):
    """'[probe] 20 dBm, 20 frames/level, crossed (idle): L3(3.10V)=20 (1) ...' ->
    {'dBm', 'frames', 'levels': {level: (crossed, idle)}}."""
    m = PROBE_RES.search(text)
    if not m:
        return None
    return {"dBm": int(m[1]), "frames": int(m[2]),
            "levels": {int(l): (int(a), int(b)) for l, _, a, b in PROBE_LEVEL.findall(m[3])}}


def sag_deepest(levels, timeline=None):
    """Deepest (highest) level crossed at all, 0 if none."""
    hit = [l for l, (a, _) in levels.items() if a > 0]
    hit += list((timeline or {}).values())
    return max(hit, default=0)


# ------------------------------------------------------------------- digest

SENT_FAN = re.compile(r"^;w(?:HoloFan|16),(.*)$")
SENT_RELAY = re.compile(r"^;w(?:HoloLifter|15),#HLF([01])$")
MCU_REPLY = re.compile(r"^\[mcu\] \d+ bytes: (.*)$")


def fan_body(text):
    return text[len(TERM):] if text.startswith(TERM) else text


def describe(e):
    """Compact digest text for an entry that matters, None for the rest."""
    if e.dir == "sent":
        m = SENT_FAN.match(e.text)
        if m:
            return "> " + m[1]
        m = SENT_RELAY.match(e.text)
        return f"> relay {'ON' if m[1] == '1' else 'OFF'} (HoloLifter #HLF{m[1]})" if m else None
    if e.dir == "#":
        if e.text.startswith("ws error"):
            return "meshlog: DroidNet unreachable (lines are missing)"
        return "meshlog connected" if e.text == "connected" else "# " + e.text
    t = e.text
    if "WCB16" in t:
        return t.replace("[ETM] ", "").split(" (src MAC")[0]
    if "#HL:FANDIAG" in t:
        d = parse_fandiag(t)
        return "FANDIAG " + " ".join(f"{k}={d[k]}" for k in ("st", "rxb", "tx", "rst", "chg", "v0") if k in d)
    if t.startswith("#HL:FAN "):
        d = kv_fields(t[8:])
        return "FAN " + " ".join(f"{k}={d[k]}" for k in ("st", "play", "name", "vol", "br", "loop", "err") if k in d)
    if t.startswith("#HL:FANDCIM"):
        return t[4:60]
    if t.startswith("#HL:NODE") and ("id=16" in t or "name=HoloFan" in t):
        d = kv_fields(t[9:])
        return "NODE16 " + " ".join(f"{k}={d[k]}" for k in ("rx", "rej", "crc", "dup", "rt", "gu") if k in d)
    if not t.startswith(TERM):
        return None
    b = fan_body(t)
    if b.startswith("[mcu] vt:") or SAG_LINE.match(b):
        return None                          # timing traces; sag timelines go to the totals
    m = MCU_REPLY.match(b)
    if m:
        s, p = parse_setting(m[1]), parse_playinfo(m[1])
        if s:
            return f"setting vol {s['vol']}, br {s['br']}, loop {s['loop']}, ble {s['ble']}, angle {s['angle']}"
        if p:
            return f"playinfo {p['name']} {p['state']} {p['prog']}/{p['total']}"
        return "MCU " + m[1][:120]
    return b[:180]


def digest(entries):
    """Time-ordered digest lines; identical neighbours collapse to one 'xN'."""
    rows = []                                # [t, text, count, last_t]
    for e in entries:
        d = describe(e)
        if d is None:
            continue
        if rows and rows[-1][1] == d:
            rows[-1][2] += 1
            rows[-1][3] = e.t
        else:
            rows.append([e.t, d, 1, e.t])
    out = []
    for t, d, n, last in rows:
        more = f"   (x{n}, to {fmt_clock(last)})" if n > 1 else ""
        out.append(f"{fmt_clock(t)}  {d}{more}")
    return out


# ------------------------------------------------------------------- totals

TEST_START = re.compile(r"\[test\] (?:(\d+): )?volume 3 for (\d+) s with the radio ([\w-]+), then volume (\d)")
TEST_DONE = re.compile(r"\[test\] volume 3 for \d+ s with the radio [\w-]+: (no reset; .*)")
TEST_STOP = re.compile(r"\[test\] (STOPPED: .*)")
TEST_RESET = re.compile(r"\[test\] (RESET during (#HLFY\d+) \(test (\d+)\): .*)")
BOOT_GAP_S = 5.0          # one boot sends 3 announces ~1.2 s apart
REJOIN_S = 10.0           # announces this close to the end of an r=0/2 hold are the radio coming back


def analyze(entries):
    """One pass: boot announces, rst values, FANDIAG tx drops, #HLFY tests,
    UNPROMPTED reports, boot records, sag runs, probe runs, meshlog outages and
    the runbook's stop rules."""
    a = SimpleNamespace(boots=[], rst=[], txdrops=[], tests=[], unprompted=[], bootrec=[],
                        sag=[], probes=[], outages=[], stops=[])
    test = None                              # the open #HLFY test
    last_closed = None
    sag = None
    hlfy = relay_on = zx = None              # (t, cmd) of the last such command sent
    tx_prev = None
    ws = None                                # the open meshlog outage

    def close_test(t, outcome):
        nonlocal test, last_closed
        test.end, test.outcome = t, outcome
        last_closed, test = test, None

    def boot_cause(t):
        prev = a.boots[-1].t if a.boots else None
        if test:
            if test.radio != "on" and t >= test.t + test.seconds - 2:
                return "radio back after the #HLFY hold (not a reset)"
            return f"during #HLFY test {test.label}: ESP reset?"
        if last_closed and last_closed.radio != "on" and t - last_closed.end <= REJOIN_S:
            return "radio back after the #HLFY hold (not a reset)"
        if relay_on and 0 <= t - relay_on[0] <= 90:
            return f"power-on (relay #HLF1 at {fmt_clock(relay_on[0])})"
        if zx and (prev is None or zx[0] > prev) and t - zx[0] <= 7200:
            return f"after maintenance/OTA ({zx[1]} at {fmt_clock(zx[0])})"
        return "UNEXPLAINED: ESP reset?"

    for e in entries:
        if e.dir == "#":
            if e.text.startswith("ws error"):
                if ws is None:
                    ws = SimpleNamespace(t=e.t, end=e.t, n=0)
                    a.outages.append(ws)
                ws.end, ws.n = e.t, ws.n + 1
            elif e.text == "connected":
                ws = None
            continue
        if e.dir == "sent":
            m = SENT_FAN.match(e.text)
            cmd = m[1] if m else ""
            if cmd.startswith("#HLFY"):
                hlfy = (e.t, cmd)
            if cmd.startswith("#HLZX"):
                zx = (e.t, cmd)
            m = SENT_RELAY.match(e.text)
            if m and m[1] == "1":
                relay_on = (e.t, "#HLF1")
            continue
        t = e.text
        b = fan_body(t)
        if "WCB16 came ONLINE (boot)" in t:
            if a.boots and e.t - a.boots[-1].last <= BOOT_GAP_S:
                a.boots[-1].last, a.boots[-1].n = e.t, a.boots[-1].n + 1
            else:
                cause = boot_cause(e.t)
                a.boots.append(SimpleNamespace(t=e.t, last=e.t, n=1, cause=cause))
                if "reset?" in cause:
                    a.stops.append(f"{fmt_clock(e.t)} boot announce, {cause}")
            continue
        if "#HL:FANDIAG" in t:
            d = parse_fandiag(t)
            if d.get("rst", "").isdigit():
                a.rst.append((e.t, int(d["rst"]), "FANDIAG"))
            if d.get("tx", "").isdigit():
                tx = int(d["tx"])
                if tx_prev and tx < tx_prev[1]:
                    a.txdrops.append(f"{fmt_clock(tx_prev[0])} tx={tx_prev[1]} -> {fmt_clock(e.t)} tx={tx}")
                tx_prev = (e.t, tx)
            continue
        if not t.startswith(TERM):
            continue
        if "UNPROMPTED" in b:
            a.unprompted.append((e.t, b))
        if b.startswith("[fan] MCU at connect") or b.startswith("[fan] MCU first settings"):
            a.bootrec.append((e.t, b[6:]))
        m = TEST_START.search(b)
        if m:
            if test:
                close_test(e.t, "no closing line seen (ESP reset, or the log missed it)")
            cmd = hlfy[1] if hlfy and 0 <= e.t - hlfy[0] <= 30 else "#HLFY?"
            test = SimpleNamespace(t=e.t, label=m[1] or "?", seconds=int(m[2]), radio=m[3], cmd=cmd,
                                   end=None, outcome=None, notes=[])
            a.tests.append(test)
            continue
        m = TEST_DONE.search(b) or TEST_STOP.search(b) or TEST_RESET.search(b)
        if m:
            if TEST_RESET.search(b):
                a.rst.append((e.t, int(re.search(r"rst=(\d+)", b)[1]) if "rst=" in b else -1, "RESET record"))
                a.stops.append(f"{fmt_clock(e.t)} {b}")
            if "RESTORE FAILED" in b:
                a.stops.append(f"{fmt_clock(e.t)} {b}")
            if not test:                     # its start is not in this log (or was already closed)
                test = SimpleNamespace(t=None, label="?", seconds=0, radio="?", cmd="#HLFY?",
                                       end=None, outcome=None, notes=[])
                a.tests.append(test)
            close_test(e.t, m[1])
            continue
        if b.startswith("[test]") and test:
            test.notes.append(b[7:])
            if "HOLD INTERRUPTED" in b:
                a.stops.append(f"{fmt_clock(e.t)} {b}")
            continue
        m = SAG_START.search(b)
        if m:
            sag = SimpleNamespace(t=e.t, lo=int(m[1]), hi=int(m[2]), seconds=int(m[3]), levels={},
                                  det=None, timeline={})
            a.sag.append(sag)
            continue
        s = parse_sag_summary(b)
        if s:
            if sag is None or sag.levels:    # the start line is not in this log
                sag = SimpleNamespace(t=e.t - s["seconds"], lo=min(s["levels"], default=3),
                                      hi=max(s["levels"], default=7), seconds=s["seconds"], levels={},
                                      det=None, timeline={})
                a.sag.append(sag)
            sag.levels, sag.det = s["levels"], s["det"]
            if any(sag.levels.get(l, (0, 0))[0] > 0 for l in (6, 7)):
                a.stops.append(f"{fmt_clock(e.t)} [sag] L6/L7 > 0 (run started {fmt_clock(sag.t)})")
            continue
        tl = parse_sag_timeline(b)
        if tl is not None:
            if sag:
                sag.timeline.update(tl)
            continue
        p = parse_probe(b)
        if p:
            a.probes.append((e.t, p))
            if any(p["levels"].get(l, (0, 0))[0] > 0 for l in (6, 7)):
                a.stops.append(f"{fmt_clock(e.t)} [probe] {p['dBm']} dBm: L6/L7 > 0")
            continue
        if "TX power NOT restored" in b:
            a.stops.append(f"{fmt_clock(e.t)} {b}")
    for t, v, src in a.rst:
        if v == 9:
            a.stops.append(f"{fmt_clock(t)} rst=9 (brownout) in {src}")
    a.stops.sort()
    return a


def wrap(items, indent="      ", width=100):
    lines, cur = [], indent
    for it in items:
        if len(cur) + len(it) + 2 > width and cur.strip():
            lines.append(cur.rstrip(", "))
            cur = indent
        cur += it + ", "
    if cur.strip():
        lines.append(cur.rstrip(", "))
    return lines


def totals(a):
    """The totals block of the summary, from analyze()."""
    out = [f"boot announces (WCB16 came ONLINE (boot); most of these broadcasts are lost): {len(a.boots)}"]
    for b in a.boots:
        out.append(f"  {fmt_clock(b.t)}  x{b.n}  {b.cause}")
    if a.txdrops:
        out.append("FANDIAG tx counter went down (the ESP rebooted in between, seen or not):")
        out += [f"  {d}" for d in a.txdrops]
    if a.rst:
        runs = []                            # (value, first t, count) per change
        for t, v, src in a.rst:
            if runs and runs[-1][0] == v:
                runs[-1][2] += 1
            else:
                runs.append([v, t, 1])
        out.append("rst values: " + ", ".join(f"{v} (x{n} from {fmt_clock(t)})" for v, t, n in runs))
    else:
        out.append("rst values: none seen (FANDIAG replies are lossy: query #HLFQ)")
    out.append(f"#HLFY tests: {len(a.tests)}")
    for x in a.tests:
        start = fmt_clock(x.t) if x.t is not None else "  (start not in log)"
        out.append(f"  {start}  {x.cmd}  test {x.label}, radio {x.radio}, {x.seconds} s -> "
                   f"{x.outcome or 'open: no closing line yet (running, or the ESP reset)'}")
        out += [f"      {n}" for n in x.notes]
    out.append(f"UNPROMPTED reports: {len(a.unprompted)}")
    out += [f"  {fmt_clock(t)}  {b}" for t, b in a.unprompted]
    out.append(f"boot records: {len(a.bootrec)}")
    out += [f"  {fmt_clock(t)}  {b}" for t, b in a.bootrec]
    out.append(f"sag runs: {len(a.sag)}")
    for s in a.sag:
        deep = sag_deepest(s.levels, s.timeline)
        lv = " ".join(f"L{l}={s.levels[l][0]}" for l in sorted(s.levels))
        det = f", det {s.det}" if s.det is not None else ""
        out.append(f"  {fmt_clock(s.t)}  L{s.lo}-{s.hi} {s.seconds} s: {lv or 'no report yet'}{det}; "
                   + (f"deepest L{deep} ({VOLTS.get(deep, '?')} V)" if deep else "nothing crossed"))
        if s.timeline:
            out += wrap([f"s{sec} {fmt_clock(s.t + sec, 0)} L{lvl}" for sec, lvl in sorted(s.timeline.items())])
    out.append(f"probe runs: {len(a.probes)}")
    for dbm in sorted({p["dBm"] for _, p in a.probes}, reverse=True):
        for t, p in a.probes:
            if p["dBm"] == dbm:
                lv = "  ".join(f"L{l} {c}({i})" for l, (c, i) in sorted(p["levels"].items()))
                out.append(f"  {dbm:2d} dBm  {fmt_clock(t)}  crossed(idle) of {p['frames']}: {lv}")
    if a.outages:
        out.append("meshlog outages (lines in these spans are missing):")
        out += [f"  {fmt_clock(o.t)} - {fmt_clock(o.end)}  ({o.n} ws errors)" for o in a.outages]
    out.append("STOP / CHECK: " + ("none" if not a.stops else ""))
    out += [f"  {s}" for s in a.stops]
    return out


def summary(lines, since=None, until=None):
    entries = parse_log(lines)
    if since is not None:
        entries = [e for e in entries if e.t % 86400 >= since]
    if until is not None:
        entries = [e for e in entries if e.t % 86400 <= until]
    dig = digest(entries)
    return [f"== digest ({len(dig)} lines of {len(entries)}) =="] + dig + ["", "== totals =="] + totals(analyze(entries))


# ------------------------------------------------------- follow / run / query

class Follower:
    """New complete lines of a growing log file, from its end at creation."""

    def __init__(self, path):
        self.f = open(path, "r", encoding="utf-8", errors="replace")
        self.f.seek(0, os.SEEK_END)
        self.part = ""

    def lines(self):
        chunk = self.f.read()
        if not chunk:
            return []
        self.part += chunk
        *done, self.part = self.part.split("\n")
        return done

    def close(self):
        self.f.close()


class Watch:
    """Prints the fan lines of a follow with +seconds since t0 (log time)."""

    def __init__(self, t0, out):
        self.t0, self.out, self.ws = clock_s(t0), out, 0

    def show(self, line, note=""):
        """Print line if it is about the fan (or note is set); True if printed."""
        e = parse_line(line)
        if e and e.dir == "#" and e.text.startswith("ws error"):
            self.ws += 1
            if self.ws == 1:
                self.out(f"  !! {line}   (meshlog lost DroidNet: lines are missing until it reconnects)")
            return False
        if not note and not is_fan_line(line):
            return False
        rel = "    ?  "
        if e:
            d = e.t - self.t0
            d += 86400 if d < -43200 else 0
            rel = f"+{d:6.1f}s"
        self.out(f"{rel}  {line}{note}")
        return True


def send_all(send, cmds, gap=0.4, out=print, sleep=time.sleep, now=time.time):
    good = True
    for i, c in enumerate(cmds):
        if i:
            sleep(gap)
        ok = send(c)
        out(f"{stamp(now())} >>> {c} {ok}")
        good = good and ok is True
    return 0 if good else 1


def run_until(send, follow, cmds, until, fail="refused", timeout=60.0, gap=1.0, tail=TAIL_S,
              out=print, now=time.time, sleep=time.sleep, poll=0.2):
    """Send cmds (gap s apart), then print the fan's lines until one matches
    `until` (0, plus `tail` s more), one matches `fail` (3) or timeout (2)."""
    rx_until, rx_fail = re.compile(until), re.compile(fail) if fail else None
    t0 = now()
    watch = Watch(t0, out)
    for i, c in enumerate(cmds):
        if i:
            sleep(gap)
        ok = send(c)
        out(f"{stamp(now())} >>> {c} {ok}")
        if ok is not True:
            return 1
    status = end = None
    while True:
        for line in follow.lines():
            e = parse_line(line)
            note = ""
            if status is None and e and matchable(e):
                if rx_until.search(e.text):
                    status, note = 0, "   <== until"
                elif rx_fail and rx_fail.search(e.text):
                    status, note = 3, "   <== fail"
                if status is not None:
                    end = now() + tail
            watch.show(line, note)
        if end is not None and now() >= end:
            break
        if status is None and now() - t0 >= timeout:
            out(f"timeout after {timeout:.0f} s: nothing matched {until!r}")
            return 2
        sleep(poll)
    if watch.ws > 1:
        out(f"  !! {watch.ws} meshlog ws errors meanwhile")
    out(f"{'matched' if status == 0 else 'a --fail line came first'}; done after {now() - t0:.1f} s")
    return status


def query(send, follow, cmd, expect, tries=12, gap=1.0, grace=2.0, out=print, now=time.time,
          sleep=time.sleep, poll=0.1):
    """Send cmd up to `tries` times, `gap` s apart, until a line matches
    `expect` (0); 2 if none did (the last try waits max(gap, grace))."""
    rx = re.compile(expect)
    t0 = now()
    watch = Watch(t0, out)
    sent_ok = 0
    for i in range(tries):
        ok = send(cmd)
        sent_ok += ok is True
        out(f"{stamp(now())} >>> {cmd} {ok} ({i + 1}/{tries})")
        deadline = now() + (gap if i + 1 < tries else max(gap, grace))
        while True:
            for line in follow.lines():
                e = parse_line(line)
                if e and matchable(e) and rx.search(e.text):
                    watch.show(line, "   <== expect")
                    out(f"matched after {i + 1} send(s), {now() - t0:.1f} s")
                    return 0
                watch.show(line)
            if now() >= deadline:
                break
            sleep(poll)
    out(f"nothing matched {expect!r} after {tries} sends ({sent_ok} accepted by DroidNet)")
    return 2 if sent_ok else 1


def mark_line(label, epoch=None, at=None):
    """'HH:MM:SS.mmm # MARK <label>' at epoch (now) or at a time of day `at` (s)."""
    clock = fmt_clock(at, 3) if at is not None else stamp(epoch if epoch is not None else time.time())
    return f"{clock} # MARK {label}"


def append_mark(path, label, at=None, now=time.time):
    line = mark_line(label, now(), at)
    with open(path, "a", encoding="utf-8") as f:
        f.write(line + "\n")
    return line


# --------------------------------------------------------------------- main

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="action", required=True)
    net = argparse.ArgumentParser(add_help=False)
    net.add_argument("--host", default=os.environ.get("HFBENCH_HOST"),
                     help="the DroidNet Pi of the WCB to send through (default $HFBENCH_HOST; resolved once, IPv4)")
    net.add_argument("--host-ip", help="its IPv4 address: skips the name lookup")
    logp = argparse.ArgumentParser(add_help=False)
    logp.add_argument("--log", default=DEFAULT_LOG, help="the meshlog.py log (default $HFLOG or /tmp/hf_mesh.log)")

    s = sub.add_parser("send", parents=[net], help="send commands, 0.4 s apart")
    s.add_argument("cmd", nargs="+")
    s.add_argument("--gap", type=float, default=0.4, help="seconds between commands")
    r = sub.add_parser("run", parents=[net, logp], help="send, then follow the log until a regex matches")
    r.add_argument("cmd", nargs="+")
    r.add_argument("--until", required=True, help="regex that ends the run (exit 0)")
    r.add_argument("--fail", default="refused", help="regex that ends it with exit 3 ('' = none)")
    r.add_argument("--timeout", type=float, default=60.0)
    r.add_argument("--gap", type=float, default=1.0, help="seconds between several commands")
    q = sub.add_parser("query", parents=[net, logp], help="resend a query until its reply shows")
    q.add_argument("cmd")
    q.add_argument("--expect", required=True, help="regex of the reply")
    q.add_argument("--tries", type=int, default=12)
    q.add_argument("--gap", type=float, default=1.0)
    m = sub.add_parser("mark", parents=[logp], help="append '# MARK <label>' to the log")
    m.add_argument("label", nargs="+")
    m.add_argument("--at", help="time of day HH:MM[:SS[.s]] (default now)")
    su = sub.add_parser("summary", help="digest and totals of a log")
    su.add_argument("file", nargs="?", default=DEFAULT_LOG)
    su.add_argument("--since", help="HH:MM[:SS[.s]]")
    su.add_argument("--until", help="HH:MM[:SS[.s]]")
    args = p.parse_args(argv)

    try:
        if args.action == "summary":
            since = parse_clock(args.since) if args.since else None
            until = parse_clock(args.until) if args.until else None
            with open(args.file, encoding="utf-8", errors="replace") as f:
                print("\n".join(summary(f, since, until)))
            return 0
        if args.action == "mark":
            at = parse_clock(args.at) if args.at else None
            print(append_mark(args.log, " ".join(args.label), at))
            return 0
        cmds = [fan_command(c) for c in (args.cmd if isinstance(args.cmd, list) else [args.cmd])]
        if args.action in ("run", "query"):
            re.compile(args.until if args.action == "run" else args.expect)
            re.compile(getattr(args, "fail", "") or "")
            if not os.path.exists(args.log):
                print(f"no log {args.log}: start it first: python3 tools/bench/meshlog.py <host> {args.log} &")
                return 1
    except (ValueError, re.error, OSError) as e:
        print(f"error: {e}")
        return 1
    if not args.host and not args.host_ip:
        print("no host: pass --host <DroidNet Pi> (or --host-ip A.B.C.D), or set HFBENCH_HOST")
        return 1
    try:
        send = wcb_sender(args.host, args.host_ip)
    except OSError as e:
        print(f"cannot resolve {args.host}: {e} (pass --host-ip)")
        return 1
    if not args.host_ip and getattr(send, "ip", None):
        print(f"{args.host} -> {send.ip} (--host-ip {send.ip} skips the lookup)")
    if args.action == "send":
        return send_all(send, cmds, args.gap)
    follow = Follower(args.log)
    try:
        if args.action == "run":
            return run_until(send, follow, cmds, args.until, args.fail or None, args.timeout, args.gap)
        return query(send, follow, cmds[0], args.expect, args.tries, args.gap)
    finally:
        follow.close()


if __name__ == "__main__":
    sys.exit(main())
