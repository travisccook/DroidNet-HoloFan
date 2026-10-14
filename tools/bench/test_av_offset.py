#!/usr/bin/env python3
"""Tests for av_offset.py: the detectors on synthetic series, then whole
phone-like videos (60 fps H.264 + AAC .mov, a rotor showing each flash only
in a sector, 2 kHz bursts in noise) built with ffmpeg in a temp dir. No
network. Run:
    python3 -B -m unittest discover -s tools/bench -p 'test_av_offset.py'
"""
import contextlib
import io
import os
import re
import subprocess
import sys
import tempfile
import unittest
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import av_offset  # noqa: E402
import silences  # noqa: E402

FFMPEG = silences.tool("ffmpeg")
SR = 48000                 # synthetic audio rate (a phone's)
W = 64                     # synthetic frame size


def burst(seconds=0.05, hz=2000.0, rate=SR, amp=0.3):
    """A tone burst with 2 ms raised-cosine edges (as clipgen's burst)."""
    n = int(round(seconds * rate))
    t = np.arange(n) / rate
    ramp = int(0.002 * rate)
    env = np.ones(n)
    edge = 0.5 - 0.5 * np.cos(np.pi * np.arange(ramp) / ramp)
    env[:ramp], env[-ramp:] = edge, edge[::-1]
    return amp * env * np.sin(2 * np.pi * hz * t)


def click(rate=SR, amp=0.5, seed=0):
    """A clap: 40 ms of decaying broadband noise."""
    n = int(0.04 * rate)
    rng = np.random.default_rng(seed)
    return amp * np.exp(-np.arange(n) / (0.008 * rate)) * rng.uniform(-1, 1, n)


def audio_track(seconds, events, delay, kind, rate=SR, seed=1):
    rng = np.random.default_rng(seed)
    x = rng.normal(0, 0.01, int(seconds * rate))               # room + rotor noise, -40 dBFS
    for i, t in enumerate(events):
        s = int(round((t + delay) * rate))
        b = burst(rate=rate) if kind == "fan" else click(rate=rate, seed=i)
        x[s:s + len(b)] += b[:max(0, len(x) - s)]
    return x


def frames_for(seconds, events, kind, fps, seed=2):
    """Grey frames (n, W, W) sampled at the instants i/fps.

    fan: a disc with a dim rotor arm turning 23 times a second; during a flash
    (50 ms) only a 70-degree sector at the arm is lit, so lit frames differ.
    flash: a phone screen that turns white for 0.2 s. clap: two bright hands
    that accelerate together and meet at each event time, then part slowly.
    """
    rng = np.random.default_rng(seed)
    n = int(seconds * fps)
    yy, xx = np.mgrid[0:W, 0:W] - (W - 1) / 2
    radius = np.hypot(xx, yy)
    angle = np.arctan2(yy, xx)
    out = np.empty((n, W, W), dtype=np.uint8)
    for i in range(n):
        t = i / fps
        f = 14 + rng.normal(0, 2, (W, W))
        if kind == "fan":
            arm = 2 * np.pi * 23 * t
            off = np.abs((angle - arm + np.pi) % (2 * np.pi) - np.pi)
            disc = radius < W / 2 - 2
            lit = any(e <= t < e + 0.05 for e in events)
            f[disc & (off < np.radians(35 if lit else 8))] = 235 if lit else 40
        elif kind == "flash":
            if any(e <= t < e + 0.2 for e in events):
                f[8:56, 16:48] = 220
        else:
            gap = W // 2 - 4                                   # hands' distance apart
            for e in events:
                if e - 0.25 <= t < e:
                    gap = int(round((W // 2 - 4) * (1 - ((t - e + 0.25) / 0.25) ** 2)))
                elif e <= t < e + 0.3:
                    gap = 0
                elif e + 0.3 <= t < e + 0.8:
                    gap = int(round((W // 2 - 4) * (t - e - 0.3) / 0.5))
            f[20:44, max(0, W // 2 - gap - 12):W // 2 - gap] = 200
            f[20:44, W // 2 + gap:min(W, W // 2 + gap + 12)] = 200
        out[i] = np.clip(f, 0, 255).astype(np.uint8)
    return out


def encoder():
    r = subprocess.run([FFMPEG, "-hide_banner", "-encoders"], capture_output=True, text=True)
    return "libx264" if "libx264" in r.stdout else "mpeg4"


def make_video(tmp, name, seconds, events, delay, kind="fan", fps=60, audio_start=0.0):
    """A .mov with the frames and the audio (events + delay) muxed by ffmpeg;
    audio_start > 0 starts the audio track that much after the video's."""
    raw = os.path.join(tmp, name + ".gray")
    wav = os.path.join(tmp, name + ".wav")
    out = os.path.join(tmp, name + ".mov")
    frames_for(seconds, events, kind, fps).tofile(raw)
    pcm = np.round(np.clip(audio_track(seconds, events, delay, kind), -1, 1) * 32767).astype("<i2")
    with wave.open(wav, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    cmd = [FFMPEG, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pixel_format", "gray", "-video_size", f"{W}x{W}",
           "-framerate", str(fps), "-i", raw]
    if audio_start:
        cmd += ["-itsoffset", str(audio_start)]
    cmd += ["-i", wav, "-c:v", encoder(), "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "128k", out]
    subprocess.run(cmd, check=True, capture_output=True)
    return out


def fan_events(count=8, start=0.53, period=1.6, drift=0.0037):
    """Flash times: not on frame boundaries, drifting against the phone's frames."""
    return [start + k * (period + drift) for k in range(count)]


def run(*argv):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        code = av_offset.main(list(argv))
    return code, buf.getvalue()


def result_ms(out):
    m = re.search(r"result: audio minus video ([+-]\d+) ms", out)
    assert m, out
    return int(m[1])


class DetectorTest(unittest.TestCase):
    def test_burst_onsets_in_noise(self):
        rate = av_offset.RATE
        times = [0.4 + 1.6 * k + 0.0013 * k for k in range(6)]
        x = np.random.default_rng(3).normal(0, 0.02, int(10.5 * rate))   # broadband, -34 dBFS
        for t in times:
            s = int(round(t * rate))
            b = burst(rate=rate)
            x[s:s + len(b)] += b
        x += 0.3 * np.sin(2 * np.pi * 120 * np.arange(len(x)) / rate)    # loud hum, off band
        t, level = av_offset.levels(x, rate, hz=2000.0)
        self.assertAlmostEqual(t[1] - t[0], 0.001)
        onsets, snr = av_offset.audio_onsets(t, level, 1.6)
        self.assertGreater(snr, 20)
        self.assertEqual(len(onsets), len(times), onsets)
        for got, want in zip(onsets, times):
            self.assertLess(abs(got - want), 0.003, (got, want))
        # the hum alone (no band) swamps the bursts: the band is what finds them
        t, level = av_offset.levels(x, rate)
        self.assertLess(len(av_offset.audio_onsets(t, level, 1.6)[0]), len(times))

    def test_no_events_in_noise(self):
        x = np.random.default_rng(4).normal(0, 0.02, 5 * av_offset.RATE)
        t, level = av_offset.levels(x, hz=2000.0)
        self.assertEqual(av_offset.audio_onsets(t, level, 1.6)[0], [])
        t = np.arange(300) / 60
        y = 20 + np.random.default_rng(5).normal(0, 0.3, 300)
        self.assertEqual(av_offset.video_onsets(t, y, 1.6), [])

    def test_flash_onset_interpolates_between_frames(self):
        fps = 60.0
        t = np.arange(600) / fps
        y = np.full(600, 30.0)
        # flash 1: frames 100-102 fully lit -> half way between frames 99 and 100
        y[100:103] = 130
        # flash 2: a dim first frame (rotor sector) then the peak: 20 % at 200,
        # 100 % at 201 -> 3/8 of the way from frame 200 to 201
        y[200], y[201], y[202] = 50, 130, 70
        # flash 3: the peak frame is the last one, after two half-lit frames
        y[300], y[301], y[302] = 90, 90, 130
        got = av_offset.video_onsets(t, y, 1.6)
        self.assertEqual(len(got), 3, got)
        self.assertAlmostEqual(got[0], 99.5 / fps, places=6)
        self.assertAlmostEqual(got[1], (200 + 0.375) / fps, places=6)
        self.assertAlmostEqual(got[2], (299 + 0.5 / 0.6) / fps, places=6)
        self.assertEqual(av_offset.video_onsets(t, y, 1.6, at_peak=True),
                         [101 / fps, 201 / fps, 302 / fps])

    def test_pairs_are_nearest_and_one_to_one(self):
        # 1.12 is nearest to 1.1, so 1.0 goes without; 2.6 takes 2.74, not
        # 2.80; 4.2 has nothing within the window; 5.3 is before its flash
        video = [1.0, 1.1, 2.6, 4.2, 5.8]
        audio = [1.12, 2.74, 2.80, 5.3, 7.0]
        self.assertEqual(av_offset.pair(video, audio, 0.8),
                         [(1.1, 1.12), (2.6, 2.74), (5.8, 5.3)])
        self.assertEqual(av_offset.pair(video, audio, 0.2), [(1.1, 1.12), (2.6, 2.74)])
        ms = av_offset.offsets_ms(av_offset.pair(video, audio, 0.8))
        np.testing.assert_allclose(ms, [20, 140, -500])

    def test_frame_times_from_showinfo(self):
        log = ("[Parsed_showinfo_2 @ 0x1] config in time_base: 1/600, frame_rate: 30/1\n"
               "[Parsed_showinfo_2 @ 0x1] n:   0 pts:      0 pts_time:0       duration: 20\n"
               "[Parsed_showinfo_2 @ 0x1]   side data - something n: 9 pts: 1\n"
               "[Parsed_showinfo_2 @ 0x1] n:   1 pts:     20 pts_time:0.0333333 duration: 20\n"
               "[Parsed_showinfo_2 @ 0x1] n:   2 pts:     41 pts_time:0.0683333 duration: 20\n")
        t, note = av_offset.frame_times(log, 3)
        np.testing.assert_allclose(t, [0, 20 / 600, 41 / 600])
        self.assertIsNone(note)
        t, note = av_offset.frame_times(log, 4)              # a frame without a line
        np.testing.assert_allclose(t, np.arange(4) / 30)
        self.assertIn("frame rate", note)
        with self.assertRaises(RuntimeError):
            av_offset.frame_times("nothing", 3)


@unittest.skipUnless(FFMPEG, "needs ffmpeg to build and decode the test videos")
class VideoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.dir = cls.tmp.name

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_finds_a_150_ms_delay(self):
        events = fan_events()
        path = make_video(self.dir, "late", 13.5, events, 0.150)
        code, out = run(path)
        self.assertEqual(code, 0, out)
        self.assertLess(abs(result_ms(out) - 150), 20, out)
        self.assertIn("flashes 8, bursts 8", out)
        self.assertIn("pairs 8 ", out)
        self.assertIn("video 60.00 fps", out)
        self.assertIn("uncalibrated", out)
        self.assertNotIn("record at 60 fps", out)
        a = av_offset.analyse(path, 1.6, 2000.0)
        ms = av_offset.offsets_ms(a.pairs)
        self.assertEqual(len(ms), 8)
        self.assertLess(ms.max() - ms.min(), 25, ms)          # within about a frame
        for (v, s), want in zip(a.pairs, events):
            self.assertLess(abs(v - want), 0.012, (v, want))   # flash onset to ~a frame
            self.assertLess(abs(s - want - 0.150), 0.004, (s, want))

    def test_zero_offset(self):
        path = make_video(self.dir, "zero", 13.5, fan_events(), 0.0)
        code, out = run(path)
        self.assertEqual(code, 0, out)
        self.assertLess(abs(result_ms(out)), 20, out)

    def test_late_audio_track_counts(self):
        # the audio track starts 0.2 s after the video's (AAC, with its encoder
        # delay): its samples play 0.2 s later, so the offset includes it
        path = make_video(self.dir, "track", 13.5, fan_events(), 0.150, audio_start=0.2)
        code, out = run(path)
        self.assertEqual(code, 0, out)
        self.assertLess(abs(result_ms(out) - 350), 20, out)

    def test_calibration_is_subtracted(self):
        fan = make_video(self.dir, "fan", 13.5, fan_events(), 0.150)
        cal_events = [0.707, 2.113, 3.319, 4.926, 6.231, 7.838]   # irregular, >= 1 s apart
        cal = make_video(self.dir, "cal", 9.0, cal_events, 0.030, kind="flash")
        code, out = run(fan, "--calibrate", cal)
        self.assertEqual(code, 0, out)
        self.assertLess(abs(result_ms(out) - 120), 20, out)
        m = re.search(r"minus the phone's own ([+-]\d+) ms", out)
        self.assertLess(abs(int(m[1]) - 30), 15, out)
        self.assertIn("calibration cal.mov", out)

    def test_clap_calibration(self):
        fan = make_video(self.dir, "fan2", 13.5, fan_events(), 0.150)
        claps = [0.93, 2.21, 3.64, 5.01, 6.52, 7.97]
        cal = make_video(self.dir, "clap", 9.5, claps, 0.0, kind="clap")
        code, out = run(fan, "--calibrate", cal, "--cal-clap")
        self.assertEqual(code, 0, out)
        m = re.search(r"minus the phone's own ([+-]\d+) ms", out)
        self.assertLess(abs(int(m[1])), 25, out)               # good to about a frame
        self.assertIn("claps 6, sounds 6", out)                # not the hands parting
        self.assertLess(abs(result_ms(out) - 150), 30, out)

    def test_too_few_pairs(self):
        path = make_video(self.dir, "short", 5.0, fan_events(3), 0.150)
        code, out = run(path)
        self.assertEqual(code, 2, out)
        self.assertIn("too few pairs (want 5): fan short.mov: 3", out)
        self.assertNotIn("result:", out)
        code, out = run(path, "--min-pairs", "3")
        self.assertEqual(code, 0, out)

    def test_30_fps_warns(self):
        path = make_video(self.dir, "slow", 13.5, fan_events(), 0.150, fps=30)
        code, out = run(path)
        self.assertEqual(code, 0, out)
        self.assertIn("30 fps: each flash time is good to about +-17 ms", out)
        self.assertLess(abs(result_ms(out) - 150), 25, out)

    def test_errors(self):
        code, out = run(os.path.join(self.dir, "missing.mov"))
        self.assertEqual(code, 1)
        self.assertIn("no such file", out)
        wav = os.path.join(self.dir, "only.wav")
        with wave.open(wav, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SR)
            w.writeframes(bytes(2 * SR))
        code, out = run(wav)
        self.assertEqual(code, 1)
        self.assertIn("cannot read the video", out)
        code, out = run(wav, "--hz", "7000")
        self.assertEqual((code, "--hz must be" in out), (1, True))


if __name__ == "__main__":
    unittest.main()
