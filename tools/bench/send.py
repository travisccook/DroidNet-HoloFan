#!/usr/bin/env python3
"""Fire-and-forget WCB commands through DroidNet. Usage: send.py HOST cmd [cmd ...] (0.4 s apart)"""
import json, sys, time, urllib.request
host = sys.argv[1]
for c in sys.argv[2:]:
    body = json.dumps({"port": "/dev/ttyUSB0", "command": c, "expect_reply": False}).encode()
    req = urllib.request.Request(f"http://{host}.local/api/wcb/command", body, {"Content-Type": "application/json"})
    ok = json.load(urllib.request.urlopen(req, timeout=10)).get("success")
    print(time.strftime("%H:%M:%S"), ">>>", c, ok)
    time.sleep(0.4)
