#!/usr/bin/env python3
"""Soak: every GAP s send #HLFXv (RTERM liveness); every 3rd round also #HLFQ (broadcast diag).
Summarise RTERM answers, longest RTERM silence, BOOT announces and FANDIAG tx/rst over the run.
Usage: soak.py HOST SECONDS GAP LOGFILE LABEL"""
import json, re, sys, time, urllib.request
host, secs, gap, logf, label = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4], sys.argv[5]
def post(c):
    body = json.dumps({"port": "/dev/ttyUSB0", "command": c, "expect_reply": False}).encode()
    req = urllib.request.Request(f"http://{host}.local/api/wcb/command", body, {"Content-Type": "application/json"})
    try: return json.load(urllib.request.urlopen(req, timeout=10)).get("success")
    except Exception as e: return repr(e)
def mark(s):
    with open(logf, "a") as f: f.write(time.strftime("%H:%M:%S") + f".000 # {s}\n")
mark(f"soak {label} start"); t0 = time.time(); i = 0
def tripped():
    seg = open(logf).read().split(f"# soak {label} start")[-1]
    return "WCB16 came ONLINE (boot)" in seg or "NOMCU" in seg
while time.time() - t0 < secs:
    if tripped(): mark(f"soak {label} tripped at {time.time() - t0:.0f} s"); break
    post(";wHoloFan,#HLFXv")
    if i % 3 == 2: time.sleep(0.6); post(";wHoloFan,#HLFQ")
    i += 1; time.sleep(gap)
time.sleep(3); mark(f"soak {label} end")
lines = open(logf).read().splitlines()
start = max(k for k, l in enumerate(lines) if f"# soak {label} start" in l); seg = lines[start:]
def ts(l):
    h, m, s = l[:12].split(":"); return int(h) * 3600 + int(m) * 60 + float(s)
rt = [ts(l) for l in seg if "[TERM:16][mcu] refused" in l]
sent = sum("sent ;wHoloFan,#HLFXv" in l for l in seg)
gaps = [b - a for a, b in zip(rt, rt[1:])]
print(f"{label}: RTERM {len(rt)}/{sent}, longest RTERM gap {max(gaps) if gaps else 0:.1f} s")
for l in seg:
    if "WCB16" in l or "NOMCU" in l: print(" ", l[:160])
for l in seg:
    m = re.search(r"FANDIAG st=(\w+) rxb=(\d+) tx=(\d+).* rst=(\d+)", l)
    if m: print(f"  {l[:8]} st={m[1]} rxb={m[2]} tx={m[3]} rst={m[4]}")
