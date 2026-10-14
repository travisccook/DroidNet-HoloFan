#!/usr/bin/env python3
"""Tests for clip_put.py against an in-process fake fan (session protocol,
docs/MAINTENANCE_API.md) with injected faults. Run:
    python3 -B -m unittest discover -s tools/bench -p 'test_clip_put.py'
"""
import json
import os
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import clip_put  # noqa: E402


class FakeFan:
    def __init__(self):
        self.lock = threading.Lock()
        self.sess = None
        self.drop_put = {}          # PUT index (0-based) -> stage this many bytes, then drop without a reply
        self.lose_post_replies = 0  # create the session, then drop the reply this many times
        self.polls_until_ok = 2     # GETs in "sending" before "ok"
        self.reboot_after_put = None
        self.post_text_404 = False  # a fan without /api/upload: httpd's plain-text 404
        self.puts = 0
        self.opens = 0
        self.post_queries = []      # the query dict of every POST


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, status, obj):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def query(self):
        u = urlsplit(self.path)
        return u.path, {k: v[0] for k, v in parse_qs(u.query).items()}

    def do_POST(self):
        fan = self.server.fan
        path, q = self.query()
        if fan.post_text_404:
            body = b"Nothing matches the given URI"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        name, size = q["name"], int(q["size"])
        with fan.lock:
            fan.post_queries.append(q)
            s = fan.sess
            if s and s["state"] in ("prefill", "sending"):
                if s["name"] == name and s["size"] == size:
                    out = (200, {"ok": True, "id": s["id"], "staged": len(s["buf"]), "size": size})
                else:
                    out = (409, {"ok": False, "err": "busy", "name": s["name"], "state": s["state"]})
            else:
                fan.opens += 1
                fan.sess = s = {"id": 77, "name": name, "size": size, "buf": bytearray(), "state": "prefill",
                                "polls": 0, "err": ""}
                out = (200, {"ok": True, "id": 77, "staged": 0, "size": size})
            lose = fan.lose_post_replies > 0
            if lose:
                fan.lose_post_replies -= 1
        if lose:
            self.close_connection = True
            return
        self.reply(*out)

    def do_PUT(self):
        fan = self.server.fan
        path, q = self.query()
        n = int(self.headers["Content-Length"])
        with fan.lock:
            s = fan.sess
            idx = fan.puts
            fan.puts += 1
            drop = fan.drop_put.get(idx)
        if not s or int(q["id"]) != s["id"]:
            self.close_connection = True
            return self.reply(404, {"ok": False, "err": "id"})
        if s["state"] not in ("prefill", "sending"):
            self.close_connection = True
            return self.reply(409, {"ok": False, "err": "state", "state": s["state"], "staged": len(s["buf"])})
        off = int(q["off"])
        if off > len(s["buf"]):
            self.close_connection = True
            return self.reply(409, {"ok": False, "err": "offset", "staged": len(s["buf"])})
        body = self.rfile.read(n if drop is None else min(drop, n))
        with fan.lock:
            skip = len(s["buf"]) - off
            s["buf"] += body[skip:] if skip < len(body) else b""
            if len(s["buf"]) >= s["size"]:
                s["state"] = "sending"
            if fan.reboot_after_put is not None and idx >= fan.reboot_after_put:
                fan.sess = None
        if drop is not None:
            self.close_connection = True
            return
        self.reply(200, {"ok": True, "staged": len(s["buf"])})

    def do_GET(self):
        fan = self.server.fan
        path, q = self.query()
        if path == "/api/info":
            return self.reply(200, {"fw": "fake", "last": "UPL fake ok"})
        with fan.lock:
            s = fan.sess
            if not s:
                return self.reply(200, {"state": "none"})
            if s["state"] == "sending" and len(s["buf"]) >= s["size"]:
                s["polls"] += 1
                if s["polls"] > fan.polls_until_ok:
                    s["state"] = "ok"
            js = {"state": s["state"], "id": s["id"], "name": s["name"], "size": s["size"],
                  "staged": len(s["buf"]), "ring": 0, "sent": 0, "err": s["err"], "partial": False,
                  "link": {"recon": 0}}
        self.reply(200, js)

    def do_DELETE(self):
        fan = self.server.fan
        with fan.lock:
            s = fan.sess
            if s and s["state"] in ("prefill", "sending"):
                s["state"], s["err"] = "failed", "abort"
            st = s["state"] if s else "none"
        self.reply(200, {"ok": True, "state": st})


class ClipPutTest(unittest.TestCase):
    def setUp(self):
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.srv.fan = FakeFan()
        self.fan = self.srv.fan
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.srv.server_address[1]}"
        self.data = bytes((i * 7 + 1) & 0xFF for i in range(200_000))
        self.lines = []

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def run_upload(self, **kw):
        fan = clip_put.Fan(self.base, timeout=5.0)
        return clip_put.upload(fan, self.data, "03-Rings.bin", chunk=32768, log=self.lines.append,
                               sleep=lambda s: None, poll=0, **kw)

    def test_clean_upload(self):
        self.assertEqual(0, self.run_upload())
        self.assertEqual(self.data, bytes(self.fan.sess["buf"]))
        self.assertEqual("ok", self.fan.sess["state"])

    def test_post_carries_timing_knobs_only_when_given(self):
        self.assertEqual(0, self.run_upload())
        self.assertEqual({"name", "size"}, set(self.fan.post_queries[0]))
        self.fan.sess = None
        self.assertEqual(0, self.run_upload(pace="burst", igap=0, pre=20, dry=60000))
        q = self.fan.post_queries[-1]
        self.assertEqual({"pace": "burst", "igap": "0", "pre": "20", "dry": "60000"},
                         {k: q[k] for k in ("pace", "igap", "pre", "dry")})

    def test_post_carries_reserve_and_keep(self):
        self.assertEqual(0, self.run_upload(pace="reserve", keep=3000))
        q = self.fan.post_queries[-1]
        self.assertEqual({"pace": "reserve", "keep": "3000"}, {k: q[k] for k in ("pace", "keep")})
        self.assertNotIn("igap", q)

    def test_post_carries_dark_only_when_asked(self):
        self.assertEqual(0, self.run_upload())
        self.assertNotIn("dark", self.fan.post_queries[-1])
        self.fan.sess = None
        self.assertEqual(0, self.run_upload(dark=True))
        self.assertEqual("1", self.fan.post_queries[-1]["dark"])   # 0.9.8: black clip / brightness 1 while uploading

    def test_resumes_after_dropped_chunks(self):
        self.fan.drop_put = {1: 5000, 3: 0, 4: 20000}
        self.assertEqual(0, self.run_upload())
        self.assertEqual(self.data, bytes(self.fan.sess["buf"]))
        # Verify resends happened: clean upload needs ceil(200000/32768)=7 PUTs, this should have more
        self.assertGreater(self.fan.puts, 7)

    def test_retries_lost_post_reply(self):
        self.fan.lose_post_replies = 2
        self.assertEqual(0, self.run_upload())
        self.assertEqual(1, self.fan.opens)            # the retried POSTs found the same session
        self.assertEqual(self.data, bytes(self.fan.sess["buf"]))

    # Review focus 5.
    def test_reports_lost_session(self):
        self.fan.reboot_after_put = 2
        self.assertEqual(1, self.run_upload())
        self.assertTrue(any("force=1" in l for l in self.lines), self.lines)
        self.assertEqual(1, self.fan.opens)            # no silent second session

    def test_busy_is_refused(self):
        self.fan.sess = {"id": 5, "name": "09-Other.bin", "size": 10, "buf": bytearray(), "state": "prefill",
                         "polls": 0, "err": ""}
        self.assertEqual(1, self.run_upload())
        self.assertTrue(any("busy" in l for l in self.lines), self.lines)

    # Final review M3: a real HTTP status with a non-JSON body is a refusal, not a network error.
    def test_text_404_open_is_refused_quickly(self):
        self.fan.post_text_404 = True
        self.assertEqual(1, self.run_upload(giveup=2.0))
        self.assertTrue(any("open refused" in l for l in self.lines), self.lines)
        self.assertFalse(any("gave up" in l for l in self.lines), self.lines)

    def test_abort(self):
        self.fan.sess = {"id": 5, "name": "09-Other.bin", "size": 10, "buf": bytearray(), "state": "sending",
                         "polls": 0, "err": ""}
        fan = clip_put.Fan(self.base, timeout=5.0)
        self.assertEqual(0, clip_put.abort(fan, log=self.lines.append, sleep=lambda s: None))
        self.assertEqual("failed", self.fan.sess["state"])


if __name__ == "__main__":
    unittest.main()
