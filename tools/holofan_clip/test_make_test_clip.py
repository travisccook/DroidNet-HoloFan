#!/usr/bin/env python3
"""Tests for make_test_clip.py.
Run: python3 -B -m unittest discover -s tools/holofan_clip -p 'test_*.py'"""
import contextlib
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import clipgen  # noqa: E402
import make_test_clip as mtc  # noqa: E402


class MakeTestClip(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.header = os.path.join(self.tmp.name, "header.bin")
        self.out = os.path.join(self.tmp.name, "out.bin")
        with open(self.header, "wb") as f:
            f.write(clipgen.HEADER)

    def tearDown(self):
        self.tmp.cleanup()

    def run_main(self, *args):
        text = io.StringIO()
        with contextlib.redirect_stdout(text):
            status = mtc.main(["make_test_clip.py", *args])
        return status, text.getvalue()

    def test_frame_is_this_fans_geometry(self):
        # docs/HOW_IT_WORKS.md, "Clip format": 520 x 6 x 24 = 74,880; 76,480 with audio
        self.assertEqual(mtc.VIDEO_LEN, 74880)
        self.assertEqual(mtc.FRAME_LEN, 76480)
        geom = clipgen.parse_geometry("64x520x6")
        self.assertEqual(mtc.VIDEO_LEN, geom.video_bytes)
        self.assertEqual(mtc.FRAME_LEN, geom.frame_bytes)
        self.assertEqual(mtc.HEADER_LEN, clipgen.HEADER_LEN)
        self.assertEqual(mtc.AUDIO_LEN, clipgen.AUDIO_BYTES)

    def test_default_is_a_clip_the_fan_lists(self):
        status, _ = self.run_main(self.header, self.out)
        self.assertEqual(status, 0)
        with open(self.out, "rb") as f:
            data = f.read()
        self.assertEqual(len(data), 812 + 30 * 76480)
        self.assertGreaterEqual(mtc.DEFAULT_FRAMES, 30)
        self.assertGreaterEqual(mtc.DEFAULT_FRAMES / clipgen.FPS, clipgen.LISTED_SECONDS)
        geom = clipgen.parse_geometry("64x520x6")
        self.assertEqual(clipgen.frames_in(geom, len(data)), 30)
        # the same bytes clipgen makes for a black, silent clip, before padding
        want = clipgen.build_clip(geom, 30)
        self.assertEqual(data, want[:len(data)])
        self.assertEqual(want[len(data):], bytes(len(want) - len(data)))

    def test_short_counts_are_refused_and_write_nothing(self):
        for frames in ("1", "20", "29"):
            status, text = self.run_main(self.header, self.out, frames)
            self.assertEqual(status, 2, frames)
            self.assertIn("refused", text)
            self.assertIn("--allow-unlisted", text)
            self.assertFalse(os.path.exists(self.out))

    def test_override_makes_a_short_clip_and_warns(self):
        for args in ((self.header, self.out, "1", "--allow-unlisted"),
                     ("--allow-unlisted", self.header, self.out, "1")):
            status, text = self.run_main(*args)
            self.assertEqual(status, 0)
            self.assertIn("WARNING", text)
            self.assertEqual(os.path.getsize(self.out), 812 + 76480)

    def test_thirty_and_more_need_no_override(self):
        status, text = self.run_main(self.header, self.out, "31")
        self.assertEqual(status, 0)
        self.assertNotIn("WARNING", text)
        self.assertEqual(os.path.getsize(self.out), 812 + 31 * 76480)

    def test_bad_arguments(self):
        for args in ((), (self.header,), (self.header, self.out, "x"), (self.header, self.out, "0"),
                     (self.header, self.out, "-3"), (self.header, self.out, "30", "extra"),
                     (self.header, self.out, "--bogus")):
            self.assertEqual(self.run_main(*args)[0], 2, args)
        self.assertFalse(os.path.exists(self.out))

    def test_bad_header_file(self):
        with open(self.header, "wb") as f:
            f.write(b"\x00" * 812)
        self.assertEqual(self.run_main(self.header, self.out)[0], 1)
        self.assertFalse(os.path.exists(self.out))


if __name__ == "__main__":
    unittest.main()
