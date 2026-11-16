#!/usr/bin/env python3
"""Fire-and-forget WCB commands through DroidNet. Usage: send.py HOST cmd [cmd ...] (0.4 s apart)"""
import json, sys, time, urllib.request
if len(sys.argv) < 3: sys.exit(__doc__)
host = sys.argv[1]
failed = 0
for c in sys.argv[2:]:
    body = json.dumps({"port": "/dev/ttyUSB0", "command": c, "expect_reply": False}).encode()
    req = urllib.request.Request(f"http://{host}.local/api/wcb/command", body, {"Content-Type": "application/json"})
    try: ok = json.load(urllib.request.urlopen(req, timeout=10)).get("success")
    except Exception as e: ok = repr(e)    # a timeout or a refused connection: say so, send the rest
    failed += ok is not True
    print(time.strftime("%H:%M:%S"), ">>>", c, ok)
    time.sleep(0.4)
if failed: sys.exit(f"{failed} of {len(sys.argv) - 2} commands were not sent")
