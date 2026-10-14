#!/usr/bin/env python3
"""Tests for hfbench.py: parsers and the summary on synthetic meshlog text,
run/query against a temp log file, fake senders only (any real network call
fails the test). Run:
    python3 -B -m unittest discover -s tools/bench -p 'test_hfbench.py'
"""
import contextlib
import io
import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hfbench  # noqa: E402


def no_network(*a, **k):
    raise AssertionError("a test tried to use the network")


class Base(unittest.TestCase):
    def setUp(self):
        for target in ("socket.getaddrinfo", "urllib.request.urlopen"):
            p = mock.patch(target, side_effect=no_network)
            p.start()
            self.addCleanup(p.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.log = os.path.join(self.tmp.name, "mesh.log")
        with open(self.log, "w") as f:
            f.write("10:00:00.000 rece [TERM:16][test] volume 3 for 240 s with the radio on: no reset; old\n")

    def append(self, text, direction="rece"):
        with open(self.log, "a") as f:
            f.write(f"{hfbench.stamp(time.time())} {direction} {text}\n")


class CommandTest(Base):
    def test_prefixing(self):
        self.assertEqual(hfbench.fan_command("#HLFQ"), ";wHoloFan,#HLFQ")
        self.assertEqual(hfbench.fan_command(" #HLFXget_setting "), ";wHoloFan,#HLFXget_setting")
        self.assertEqual(hfbench.fan_command(";wHoloLifter,#HLF0"), ";wHoloLifter,#HLF0")
        self.assertEqual(hfbench.fan_command(";w16,#HLFS"), ";w16,#HLFS")
        self.assertEqual(hfbench.fan_command("?WDP,DUMP"), "?WDP,DUMP")
        self.assertEqual(hfbench.fan_command("?backup"), "?backup")

    def test_wcb_setters_refused(self):
        for c in ("?WCBQ,3", "?REBOOT", "?BCAST,OUT,S0,ON", ""):
            with self.assertRaises(ValueError, msg=c):
                hfbench.fan_command(c)

    def test_sender_resolves_once_and_posts_json(self):
        posted = []

        def urlopen(req, timeout):
            posted.append((req.full_url, json.loads(req.data), req.get_header("Content-type"), timeout))
            return io.BytesIO(b'{"success": true}')

        gai = mock.Mock(return_value=[(socket.AF_INET, socket.SOCK_STREAM, 6, "", ("192.0.2.3", 80))])
        with mock.patch("socket.getaddrinfo", gai), mock.patch("urllib.request.urlopen", urlopen):
            send = hfbench.wcb_sender("droidnet.local")
            self.assertIs(send(";wHoloFan,#HLFQ"), True)
            self.assertIs(send(";wHoloFan,#HLFS"), True)
        gai.assert_called_once_with("droidnet.local", 80, socket.AF_INET)
        self.assertEqual(send.ip, "192.0.2.3")
        self.assertEqual(posted[0], ("http://192.0.2.3/api/wcb/command",
                                     {"port": "/dev/ttyUSB0", "command": ";wHoloFan,#HLFQ", "expect_reply": False},
                                     "application/json", 10.0))

    def test_sender_reports_errors_and_skips_lookup_with_ip(self):
        def urlopen(req, timeout):
            raise OSError("no route")

        with mock.patch("urllib.request.urlopen", urlopen):
            send = hfbench.wcb_sender("droidnet.local", "192.0.2.9")   # getaddrinfo is still the failing guard
            self.assertIn("no route", send(";wHoloFan,#HLFQ"))


class ParseTest(Base):
    def test_parse_line(self):
        e = hfbench.parse_line("19:24:26.255 rece [TERM:16][test] 1: volume 3\n")
        self.assertAlmostEqual(e.t, 19 * 3600 + 24 * 60 + 26.255)
        self.assertEqual((e.dir, e.text), ("rece", "[TERM:16][test] 1: volume 3"))
        e = hfbench.parse_line("19:24:26.255 # MARK silence start")
        self.assertEqual((e.dir, e.text), ("#", "MARK silence start"))
        self.assertEqual(hfbench.parse_line("17:46:17.452 # connected").text, "connected")
        self.assertIsNone(hfbench.parse_line("garbage"))
        self.assertIsNone(hfbench.parse_line(""))

    def test_parse_log_orders_and_wraps(self):
        es = hfbench.parse_log(["23:59:59.000 rece a", "00:00:01.000 rece b", "23:59:58.000 # MARK late"])
        self.assertEqual([e.text for e in es], ["MARK late", "a", "b"])
        self.assertAlmostEqual(es[2].t, 86401.0)

    def test_clock(self):
        self.assertEqual(hfbench.parse_clock("19:25"), 69900)
        self.assertEqual(hfbench.parse_clock("19:25:10.5"), 69910.5)
        self.assertEqual(hfbench.fmt_clock(69910.5), "19:25:10.5")
        self.assertEqual(hfbench.fmt_clock(59.96), "00:01:00.0")
        self.assertEqual(hfbench.fmt_clock(69910.5, 3), "19:25:10.500")
        for bad in ("25:00", "19:61", "1925", "19:25:1"):
            with self.assertRaises(ValueError, msg=bad):
                hfbench.parse_clock(bad)

    def test_fandiag(self):
        d = hfbench.parse_fandiag("#HL:FANDIAG st=READY rxb=8621 tx=242 tries=1 ans=1 rx5=20/20 sync18=20/20 "
                                  "rst=3 chg=2 v0=3 last=1.2|PlayInfo.\"04-Beeps.bin\" playing 1 2|")
        self.assertEqual((d["st"], d["tx"], d["rst"], d["chg"], d["v0"]), ("READY", "242", "3", "2", "3"))
        self.assertEqual(d["last"], "1.2|PlayInfo.\"04-Beeps.bin\" playing 1 2|")
        d = hfbench.parse_fandiag("#HL:FANDIAG st=NOMCU rxb=0 tx=9 tries=3 ans=0 rx5=0/0 sync18=0/0 rst=9 last=")
        self.assertEqual((d["st"], d["rst"], d["last"]), ("NOMCU", "9", ""))
        self.assertNotIn("chg", d)
        self.assertIsNone(hfbench.parse_fandiag("#HL:FAN st=READY"))

    def test_setting_and_playinfo(self):
        s = hfbench.parse_setting("[mcu] 29 bytes: setting on one 2 3 164 F-S12|")
        self.assertEqual(s, {"ble": "on", "loop": "one", "br": 2, "vol": 3, "angle": 164, "model": "F-S12"})
        p = hfbench.parse_playinfo('[mcu] 36 bytes: PlayInfo "04-Beeps.bin" playing 1 2|')
        self.assertEqual(p, {"name": "04-Beeps.bin", "state": "playing", "prog": 1, "total": 2})
        self.assertIsNone(hfbench.parse_setting("mculog:volume."))

    def test_sag(self):
        s = hfbench.parse_sag_summary("[sag] 300 s, ms crossed/watched: L3(3.10V)=6/57601 L4(2.92V)=42/57600 "
                                      "L5(2.76V)=13/57600 L6(2.64V)=0/57600 L7(2.51V)=0/57600; det 4")
        self.assertEqual(s["seconds"], 300)
        self.assertEqual(s["levels"][4], (42, 57600))
        self.assertEqual(s["det"], 4)
        self.assertIsNone(hfbench.parse_sag_summary("[sag] 60 s, ms crossed/watched: L3(3.10V)=4/11521")["det"])
        tl = hfbench.parse_sag_timeline("[sag] s 100-199: -----4---5")
        self.assertEqual(tl, {105: 4, 109: 5})
        self.assertEqual(hfbench.sag_deepest(s["levels"]), 5)
        self.assertEqual(hfbench.sag_deepest({3: (0, 9)}, {7: 4}), 4)
        self.assertEqual(hfbench.sag_deepest({3: (0, 9)}), 0)

    def test_probe(self):
        p = hfbench.parse_probe("[probe] 08 dBm, 20 frames/level, crossed (idle): L3(3.10V)=20 (1) L4(2.92V)=5 (0)")
        self.assertEqual(p, {"dBm": 8, "frames": 20, "levels": {3: (20, 1), 4: (5, 0)}})
        self.assertIsNone(hfbench.parse_probe("[probe] levels 3-7 at 20 dBm, 20 frames each (~10 s): ..."))


SESSION = """\
19:00:00.000 # connected
19:00:01.000 sent ;wHoloLifter,#HLF1
19:00:09.100 rece [ETM] WCB16 came ONLINE (boot) (src MAC: 02:12:34:00:00:10)
19:00:10.300 rece [ETM] WCB16 came ONLINE (boot) (src MAC: 02:12:34:00:00:10)
19:00:20.000 sent ;wHoloFan,#HLFXget_setting
19:00:20.050 rece [TERM:16][fan] MCU at connect: ble on, loop all, br 2, vol 3 (capped to 2), angle 164; playing 04-Beeps.bin 1/2
19:00:20.060 rece [TERM:16][mcu] 29 bytes: setting on all 2 2 164 F-S12|
19:00:30.000 sent ;wHoloFan,#HLFQ
19:00:30.500 rece #HL:FANDIAG st=READY rxb=900 tx=40 tries=1 ans=1 rx5=20/20 sync18=20/20 rst=1 chg=0 v0=3 last=x
19:01:00.000 sent ;wHoloFan,#HLFJ3720
19:01:00.010 rece [TERM:16][probe] levels 3-7 at 20 dBm, 20 frames each (~10 s): NO brownout reset until it ends
19:01:10.000 rece [TERM:16][probe] 20 dBm, 20 frames/level, crossed (idle): L3(3.10V)=20 (1) L4(2.92V)=12 (0) L5(2.76V)=0 (0) L6(2.64V)=0 (0) L7(2.51V)=0 (0)
19:01:10.010 rece [TERM:16][probe] done: TX power 80 (0.25 dBm), brownout reset at level 7 again
19:01:20.000 sent ;wHoloFan,#HLFJ3708
19:01:30.000 rece [TERM:16][probe] 08 dBm, 20 frames/level, crossed (idle): L3(3.10V)=9 (1) L4(2.92V)=2 (0) L5(2.76V)=0 (0) L6(2.64V)=0 (0) L7(2.51V)=0 (0)
19:02:00.000 sent ;wHoloFan,#HLFE37300
19:02:00.010 rece [TERM:16][sag] levels 3-7 for 300 s: NO brownout reset until it ends
19:02:01.000 sent ;wHoloFan,#HLFY1240
19:02:01.010 rece [TERM:16][test] 3: volume 3 for 240 s with the radio on, then volume 2
19:02:01.020 rece [TERM:16][mcu] vt: |[0]> m 3||[4]> get_setting|
19:02:30.000 rece [TERM:16][mcu] mculog:volume.
19:02:31.000 # MARK op: beeps stopped
19:02:41.000 # MARK op: beeps back
19:06:01.500 rece [TERM:16][test] NOTE: #HLFE/#HLFJ had the brownout reset off: "no reset" is no verdict; read the [sag] L7 count
19:06:01.510 rece [TERM:16][test] volume 3 for 240 s with the radio on: no reset; volume 2 again (read back)
19:07:00.000 rece [TERM:16][sag] 300 s, ms crossed/watched: L3(3.10V)=6/57601 L4(2.92V)=42/57600 L5(2.76V)=13/57600 L6(2.64V)=2/57600 L7(2.51V)=0/57600; det 3
19:07:00.010 rece [TERM:16][sag] s 0-99: 35-----4--
19:07:00.020 rece [TERM:16][sag] s 100-199: -----6
19:07:00.030 rece [TERM:16][sag] done: brownout reset at level 7 again
19:10:00.000 sent ;wHoloFan,#HLFY0240
19:10:00.010 rece [TERM:16][test] 4: volume 3 for 240 s with the radio OFF, then volume 2
19:14:01.000 rece [ETM] WCB16 came ONLINE (boot) (src MAC: 02:12:34:00:00:10)
19:14:02.000 rece [TERM:16][test] volume 3 for 240 s with the radio OFF: no reset; volume 2 again (read back)
19:15:00.000 sent ;wHoloFan,#HLFY1240
19:15:00.010 rece [TERM:16][test] 5: volume 3 for 240 s with the radio on, then volume 2
19:15:40.000 rece [ETM] WCB16 came ONLINE (boot) (src MAC: 02:12:34:00:00:10)
19:16:00.000 sent ;wHoloFan,#HLFXget_setting
19:16:00.010 rece [TERM:16][test] RESET during #HLFY1240 (test 5): phase HOLD, 38.2 s into the hold, 39.5 s into the test; rst=9
19:16:00.020 rece [TERM:16][mcu] UNPROMPTED: vol 2->3 (capped)
19:16:05.000 rece #HL:FANDIAG st=READY rxb=100 tx=5 tries=1 ans=1 rx5=20/20 sync18=20/20 rst=9 chg=1 v0=2 last=y
19:17:00.000 rece [TERM:16][mcu] UNPROMPTED while off the mesh: loop one->all
19:17:05.000 rece [TERM:16][mcu] refused (empty, unsafe, or v)
19:17:06.000 rece [TERM:16][mcu] refused (empty, unsafe, or v)
19:17:07.000 rece [TERM:15] lifter line
19:18:00.000 # ws error TimeoutError('timed out during opening handshake')
19:18:12.000 # ws error TimeoutError('timed out during opening handshake')
19:18:30.000 # connected
19:01:15.000 # MARK backdated: operator heard a click
"""


class SummaryTest(Base):
    def setUp(self):
        super().setUp()
        self.entries = hfbench.parse_log(SESSION.splitlines())
        self.a = hfbench.analyze(self.entries)

    def test_boot_announces_classified(self):
        causes = [(hfbench.fmt_clock(b.t), b.n, b.cause) for b in self.a.boots]
        self.assertEqual(len(causes), 3)
        self.assertEqual(causes[0][:2], ("19:00:09.1", 2))
        self.assertIn("power-on", causes[0][2])
        self.assertIn("radio back", causes[1][2])
        self.assertIn("during #HLFY test 5", causes[2][2])
        self.assertEqual(self.a.txdrops, ["19:00:30.5 tx=40 -> 19:16:05.0 tx=5"])

    def test_tests_and_counts(self):
        self.assertEqual([(t.label, t.radio, t.cmd) for t in self.a.tests],
                         [("3", "on", "#HLFY1240"), ("4", "OFF", "#HLFY0240"), ("5", "on", "#HLFY1240")])
        self.assertTrue(self.a.tests[0].outcome.startswith("no reset"))
        self.assertIn("NOTE", self.a.tests[0].notes[0])
        self.assertTrue(self.a.tests[2].outcome.startswith("RESET during #HLFY1240"))
        self.assertEqual([v for _, v, _ in self.a.rst], [1, 9, 9])
        self.assertEqual(len(self.a.unprompted), 2)
        self.assertEqual(len(self.a.bootrec), 1)
        self.assertIn("vol 3 (capped to 2)", self.a.bootrec[0][1])

    def test_sag_and_probes(self):
        (s,) = self.a.sag
        self.assertEqual((s.lo, s.hi, s.seconds, s.det), (3, 7, 300, 3))
        self.assertEqual(hfbench.sag_deepest(s.levels, s.timeline), 6)
        self.assertEqual(s.timeline, {0: 3, 1: 5, 7: 4, 105: 6})
        self.assertEqual([p["dBm"] for _, p in self.a.probes], [20, 8])

    def test_stop_rules(self):
        stops = "\n".join(self.a.stops)
        for want in ("during #HLFY test 5", "RESET during", "rst=9 (brownout) in RESET record",
                     "rst=9 (brownout) in FANDIAG", "[sag] L6/L7 > 0"):
            self.assertIn(want, stops)
        self.assertEqual(len(self.a.outages), 1)
        self.assertEqual(self.a.outages[0].n, 2)

    def test_summary_text(self):
        text = "\n".join(hfbench.summary(SESSION.splitlines()))
        self.assertIn("19:01:15.0  # MARK backdated: operator heard a click", text)   # sorted into place
        self.assertLess(text.index("MARK backdated"), text.index("08 dBm, 20 frames"))
        self.assertIn("WCB16 came ONLINE (boot)   (x2, to 19:00:10.3)", text)
        self.assertIn("[mcu] refused (empty, unsafe, or v)   (x2, to 19:17:06.0)", text)
        self.assertIn("setting vol 2, br 2, loop all, ble on, angle 164", text)
        self.assertIn("FANDIAG st=READY rxb=100 tx=5 rst=9 chg=1 v0=2", text)
        self.assertIn("> relay ON (HoloLifter #HLF1)", text)
        self.assertIn("deepest L6 (2.64 V)", text)
        self.assertIn("s105 19:03:45 L6", text)
        self.assertLess(text.index("20 dBm  19:01:10.0"), text.index(" 8 dBm  19:01:30.0"))
        self.assertIn("UNPROMPTED reports: 2", text)
        self.assertNotIn("vt:", text)
        self.assertNotIn("lifter line", text)
        self.assertNotIn("STOP / CHECK: none", text)

    def test_since_until(self):
        text = "\n".join(hfbench.summary(SESSION.splitlines(), hfbench.parse_clock("19:10"),
                                         hfbench.parse_clock("19:15:30")))
        self.assertIn("#HLFY tests: 2", text)
        self.assertIn("boot announces (WCB16 came ONLINE (boot); most of these broadcasts are lost): 1", text)
        self.assertNotIn("19:00:", text)
        self.assertNotIn("19:16:", text)


class FollowTest(Base):
    def run_writer(self, steps):
        """Append (delay_s, text, dir) lines from a thread, like meshlog.py."""
        def go():
            for delay, text, direction in steps:
                time.sleep(delay)
                self.append(text, direction)
        th = threading.Thread(target=go, daemon=True)
        th.start()
        return th

    def test_run_until_matches_past_the_echo(self):
        follow = hfbench.Follower(self.log)
        self.addCleanup(follow.close)
        sent, out = [], []
        th = self.run_writer([
            (0.10, ";wHoloFan,#HLFY1240", "sent"),                 # our echo: matches the regex, must not count
            (0.10, "[TERM:16][test] 7: volume 3 for 240 s with the radio on, then volume 2", "rece"),
            (0.10, "[TERM:15] not the fan", "rece"),
            (0.10, "[TERM:16][test] volume 3 for 240 s with the radio on: no reset; volume 2 again", "rece"),
            (0.10, "[ETM] WCB16 came ONLINE (src MAC: 02:12:34:00:00:10)", "rece"),   # inside the tail
            (1.50, "[TERM:16][mcu] after the tail", "rece"),
        ])
        rc = hfbench.run_until(lambda c: sent.append(c) or True, follow, [";wHoloFan,#HLFY1240"],
                               "HLFY1240|no reset|STOPPED", timeout=10, tail=0.5, out=out.append, poll=0.02)
        self.assertEqual(rc, 0)
        self.assertEqual(sent, [";wHoloFan,#HLFY1240"])
        text = "\n".join(out)
        marked = [l for l in out if "<== until" in l]
        self.assertEqual(len(marked), 1)
        self.assertIn("no reset", marked[0])
        self.assertIn("sent ;wHoloFan,#HLFY1240", text)          # printed, not matched
        self.assertIn("came ONLINE", text)
        self.assertNotIn("not the fan", text)
        self.assertNotIn("after the tail", text)
        self.assertNotIn("; old", text)                           # lines before the send are skipped
        self.assertTrue(all(l.startswith("+") for l in out if "TERM:16" in l))
        th.join()

    def test_run_until_timeout_and_fail(self):
        follow = hfbench.Follower(self.log)
        self.addCleanup(follow.close)
        out = []
        rc = hfbench.run_until(lambda c: True, follow, [";wHoloFan,#HLFS"], "never", timeout=0.3,
                               out=out.append, poll=0.02)
        self.assertEqual(rc, 2)
        self.assertIn("timeout", out[-1])
        th = self.run_writer([(0.05, "[TERM:16][test] refused: a test is running", "rece")])
        rc = hfbench.run_until(lambda c: True, follow, [";wHoloFan,#HLFY1240"], "no reset", timeout=5,
                               tail=0.1, out=out.append, poll=0.02)
        self.assertEqual(rc, 3)
        th.join()

    def test_run_until_send_failure_and_gap(self):
        follow = hfbench.Follower(self.log)
        self.addCleanup(follow.close)
        out, sent, slept = [], [], []
        rc = hfbench.run_until(lambda c: "URLError('down')", follow, [";wHoloFan,#HLFS"], "x", out=out.append)
        self.assertEqual(rc, 1)
        th = self.run_writer([(0.05, "[TERM:16][sag] levels 3-7 for 60 s", "rece")])
        rc = hfbench.run_until(lambda c: sent.append(c) or True, follow, ["a", "b"], r"\[sag\] levels",
                               gap=1.0, tail=0.05, out=out.append, sleep=lambda s: slept.append(s) or time.sleep(0.02))
        self.assertEqual((rc, sent), (0, ["a", "b"]))
        self.assertEqual(slept[0], 1.0)
        th.join()

    def test_query_retries_until_the_reply(self):
        follow = hfbench.Follower(self.log)
        self.addCleanup(follow.close)
        calls, out = [], []

        def send(c):                                          # the WCB echoes each send; reply on the 3rd
            calls.append(c)
            self.append(c, "sent")
            if len(calls) == 3:
                self.append("#HL:FANDIAG st=READY rxb=1 tx=2 rst=3 chg=0 v0=2 last=")
            return True

        rc = hfbench.query(send, follow, ";wHoloFan,#HLFQ", "FANDIAG", tries=8, gap=0.1, out=out.append, poll=0.01)
        self.assertEqual((rc, len(calls)), (0, 3))
        self.assertTrue(any("<== expect" in l and "FANDIAG" in l for l in out))

    def test_query_ignores_its_echo_and_gives_up(self):
        follow = hfbench.Follower(self.log)
        self.addCleanup(follow.close)
        calls, out = [], []

        def send(c):
            calls.append(c)
            self.append(c, "sent")
            return True

        rc = hfbench.query(send, follow, ";wHoloFan,#HLFQ", "HLFQ", tries=3, gap=0.05, grace=0.1,
                           out=out.append, poll=0.01)
        self.assertEqual((rc, len(calls)), (2, 3))
        rc = hfbench.query(lambda c: "error", follow, ";wHoloFan,#HLFQ", "x", tries=2, gap=0.01, grace=0.01,
                           out=out.append, poll=0.01)
        self.assertEqual(rc, 1)


class MarkAndMainTest(Base):
    def test_mark(self):
        line = hfbench.append_mark(self.log, "silence start")
        e = hfbench.parse_line(line)
        self.assertEqual((e.dir, e.text), ("#", "MARK silence start"))
        line = hfbench.append_mark(self.log, "op: silence ~10 s", at=hfbench.parse_clock("19:25:10.5"))
        self.assertEqual(line, "19:25:10.500 # MARK op: silence ~10 s")
        with open(self.log) as f:
            self.assertEqual(f.read().splitlines()[-1], line)

    def fake_factory(self):
        made = []

        def factory(host, host_ip=None):
            sent = []

            def send(c):
                sent.append(c)
                return True
            send.ip = host_ip or "192.0.2.2"
            made.append((host, host_ip, sent))
            return send
        return made, factory

    def call_main(self, argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = hfbench.main(argv)
        return rc, buf.getvalue()

    def test_main_send(self):
        made, factory = self.fake_factory()
        with mock.patch.object(hfbench, "wcb_sender", factory):
            rc, text = self.call_main(["send", "#HLFQ", ";wHoloLifter,#HLF0", "?WDP,DUMP", "--gap", "0",
                                       "--host", "droidnet.local", "--host-ip", "192.0.2.87"])
            self.assertEqual(rc, 0)
            self.assertEqual(made, [("droidnet.local", "192.0.2.87", [";wHoloFan,#HLFQ", ";wHoloLifter,#HLF0", "?WDP,DUMP"])])
            self.assertNotIn("skips the lookup", text)
            rc, text = self.call_main(["send", "#HLFS", "?WCBQ,3", "--host", "droidnet.local"])      # refused before anything is sent
            self.assertEqual((rc, len(made)), (1, 1))
            self.assertIn("never ?X,<value> setters", text)
            rc, text = self.call_main(["send", "#HLFS", "--gap", "0", "--host", "droidnet.local"])   # name lookup: prints the shortcut
            self.assertIn("droidnet.local -> 192.0.2.2 (--host-ip 192.0.2.2 skips the lookup)", text)

    def test_main_send_needs_a_host(self):
        made, factory = self.fake_factory()
        with mock.patch.object(hfbench, "wcb_sender", factory), mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("HFBENCH_HOST", None)
            rc, text = self.call_main(["send", "#HLFS", "--gap", "0"])
            self.assertEqual((rc, made), (1, []))
            self.assertIn("pass --host", text)
            os.environ["HFBENCH_HOST"] = "droidnet.local"
            rc, text = self.call_main(["send", "#HLFS", "--gap", "0"])
            self.assertEqual(rc, 0)
            self.assertEqual(made[0][0], "droidnet.local")

    def test_main_run_needs_the_log_and_mark_and_summary(self):
        made, factory = self.fake_factory()
        with mock.patch.object(hfbench, "wcb_sender", factory):
            rc, text = self.call_main(["run", "#HLFS", "--until", "x", "--host", "droidnet.local",
                                       "--log", os.path.join(self.tmp.name, "nope")])
            self.assertEqual((rc, made), (1, []))
            self.assertIn("meshlog.py", text)
            rc, text = self.call_main(["query", "#HLFQ", "--expect", "(", "--host", "droidnet.local", "--log", self.log])
            self.assertEqual((rc, made), (1, []))
        rc, text = self.call_main(["mark", "beeps", "stopped", "--log", self.log])
        self.assertEqual(rc, 0)
        self.assertIn("# MARK beeps stopped", text)
        with open(self.log, "a") as f:
            f.write(SESSION)
        rc, text = self.call_main(["summary", self.log, "--since", "19:00"])
        self.assertEqual(rc, 0)
        self.assertIn("== totals ==", text)
        self.assertIn("#HLFY tests: 3", text)


if __name__ == "__main__":
    unittest.main()
