#!/usr/bin/env python3
"""Watch a WCB's USB lines via DroidNet's read-only serial WebSocket while sending
fire-and-forget commands through POST /api/wcb/command.
Usage: wcbwatch.py HOST SECONDS_PER_CMD cmd1 cmd2 ..."""
import asyncio, json, sys, urllib.request, websockets
host, per, cmds = sys.argv[1], float(sys.argv[2]), sys.argv[3:]
WS = f"ws://{host}.local:8080/api/serial/ws?port=/dev/ttyUSB0&baud=115200&readonly=true"
def post(cmd):
    body = json.dumps({"port": "/dev/ttyUSB0", "command": cmd, "expect_reply": False}).encode()
    import os; sh = os.environ.get("SEND_HOST", host); req = urllib.request.Request(f"http://{sh}.local/api/wcb/command", body, {"Content-Type": "application/json"})
    return json.load(urllib.request.urlopen(req, timeout=10))
async def main():
    async with websockets.connect(WS, open_timeout=10) as ws:
        async def collect(sec):
            end = asyncio.get_event_loop().time() + sec
            while True:
                left = end - asyncio.get_event_loop().time()
                if left <= 0: return
                try:
                    m = await asyncio.wait_for(ws.recv(), timeout=left)
                    m = m if isinstance(m, str) else m.decode(errors='replace')
                    try:
                        j = json.loads(m)
                        if j.get("replay") or "line" not in j: continue
                        print("  <", j.get("direction", "")[:4], j["line"][:300])
                    except ValueError:
                        print("  <", m[:300])
                except asyncio.TimeoutError:
                    return
        await collect(1.5)
        for c in cmds:
            print(">>>", c, "->", post(c).get("success"))
            await collect(per)
asyncio.run(main())
