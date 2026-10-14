#!/usr/bin/env python3
"""Upload a clip to the HoloFan (maintenance mode) with the resumable session API.

Usage: clip_put.py FILE BASE_URL NAME [--chunk 32768] [--mhz 10|20|40]
                   [--clk pll|xtal]
                   [--pace reserve|burst] [--keep MS] [--igap MS] [--pre N] [--dry MS] [--mss N] [--timeout 5] [--giveup 900]
       clip_put.py --abort BASE_URL
  BASE_URL e.g. http://holofan.local

Protocol (docs/MAINTENANCE_API.md, "Resumable upload"): POST /api/upload
opens a session (idempotent), PUT /api/upload?id=&off= stages bytes from an
offset, GET /api/upload reports, DELETE /api/upload aborts. From the dome the
fan's Wi-Fi has outages of ~6 to 30+ s that stall or reset TCP; on any error
this client reconnects, asks the fan how many bytes it holds (staged) and
continues from there. Standard library only.
"""
import argparse
import http.client
import json
import socket
import sys
import time
from urllib.parse import urlencode, urlsplit


class Fan:
    """HTTP to the fan: one keep-alive connection for chunks, short ones otherwise."""

    def __init__(self, base, timeout=5.0, mss=0):
        u = urlsplit(base)
        if u.scheme != "http" or not u.hostname:
            raise ValueError("BASE_URL must be http://host[:port]")
        self.host, self.port = u.hostname, u.port or 80
        self.timeout, self.mss = timeout, mss
        self.conn = None
        # Resolve once, IPv4 only. Bench 2026-10-07: getaddrinfo("holofan.local")
        # took 5.0 s on macOS (it waits for an AAAA answer the fan's mDNS never
        # gives), and every new connection paid it: 16 KB chunks crawled at
        # 2.6 KB/s; by address the link carries ~15 KB/s.
        try:
            self.addr = socket.getaddrinfo(self.host, self.port, socket.AF_INET, socket.SOCK_STREAM)[0][4][0]
        except OSError:
            self.addr = self.host

    def _connect(self):
        c = http.client.HTTPConnection(self.addr, self.port, timeout=self.timeout)
        c.connect()
        if self.mss:
            try:   # macOS: a connected socket may lower its send MSS (tcp(4) TCP_MAXSEG)
                c.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_MAXSEG, self.mss)
            except OSError:
                pass
        return c

    def close(self):
        if self.conn:
            self.conn.close()
            self.conn = None

    def call(self, method, path, body=None, keep=False):
        """(status, dict); (status, None) if a status line arrived but the body is not JSON
        (e.g. httpd's text 404); (None, None) on any network error."""
        c = None
        try:
            if keep and self.conn:
                c = self.conn
            else:
                c = self._connect()
                if keep:
                    self.conn = c
            headers = {"Content-Type": "application/octet-stream"} if body is not None else {}
            c.request(method, path, body=body, headers=headers)
            r = c.getresponse()
            data = r.read()
            if not keep or r.will_close:
                c.close()
                if keep:
                    self.conn = None
            try:
                return r.status, (json.loads(data) if data else {})
            except ValueError:
                return r.status, None
        except (OSError, http.client.HTTPException):
            if c is not None:
                c.close()
            if keep:
                self.conn = None
            return None, None


def _status(fan, sleep, gave_up):
    """GET /api/upload until it answers; None if gave_up() first."""
    while True:
        st, js = fan.call("GET", "/api/upload")
        if st == 200 and js is not None:
            return js
        if gave_up():
            return None
        sleep(1)


def _hint(name):
    return f"a partial file may remain: DELETE /api/clips/{name}?force=1, then upload again"


def _lost(log, name):
    log(f"the fan lost the session (it rebooted?); {_hint(name)}")
    return 1


def _gave_up(log, name, msg):
    """A give-up once a session exists: the fan may have rebooted into no HTTP."""
    log(f"{msg}; if the fan rebooted, {_hint(name)}")
    return 1


def upload(fan, data, name, chunk=32768, mhz=None, clk=None, giveup=900.0,
           log=print, sleep=time.sleep, now=time.monotonic, poll=2.0,
           pace=None, igap=None, pre=None, dry=None, keep=None, dark=False):
    size = len(data)
    q = {"name": name, "size": size}
    if mhz:
        q["mhz"] = mhz
    if clk:
        q["clk"] = clk
    for key, val in (("pace", pace), ("igap", igap), ("keep", keep), ("pre", pre), ("dry", dry)):
        if val is not None:                           # only what was given (timing knobs; the fan defaults to reserve, keep 3000, dry 4000)
            q[key] = val
    if dark:
        q["dark"] = 1                                 # 0.9.8: dark and silent while uploading (99-Black.bin or brightness 1)
    t0 = now()
    progress = [t0]                                   # last time staged grew

    def gave_up():
        return now() - progress[0] > giveup

    try:
        # 1. Open. Idempotent, so a lost reply is just retried.
        while True:
            st, js = fan.call("POST", "/api/upload?" + urlencode(q))
            if st == 200 and js is not None:
                sid, staged = js["id"], js["staged"]
                break
            if st is not None and st != 200 and not (st == 503 and js and js.get("err") == "busy"):
                log(f"open refused: {st} {json.dumps(js) if js is not None else '(not JSON)'}")
                return 1
            if gave_up():
                log("gave up: the fan did not answer the open")
                return 1
            sleep(1)
        log(f"session {sid}: {name} {size:,} B, the fan holds {staged:,} B")

        # 2. Chunks from staged; on anything unexpected ask the fan and continue.
        last_print = now()
        while staged < size:
            if gave_up():
                return _gave_up(log, name, f"gave up: no progress for {giveup:.0f} s at {staged:,} B")
            end = min(size, staged + chunk)
            st, js = fan.call("PUT", f"/api/upload?id={sid}&off={staged}", data[staged:end], keep=True)
            if js is None:
                st = None                                 # a non-JSON reply: resync like a failed one
            if st == 200 and js.get("staged", staged) > staged:
                progress[0] = now()
                staged = js.get("staged", staged)
            elif st == 409 and js.get("err") == "state":
                break
            elif st == 404:
                return _lost(log, name)
            elif st == 400:
                log(f"chunk refused: {json.dumps(js)}")
                return 1
            else:                                         # 200 without progress, 409 offset, 408 recv, a network error
                fan.close()
                sleep(1)
                js = _status(fan, sleep, gave_up)
                if js is None:
                    return _gave_up(log, name, f"gave up: the fan did not answer for {giveup:.0f} s")
                if js.get("state") == "none" or js.get("id") != sid:
                    return _lost(log, name)
                if js.get("state") in ("ok", "failed"):
                    break
                if js["staged"] > staged:
                    progress[0] = now()
                staged = js["staged"]
            if now() - last_print >= 10:
                last_print = now()
                st2, js2 = fan.call("GET", "/api/upload")
                el = max(now() - t0, 0.001)
                extra = ""
                if st2 == 200 and js2 is not None:
                    link = js2.get("link", {})
                    extra = (f" state={js2.get('state')} ring={js2.get('ring')} sent={js2.get('sent')}"
                             f" recon={link.get('recon')} rssi={link.get('rssi')}")
                log(f"  {staged:,}/{size:,} B ({100 * staged / size:.0f} %) {staged / el / 1024:.1f} KB/s{extra}")

        # 3. The fan finishes sending to the MCU.
        while True:
            js = _status(fan, sleep, gave_up)
            if js is None:
                return _gave_up(log, name, "gave up waiting for the result")
            if js.get("state") == "none" or js.get("id") != sid:
                return _lost(log, name)
            if js.get("state") in ("ok", "failed"):
                break
            sleep(poll)
        log(f"result: {json.dumps(js)}")
        st, info = fan.call("GET", "/api/info")
        if st == 200 and info is not None:
            log("last: " + str(info.get("last", "")))
        log(f"{size:,} B in {now() - t0:.0f} s")
        return 0 if js["state"] == "ok" else 1
    finally:
        fan.close()


def abort(fan, log=print, sleep=time.sleep, now=time.monotonic, giveup=120.0):
    t0 = now()
    try:
        while True:
            st, js = fan.call("DELETE", "/api/upload")
            if st == 200 and js is not None:
                break
            if now() - t0 > giveup:
                log("gave up: the fan did not answer the abort")
                return 1
            sleep(1)
        while js.get("state") in ("prefill", "sending"):
            if now() - t0 > giveup:
                log(f"gave up waiting for the session to end (last state={js.get('state')}); "
                    "the fan is still finishing; re-run --abort to keep waiting")
                return 1
            sleep(2)
            st, js2 = fan.call("GET", "/api/upload")
            if st == 200 and js2 is not None:
                js = js2
        log(f"state={js.get('state')} err={js.get('err')} partial={js.get('partial')}")
        return 0 if js.get("state") in ("failed", "none") else 1
    finally:
        fan.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--abort", metavar="BASE_URL", help="abort the fan's active upload session")
    ap.add_argument("file", nargs="?")
    ap.add_argument("base", nargs="?", help="e.g. http://holofan.local")
    ap.add_argument("name", nargs="?", help="NN-Name.bin on the SD card")
    ap.add_argument("--chunk", type=int, default=32768)
    ap.add_argument("--mhz", choices=["10", "20", "40"])
    ap.add_argument("--clk", choices=["pll", "xtal"])
    ap.add_argument("--pace", choices=["reserve", "burst"], help="reserve (the fan default) or window-burst (0.9.1: sessions refuse the per-second trickle)")
    ap.add_argument("--keep", type=int, metavar="MS", help="0.8.2: reserve pace: ms between windows (1000..10000, below dry)")
    ap.add_argument("--igap", type=int, metavar="MS", help="0.8.1: least ms between windows (0..60000)")
    ap.add_argument("--pre", type=int, metavar="N", help="0.8.1: frames buffered before sending (20..617; default: the ring minus one window)")
    ap.add_argument("--dry", type=int, metavar="MS", help="0.8.1: dry limit (1000..600000; default 4000 for reserve, else 30000)")
    ap.add_argument("--dark", action="store_true", help="0.9.8: the fan plays 99-Black.bin (or dims to brightness 1) while uploading")
    ap.add_argument("--mss", type=int, default=0, help="lower the send MSS after connect (macOS); 0 = leave it")
    ap.add_argument("--timeout", type=float, default=5.0, help="socket timeout per operation, s (a fresh connection beats TCP back-off)")
    ap.add_argument("--giveup", type=float, default=900.0, help="stop after this long without progress, s")
    a = ap.parse_args()
    if a.abort:
        return abort(Fan(a.abort, a.timeout, a.mss))
    if not (a.file and a.base and a.name):
        ap.error("FILE BASE_URL NAME are required (or --abort BASE_URL)")
    with open(a.file, "rb") as f:
        data = f.read()
    return upload(Fan(a.base, a.timeout, a.mss), data, a.name, a.chunk, a.mhz, a.clk, a.giveup,
                  pace=a.pace, igap=a.igap, pre=a.pre, dry=a.dry, keep=a.keep, dark=a.dark)


if __name__ == "__main__":
    sys.exit(main())
