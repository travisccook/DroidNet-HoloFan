#!/usr/bin/env python3
"""Tests for silences.py on synthetic recordings: clipgen's 'beeps:10:2000'
(the 04-Beeps audio) with one 10 s silence and a 12 dB quieter stretch, plus
the --sync arithmetic. Run:
    python3 -B -m unittest discover -s tools/bench -p 'test_silences.py'
"""
import argparse
import contextlib
import io
import os
import re
import sys
import tempfile
import unittest
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "holofan_clip"))
import clipgen  # noqa: E402
import silences  # noqa: E402

SIL = (20.0, 30.0)          # the beeps are cut out here
QUIET = (40.0, 55.0)        # and 12 dB quieter here (a volume step)


def beeps(seconds, rate):
    """clipgen's 'beeps:10:2000' audio as float -1..1 at rate (32000 or 16000)."""
    u8 = clipgen.audio_samples("beeps:10:2000", int(seconds * clipgen.SAMPLE_RATE))
    x = (np.frombuffer(u8, dtype=np.uint8).astype(np.float64) - 128) / 128
    return x[::clipgen.SAMPLE_RATE // rate]   # 2 kHz is far under 8 kHz: plain decimation is fine


def scene(rate=silences.RATE, seconds=60.0, noise_db=-60.0, seed=1):
    """(signal, true silence start, true end): beeps, cut over SIL, quieter over
    QUIET, plus white noise. The truth is the last beep sample before SIL and
    the first one after it (the beeps' own 80 ms gaps border the cut)."""
    x = beeps(seconds, rate)
    t = np.arange(len(x)) / rate
    x[(t >= SIL[0]) & (t < SIL[1])] = 0
    x[(t >= QUIET[0]) & (t < QUIET[1])] *= 10 ** (-12 / 20)
    on = np.nonzero(np.abs(x) > 0)[0]
    start = (on[on < SIL[0] * rate].max() + 1) / rate
    end = on[on >= SIL[1] * rate].min() / rate
    x = x + np.random.default_rng(seed).normal(0, 10 ** (noise_db / 20), len(x))
    return x, start, end


def args(**kw):
    a = argparse.Namespace(window=50.0, hz=None, rel_db=25.0, abs_db=None, min=silences.DEFAULT_MIN_S, levels=False)
    for k, v in kw.items():
        setattr(a, k, v)
    return a


def longest_gap_s(spec, seconds=3.2):
    """The longest run of silent (0x80) samples in a clipgen audio spec."""
    u8 = np.frombuffer(clipgen.audio_samples(spec, int(seconds * clipgen.SAMPLE_RATE)), dtype=np.uint8)
    quiet = np.concatenate(([0], (u8 == clipgen.SILENCE).astype(np.int8), [0]))
    edges = np.diff(quiet)
    runs = np.nonzero(edges == -1)[0] - np.nonzero(edges == 1)[0]
    return runs.max() / clipgen.SAMPLE_RATE


class AnalysisTest(unittest.TestCase):
    def test_default_min_clears_the_beep_gaps(self):
        gap = longest_gap_s("beeps:10:2000")
        self.assertLess(gap, 0.1)                                   # 80 ms gaps (+ ramp tails)
        self.assertGreaterEqual(silences.DEFAULT_MIN_S, 10 * gap)
        self.assertGreater(longest_gap_s("beeps:1:2000"), silences.DEFAULT_MIN_S)   # why '--min 2' there

    def test_finds_exactly_the_long_silence(self):
        x, start, end = scene()
        levels, step = silences.window_levels(x, silences.RATE, 50)
        self.assertAlmostEqual(step, 0.05)
        thr, ref = silences.threshold(levels)
        self.assertAlmostEqual(ref - thr, 25.0)
        found = silences.find_silences(levels, step, thr)
        self.assertEqual(len(found), 1, found)
        (a, b), = found
        self.assertLess(abs(a - start), 0.1, (a, start))
        self.assertLess(abs(b - end), 0.1, (b, end))
        self.assertLess(abs(a - SIL[0]), 0.1)
        self.assertLess(abs(b - SIL[1]), 0.1)

    def test_min_and_abs_threshold(self):
        x, _, _ = scene()
        levels, step = silences.window_levels(x, silences.RATE, 50)
        thr, _ = silences.threshold(levels)
        self.assertEqual(silences.find_silences(levels, step, thr, min_s=11.0), [])
        self.assertGreater(len(silences.find_silences(levels, step, thr, min_s=0.05)), 100)  # every beep gap
        self.assertEqual(silences.threshold(levels, abs_db=-40.0), (-40.0, None))
        self.assertEqual(len(silences.find_silences(levels, step, -40.0)), 1)

    def test_band_ignores_broadband_noise(self):
        x, start, end = scene(noise_db=-26.0)         # rotor whoosh: louder than the silence threshold
        levels, step = silences.window_levels(x, silences.RATE, 50)
        self.assertEqual(silences.find_silences(levels, step, silences.threshold(levels)[0]), [])
        levels, step = silences.window_levels(x, silences.RATE, 50, hz=2000)
        (a, b), = silences.find_silences(levels, step, silences.threshold(levels)[0])
        self.assertLess(abs(a - start), 0.1)
        self.assertLess(abs(b - end), 0.1)

    def test_band_level_matches_broadband_for_a_tone(self):
        t = np.arange(silences.RATE) / silences.RATE
        x = 0.5 * np.sin(2 * np.pi * 2000 * t)
        full, _ = silences.window_levels(x, silences.RATE, 50)
        band, _ = silences.window_levels(x, silences.RATE, 50, hz=2000)
        off, _ = silences.window_levels(x, silences.RATE, 50, hz=440)
        self.assertAlmostEqual(full[0], 20 * np.log10(0.5 / np.sqrt(2)), places=3)
        self.assertAlmostEqual(band[0], full[0], places=3)
        self.assertLess(off[0], full[0] - 60)

    def test_block_levels_show_the_volume_step(self):
        x, _, _ = scene()
        levels, step = silences.window_levels(x, silences.RATE, 50)
        blocks = silences.block_levels(levels, step)
        self.assertEqual(len(blocks), 12)
        med = {round(t): m for t, m, _ in blocks}
        loud = np.mean([med[t] for t in (0, 5, 10, 15)])
        quiet = np.mean([med[t] for t in (40, 45, 50)])
        self.assertAlmostEqual(loud - quiet, 12.0, delta=1.5)
        self.assertLess(med[20], loud - 40)


class TimeTest(unittest.TestCase):
    def test_sync_arithmetic(self):
        want = silences.parse_clock("19:18:40.6")
        for spec in ("19:20:05.3@84.7", "19:20:05.3@1:24.7", "19:20:05.3@0:01:24.7"):
            self.assertAlmostEqual(silences.sync_start(spec), want, places=6, msg=spec)
        self.assertEqual(silences.fmt_clock(silences.sync_start("00:00:30@60")), "23:59:30.0")
        self.assertEqual(silences.fmt_clock(silences.sync_start("12:00:00@0")), "12:00:00.0")
        for bad in ("19:20:05", "19:20:05@x", "19:20:05@1:2:3:4", "99:00@5"):
            with self.assertRaises(ValueError, msg=bad):
                silences.sync_start(bad)

    def test_formats(self):
        self.assertEqual(silences.fmt_rec(84.7), "1:24.70")
        self.assertEqual(silences.fmt_rec(5), "0:05.00")
        self.assertEqual(silences.fmt_clock(69910.55), "19:25:10.6")
        self.assertEqual(silences.parse_offset("90"), 90.0)

    def test_report_gives_wall_clock(self):
        x, start, end = scene()
        sync = "19:00:20.0@20.0"                       # the cut at rec 20.0 s was 19:00:20.0
        lines = silences.report(x, silences.RATE, args(levels=True), silences.sync_start(sync))
        rows = [l for l in lines if re.match(r"\s+1\s", l)]
        self.assertEqual(len(rows), 1, lines)
        walls = re.findall(r"19:00:(\d\d\.\d)", rows[0])
        self.assertLess(abs(float(walls[0]) - start), 0.15)
        self.assertLess(abs(float(walls[1]) - end), 0.15)
        self.assertIn("silences: 1", lines)
        self.assertTrue(any(l.startswith("levels per 5 s") for l in lines))


def have_decoder():
    return silences.tool("ffmpeg") or silences.tool("afconvert")


@unittest.skipUnless(have_decoder(), "needs ffmpeg or afconvert")
class EndToEndTest(unittest.TestCase):
    def test_main_on_a_stereo_32k_wav(self):
        x, start, end = scene(rate=32000)
        pcm = np.round(np.clip(x, -1, 1) * 32767).astype("<i2")
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "rec.wav")
            with wave.open(path, "wb") as w:
                w.setnchannels(2)
                w.setsampwidth(2)
                w.setframerate(32000)
                w.writeframes(np.repeat(pcm, 2).tobytes())   # L = R
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = silences.main([path, "--start", "19:00:00"])
            out = buf.getvalue()
            self.assertEqual(rc, 0, out)
            self.assertIn("silences: 1", out)
            row = [l for l in out.splitlines() if re.match(r"\s+1\s", l)][0]
            walls = [float(s) for s in re.findall(r"19:00:(\d\d\.\d)", row)]
            self.assertLess(abs(walls[0] - start), 0.15, out)
            self.assertLess(abs(walls[1] - end), 0.15, out)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertEqual(silences.main([os.path.join(tmp, "missing.m4a")]), 1)
                self.assertEqual(silences.main([path, "--start", "19:00", "--sync", "19:00@1"]), 1)


if __name__ == "__main__":
    unittest.main()
