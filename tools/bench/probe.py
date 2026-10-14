#!/usr/bin/env python3
"""Alternate a broadcast-reply query (#HLZQ) and a unicast-RTERM probe (#HLFXv) to the fan,
then count the answers in the meshlog file. Usage: probe.py HOST ROUNDS GAP_S LOGFILE LABEL"""
import json, sys, time, urllib.request
host, rounds, gap, logf, label = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4], sys.argv[5]
def post(c):
    body = json.dumps({"port": "/dev/ttyUSB0", "command": c, "expect_reply": False}).encode()
    req = urllib.request.Request(f"http://{host}.local/api/wcb/command", body, {"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req, timeout=10)).get("success")
t0 = time.strftime("%H:%M:%S")
with open(logf, "a") as f: f.write(f"{t0}.000 # probe {label} start\n")
for i in range(rounds):
    post(";wHoloFan,#HLZQ"); time.sleep(gap)
    post(";wHoloFan,#HLFXv"); time.sleep(gap)
time.sleep(3)
t1 = time.strftime("%H:%M:%S")
with open(logf, "a") as f: f.write(f"{t1}.000 # probe {label} end\n")
lines = open(logf).read().splitlines()
start = max(i for i, l in enumerate(lines) if f"# probe {label} start" in l)
seg = lines[start:]
sq = sum("sent ;wHoloFan,#HLZQ" in l for l in seg); sx = sum("sent ;wHoloFan,#HLFXv" in l for l in seg)
rq = [l for l in seg if "#HL:NODE id=16" in l]; rx = sum("[TERM:16][mcu] refused" in l for l in seg)
off = [l for l in seg if "WCB16" in l]
print(f"{label} {t0}-{t1}: ZQ broadcast replies {len(rq)}/{sq}, RTERM replies {rx}/{sx}")
if rq: print(" first", rq[0][:150]); print(" last ", rq[-1][:150])
for l in off: print(" ", l)
