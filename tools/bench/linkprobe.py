#!/usr/bin/env python3
"""Measure where a HoloFan upload spends its time, without writing the SD card.

Usage: linkprobe.py BASE_URL [--bytes 700000] [--chunk 32768] [--name 09-Probe.bin]
                    [--csv FILE] [--timeout 5]
  BASE_URL e.g. http://<fan-ip>   (the fan in maintenance mode; use the IPv4
  address: a .local lookup costs 5 s on macOS)

What it does: opens a resumable upload session for a clip larger than what it
sends, stages --bytes of zeros (at most 800,000: below the 574-frame prefill,
so the fan never creates a file and never sends a frame to its LED/SD
controller), aborts the session and reads the fan's "last" line. Like every
upload it overwrites the spare OTA slot (the fan's previous firmware image).

While the chunks go out a thread samples the sending socket's TCP state every
20 ms (macOS TCP_CONNECTION_INFO, Linux TCP_INFO). The report splits the
elapsed time into:
  flash   the fan's staging flash erase + write time (fl= in "last"): the fan's
          CPU and Wi-Fi task stand still for all of it
  zero    the fan advertised a zero receive window while bytes waited here
  stuck   no byte left this socket for 1 s or longer, and what the window was
          (zero: a lost window update, the sender waits for its persist timer;
          open: lost segments, the sender waits for its retransmit timer)
  retx    bytes sent twice
The CSV has every sample for a closer look. Nothing here is a fix: it tells
which of flash, TCP flow control and radio loss to work on.
"""
import argparse
import json
import re
import socket
import struct
import sys
import threading
import time
from urllib.parse import urlencode

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import clip_put  # noqa: E402

MAX_BYTES = 800_000            # 555 frames: below the fan's 574-frame prefill
SPI_FRAME = 1440
SAMPLE_S = 0.02
STUCK_S = 1.0

# macOS <netinet/tcp.h> struct tcp_connection_info (112 bytes): 4 bytes of
# state/scales, 12 u32 (options flags rto maxseg ssthresh cwnd snd_wnd
# snd_sbbytes rcv_wnd rttcur srtt rttvar), a u32 of TFO bits, 7 u64
# (txpackets txbytes txretransmitbytes rxpackets rxbytes rxoutoforderbytes
# txretransmitpackets).
MAC_TCP_CONNECTION_INFO = 0x106
MAC_FMT = "<4x12II7Q"
# Linux include/uapi/linux/tcp.h struct tcp_info (v6.1: 232 bytes; older
# kernels return fewer, without snd_wnd).
LINUX_TCP_INFO = 11
LINUX_FMT = "<8B24I4Q6I4Q2I2Q4I"


def parse_mac(raw):
    """A sample dict from a macOS tcp_connection_info, or None if too short."""
    if len(raw) < struct.calcsize(MAC_FMT):
        return None
    f = struct.unpack_from(MAC_FMT, raw)
    return {"wnd": f[6], "queued": f[7], "rto": f[2], "srtt": f[10], "cwnd": f[5],
            "sent": f[14], "retx": f[15]}


def parse_linux(raw):
    """A sample dict from a Linux tcp_info; wnd is None on kernels without it."""
    size = struct.calcsize(LINUX_FMT)
    full = len(raw) >= size
    f = struct.unpack_from(LINUX_FMT, raw.ljust(size, b"\0"))
    u32 = f[8:32]                      # rto .. total_retrans
    mss, unacked = u32[2], u32[4]
    notsent = f[38]
    # 54 values: 8 u8, u32 8..31, u64 32..35, u32 36..41, u64 42..45
    # (delivery_rate busy_time rwnd_limited sndbuf_limited), u32 46..47,
    # u64 48..49 (bytes_sent bytes_retrans), u32 50..53 (.. snd_wnd).
    return {"wnd": f[53] if full else None, "queued": unacked * mss + notsent,
            "rto": u32[0] // 1000, "srtt": u32[15] // 1000, "cwnd": u32[18] * mss,
            "sent": f[48], "retx": f[49], "probes": f[3], "rwnd_limited_us": f[44]}


def sample(sock):
    """One TCP state sample of a connected socket, or None (unsupported / closed)."""
    try:
        if sys.platform == "darwin":
            return parse_mac(sock.getsockopt(socket.IPPROTO_TCP, MAC_TCP_CONNECTION_INFO, 256))
        if sys.platform.startswith("linux"):
            return parse_linux(sock.getsockopt(socket.IPPROTO_TCP, LINUX_TCP_INFO, 256))
    except (OSError, ValueError, AttributeError):
        pass
    return None


def parse_fl(last):
    """(erase_ms, write_ms, longest_ms) from a "last" line, or None."""
    m = re.search(r"\bfl=(\d+)/(\d+)/(\d+)", last or "")
    return tuple(int(g) for g in m.groups()) if m else None


def summarize(samples):
    """Totals over [(t, conn, sample dict)]: zero-window and stuck time, retransmits.

    zero_s: time with a zero window while bytes waited. stuck: [(start, seconds,
    window at the start)] for every stretch of STUCK_S or longer in which the
    queued byte count did not change. sent/retx: bytes, summed over connections.
    """
    zero_s = longest_zero = run_zero = 0.0
    stuck, run_from, run_wnd = [], None, None
    per_conn = {}
    prev = None
    for t, conn, s in samples:
        per_conn[conn] = (s["sent"], s["retx"])
        if prev is not None and prev[1] == conn:
            pt, _, ps = prev
            dt = t - pt
            if ps["wnd"] == 0 and ps["queued"] > 0:
                zero_s += dt
                run_zero += dt
                longest_zero = max(longest_zero, run_zero)
            else:
                run_zero = 0.0
            same = ps["queued"] > 0 and s["queued"] == ps["queued"]
            if same and run_from is None:
                run_from, run_wnd = pt, ps["wnd"]
            if not same and run_from is not None:
                if pt - run_from >= STUCK_S:
                    stuck.append((run_from, pt - run_from, run_wnd))
                run_from = None
        else:
            run_zero = 0.0
            if run_from is not None and prev is not None and prev[0] - run_from >= STUCK_S:
                stuck.append((run_from, prev[0] - run_from, run_wnd))
            run_from = None
        prev = (t, conn, s)
    if run_from is not None and prev is not None and prev[0] - run_from >= STUCK_S:
        stuck.append((run_from, prev[0] - run_from, run_wnd))
    return {"zero_s": zero_s, "longest_zero_s": longest_zero, "stuck": stuck,
            "sent": sum(v[0] for v in per_conn.values()), "retx": sum(v[1] for v in per_conn.values()),
            "conns": len(per_conn)}


class Sampler(threading.Thread):
    def __init__(self, fan, now):
        super().__init__(daemon=True)
        self.fan, self.now = fan, now
        self.samples, self.stop = [], threading.Event()

    def run(self):
        while not self.stop.wait(SAMPLE_S):
            conn = self.fan.conn
            sock = getattr(conn, "sock", None)
            if sock is None:
                continue
            s = sample(sock)
            if s is not None:
                self.samples.append((self.now(), id(conn), s))


def probe(fan, nbytes, name, chunk=32768, log=print, now=time.monotonic, sleep=time.sleep, giveup=120.0):
    """Returns (report dict, samples) or (None, samples) if the fan refused."""
    nbytes = min(nbytes, MAX_BYTES)
    size = nbytes + 1000 * SPI_FRAME                  # never complete, never prefilled
    st, info = fan.call("GET", "/api/info")
    if st != 200 or info is None:
        log("the fan did not answer GET /api/info (is it in maintenance mode?)")
        return None, []
    log(f"fan {info.get('fw')} heap {info.get('heap')} wifi {json.dumps(info.get('wifi'))}")
    st, js = fan.call("POST", "/api/upload?" + urlencode({"name": name, "size": size}))
    if st != 200 or js is None:
        log(f"open refused: {st} {json.dumps(js)}")
        return None, []
    sid, staged = js["id"], js["staged"]
    data = bytes(chunk)
    sampler = Sampler(fan, now)
    sampler.start()
    t0 = last = now()
    marks = [(0.0, staged)]
    try:
        while staged < nbytes and now() - last < giveup:
            n = min(chunk, nbytes - staged)
            st, js = fan.call("PUT", f"/api/upload?id={sid}&off={staged}", data[:n], keep=True)
            if st == 200 and js and js.get("staged", staged) > staged:
                staged, last = js["staged"], now()
            else:
                fan.close()
                sleep(1)
                st, js = fan.call("GET", "/api/upload")
                if st == 200 and js and js.get("id") == sid and js.get("state") == "prefill":
                    if js["staged"] > staged:
                        last = now()
                    staged = js["staged"]
                elif st == 200 and js:
                    log(f"the session ended early: {json.dumps(js)}")
                    break
            marks.append((now() - t0, staged))
        elapsed = now() - t0
    finally:
        sampler.stop.set()
        sampler.join()
        fan.close()
    st, status = fan.call("GET", "/api/upload")
    for _ in range(10):                               # abort, then wait for the task to end
        fan.call("DELETE", f"/api/upload?id={sid}")
        sleep(1)
        st2, js2 = fan.call("GET", "/api/upload")
        if st2 == 200 and js2 and js2.get("state") not in ("prefill", "sending"):
            break
    st, info = fan.call("GET", "/api/info")
    last_line = str(info.get("last", "")) if st == 200 and info else ""
    rep = summarize(sampler.samples)
    rep.update({"bytes": staged, "elapsed_s": elapsed, "fl": parse_fl(last_line), "last": last_line,
                "link": (status or {}).get("link") if isinstance(status, dict) else None, "marks": marks,
                "t0": t0})
    return rep, sampler.samples


def report(rep, log=print):
    el = max(rep["elapsed_s"], 0.001)
    log(f"staged {rep['bytes']:,} B in {el:.1f} s = {rep['bytes'] / el / 1024:.1f} KB/s"
        f" over {rep['conns']} connection(s)")
    if rep["fl"]:
        e, w, longest = rep["fl"]
        log(f"flash   erase {e} ms + write {w} ms = {100 * (e + w) / 1000 / el:.0f} % of the time"
            f" (longest call {longest} ms)")
    else:
        log("flash   no fl= in the fan's last line")
    log(f"zero    {rep['zero_s']:.1f} s = {100 * rep['zero_s'] / el:.0f} % with a zero window and bytes waiting"
        f" (longest {rep['longest_zero_s']:.2f} s)")
    total = sum(s[1] for s in rep["stuck"])
    log(f"stuck   {len(rep['stuck'])} stretch(es) of {STUCK_S:.0f} s or more, {total:.1f} s"
        f" = {100 * total / el:.0f} %")
    for start, secs, wnd in rep["stuck"]:
        why = "window unknown" if wnd is None else "zero window" if wnd == 0 else f"window {wnd} B open"
        log(f"          at {start - rep['t0']:.1f} s for {secs:.1f} s ({why})")
    if rep["sent"]:
        log(f"retx    {rep['retx']:,} of {rep['sent']:,} B sent = {100 * rep['retx'] / rep['sent']:.1f} %")
    log(f"fan link: {json.dumps(rep['link'])}")
    log(f"last: {rep['last']}")


def write_csv(path, samples, t0):
    keys = ["wnd", "queued", "rto", "srtt", "cwnd", "sent", "retx", "probes", "rwnd_limited_us"]
    conns = {}
    with open(path, "w") as f:
        f.write("t,conn," + ",".join(keys) + "\n")
        for t, conn, s in samples:
            c = conns.setdefault(conn, len(conns) + 1)
            f.write(f"{t - t0:.3f},{c}," + ",".join("" if s.get(k) is None else str(s[k]) for k in keys) + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("base_url")
    ap.add_argument("--bytes", type=int, default=700_000)
    ap.add_argument("--chunk", type=int, default=32768)
    ap.add_argument("--name", default="09-Probe.bin")
    ap.add_argument("--csv")
    ap.add_argument("--timeout", type=float, default=5.0)
    a = ap.parse_args(argv)
    if a.bytes > MAX_BYTES:
        print(f"--bytes is capped at {MAX_BYTES:,} (more would start a real upload to the SD card)")
    if sample(socket.socket()) is None and sys.platform not in ("darwin",) and not sys.platform.startswith("linux"):
        print("no TCP state on this platform: only the rate and the fan's own numbers will show")
    rep, samples = probe(clip_put.Fan(a.base_url, timeout=a.timeout), a.bytes, a.name, chunk=a.chunk)
    if rep is None:
        return 1
    report(rep)
    if a.csv:
        write_csv(a.csv, samples, rep["t0"])
        print(f"samples: {a.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
