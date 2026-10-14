#!/usr/bin/env python3
"""Log every line of a WCB's USB console (DroidNet read-only WebSocket) with a timestamp.
Usage: meshlog.py HOST LOGFILE. Reconnects on drop."""
import asyncio, json, sys, time, websockets
host, path = sys.argv[1], sys.argv[2]
WS = f"ws://{host}.local:8080/api/serial/ws?port=/dev/ttyUSB0&baud=115200&readonly=true"
def log(s):
    with open(path, "a") as f:
        f.write(time.strftime("%H:%M:%S") + f".{int(time.time()*1000)%1000:03d} " + s + "\n")
async def main():
    while True:
        try:
            async with websockets.connect(WS, open_timeout=10) as ws:
                log("# connected")
                async for m in ws:
                    m = m if isinstance(m, str) else m.decode(errors="replace")
                    try:
                        j = json.loads(m)
                        if j.get("replay") or "line" not in j: continue
                        log(f"{j.get('direction','')[:4]} {j['line'][:400]}")
                    except ValueError:
                        log("raw " + m[:400])
        except Exception as e:
            log(f"# ws error {e!r}; reconnecting")
            await asyncio.sleep(2)
asyncio.run(main())
