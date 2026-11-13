#!/usr/bin/env python3
"""Tests for linkprobe.py: the parsers, the summary, and one run against the
fake fan of test_clip_put.py. Run:
    python3 -B -m unittest discover -s tools/bench -p 'test_linkprobe.py'
"""
import os
import socket
import struct
import sys
import threading
import unittest
from http.server import ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import clip_put  # noqa: E402
import linkprobe  # noqa: E402
from test_clip_put import FakeFan, Handler  # noqa: E402


def s(wnd, queued, sent=0, retx=0):
    return {"wnd": wnd, "queued": queued, "rto": 0, "srtt": 0, "cwnd": 0, "sent": sent, "retx": retx}


class ParseTest(unittest.TestCase):
    def test_struct_sizes_match_the_headers(self):
        self.assertEqual(struct.calcsize(linkprobe.MAC_FMT), 112)
        self.assertEqual(struct.calcsize(linkprobe.LINUX_FMT), 232)

    def test_mac_fields(self):
        u32 = [0, 0, 230, 536, 0, 4380, 4288, 1072, 0, 9, 11, 0]   # options .. rttvar
        raw = struct.pack(linkprobe.MAC_FMT, *u32, 0, 10, 9000, 536, 0, 0, 0, 1)
        got = linkprobe.parse_mac(raw)
        self.assertEqual((got["wnd"], got["queued"], got["rto"], got["srtt"], got["cwnd"]), (4288, 1072, 230, 11, 4380))
        self.assertEqual((got["sent"], got["retx"]), (9000, 536))
        self.assertIsNone(linkprobe.parse_mac(raw[:100]))

    def test_linux_fields_and_short_struct(self):
        f = [0] * 54
        f[3] = 2                 # tcpi_probes
        f[8] = 204000            # rto, us
        f[10] = 536              # snd_mss
        f[12] = 3                # unacked, segments
        f[38] = 100              # notsent_bytes
        f[44] = 7000             # rwnd_limited, us
        f[48], f[49], f[53] = 9000, 536, 0
        raw = struct.pack(linkprobe.LINUX_FMT, *f)
        got = linkprobe.parse_linux(raw)
        self.assertEqual((got["wnd"], got["queued"], got["rto"], got["probes"]), (0, 3 * 536 + 100, 204, 2))
        self.assertEqual((got["sent"], got["retx"], got["rwnd_limited_us"]), (9000, 536, 7000))
        self.assertIsNone(linkprobe.parse_linux(raw[:200])["wnd"])     # an older kernel: no snd_wnd

    def test_fl(self):
        self.assertEqual(linkprobe.parse_fl("UPL x abort f=0 b=700000a crc=0 wmax=0 igmax=0 stg=0 fl=6840/1210/52 req=22"),
                         (6840, 1210, 52))
        self.assertIsNone(linkprobe.parse_fl("none"))

    @unittest.skipUnless(sys.platform == "darwin" or sys.platform.startswith("linux"), "no TCP state here")
    def test_sample_of_a_real_socket(self):
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        c = socket.create_connection(srv.getsockname())
        peer, _ = srv.accept()
        try:
            c.sendall(b"x" * 1000)
            self.assertEqual(peer.recv(2000)[:1], b"x")
            got = linkprobe.sample(c)
            self.assertIsNotNone(got)
            self.assertGreaterEqual(got["sent"], 1000)
            self.assertEqual(got["retx"], 0)
            if got["wnd"] is not None:
                self.assertGreater(got["wnd"], 0)
        finally:
            c.close(), peer.close(), srv.close()


class SummaryTest(unittest.TestCase):
    def test_zero_window_time_counts_only_while_bytes_wait(self):
        samples = [(0.0, 1, s(4288, 500)), (0.1, 1, s(0, 500)), (0.3, 1, s(0, 400)), (0.4, 1, s(0, 0)),
                   (0.9, 1, s(4288, 0))]
        rep = linkprobe.summarize(samples)
        self.assertAlmostEqual(rep["zero_s"], 0.3)
        self.assertAlmostEqual(rep["longest_zero_s"], 0.3)
        self.assertEqual(rep["stuck"], [])

    def test_stuck_stretch_reports_the_window(self):
        samples = [(0.0, 1, s(4288, 900)), (0.5, 1, s(0, 800)), (1.0, 1, s(0, 800)), (5.5, 1, s(0, 800)),
                   (5.6, 1, s(1072, 300))]
        rep = linkprobe.summarize(samples)
        self.assertEqual(len(rep["stuck"]), 1)
        start, secs, wnd = rep["stuck"][0]
        self.assertAlmostEqual(start, 0.5)
        self.assertAlmostEqual(secs, 5.0)
        self.assertEqual(wnd, 0)

    def test_short_pauses_and_idle_sockets_are_not_stuck(self):
        samples = [(0.0, 1, s(4288, 800)), (0.5, 1, s(4288, 800)), (0.6, 1, s(4288, 0)), (9.0, 1, s(4288, 0))]
        self.assertEqual(linkprobe.summarize(samples)["stuck"], [])

    def test_counters_add_up_over_connections(self):
        samples = [(0.0, 1, s(4288, 0, 1000, 100)), (0.1, 1, s(4288, 0, 3000, 536)), (2.0, 2, s(4288, 0, 500, 0))]
        rep = linkprobe.summarize(samples)
        self.assertEqual((rep["sent"], rep["retx"], rep["conns"]), (3500, 536, 2))


class ProbeTest(unittest.TestCase):
    def setUp(self):
        self.srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.srv.fan = FakeFan()
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
        self.base = f"http://127.0.0.1:{self.srv.server_address[1]}"

    def tearDown(self):
        self.srv.shutdown()
        self.srv.server_close()

    def test_stages_below_the_prefill_then_aborts(self):
        lines = []
        rep, _ = linkprobe.probe(clip_put.Fan(self.base), 5_000_000, "09-Probe.bin", log=lines.append,
                                 sleep=lambda x: None)
        sess = self.srv.fan.sess
        self.assertEqual(rep["bytes"], linkprobe.MAX_BYTES)                 # capped: never a real upload
        self.assertEqual(len(sess["buf"]), linkprobe.MAX_BYTES)
        self.assertLess(linkprobe.MAX_BYTES // linkprobe.SPI_FRAME, 620)     # below the fan's prefill
        self.assertGreater(sess["size"], linkprobe.MAX_BYTES)               # the body never completes
        self.assertEqual((sess["state"], sess["err"]), ("failed", "abort"))
        linkprobe.report(rep, log=lines.append)
        self.assertTrue(any(line.startswith("staged 800,000 B") for line in lines))

    def test_resumes_after_a_dropped_chunk(self):
        self.srv.fan.drop_put = {1: 5000}
        rep, _ = linkprobe.probe(clip_put.Fan(self.base), 200_000, "09-Probe.bin", log=lambda x: None,
                                 sleep=lambda x: None)
        self.assertEqual(rep["bytes"], 200_000)
        self.assertEqual(self.srv.fan.sess["state"], "failed")


if __name__ == "__main__":
    unittest.main()
