#!/usr/bin/env python3
"""Tests for clipgen.py (standard library only).

Run from the repo root (-B: no __pycache__ in the tree):
    python3 -B -m unittest discover -s tools/holofan_clip -p 'test_clipgen.py' -v

The SW130 tests read ~/Downloads/SW130.BIN as data (read-only) and are
skipped when it is absent.
"""
import contextlib
import hashlib
import io
import os
import random
import struct
import sys
import tempfile
import unittest
import wave
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import clipgen  # noqa: E402

SW130 = os.path.expanduser("~/Downloads/SW130.BIN")


def read_png(data):
    """(width, height, rows of RGB bytes) from a PNG written by clipgen."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, chunks = 8, []
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])[0]
        assert crc == zlib.crc32(kind + body), kind
        chunks.append((kind, body))
        pos += 12 + length
    assert chunks[0][0] == b"IHDR" and chunks[-1][0] == b"IEND"
    width, height, depth, ctype = struct.unpack(">IIBB", chunks[0][1][:10])
    assert (depth, ctype) == (8, 2)
    raw = zlib.decompress(b"".join(b for k, b in chunks if k == b"IDAT"))
    stride = 1 + 3 * width
    rows = []
    for y in range(height):
        row = raw[y * stride:(y + 1) * stride]
        assert row[0] == 0  # filter type None
        rows.append(row[1:])
    return width, height, rows


class GeometryTests(unittest.TestCase):
    def test_frame_sizes(self):
        expect = {
            "48x300x5": (28_600, 0),
            "48x300x6": (34_000, 0),
            "64x520x6": (76_480, 0),
            "85x400x6": (78_400, 1),
            "64x520x6:32": (101_440, 64),
            "116x520x6": (138_880, 4),
        }
        for text, (frame, pad) in expect.items():
            g = clipgen.parse_geometry(text)
            self.assertEqual(g.frame_bytes, frame, text)
            self.assertEqual(g.pad_bits, pad, text)
            self.assertEqual(g.video_bytes + clipgen.AUDIO_BYTES, frame, text)
            self.assertEqual(g.name, text)

    def test_candidates_match_spec(self):
        got = [(c.number, c.geom.name, c.geom.frame_bytes, c.pitch)
               for c in clipgen.CANDIDATES]
        self.assertEqual(got, [
            (1, "48x300x5", 28_600, 500),
            (2, "48x300x6", 34_000, 700),
            (3, "64x520x6", 76_480, 900),
            (4, "85x400x6", 78_400, 1100),
            (5, "64x520x6:32", 101_440, 1300),
            (6, "116x520x6", 138_880, 1500),
        ])

    def test_parse_rejects_bad_geometry(self):
        for text in ("85x400x6:31", "0x400x6", "85x400", "85x400x0",
                     "85x400x9", "abc", "85x0x6"):
            with self.assertRaises(ValueError, msg=text):
                clipgen.parse_geometry(text)


class CodecTests(unittest.TestCase):
    def test_round_trip(self):
        rng = random.Random(1234)
        for text in ("48x300x5", "85x400x6", "64x520x6:32", "116x520x6",
                     "5x7x3", "3x4x2:4"):
            g = clipgen.parse_geometry(text)
            top = g.max_level
            lines = [[tuple(rng.randint(0, top) for _ in range(3))
                      for _ in range(g.lights)] for _ in range(g.lines)]
            video = clipgen.encode_video(g, lines)
            self.assertEqual(len(video), g.video_bytes, text)
            self.assertEqual(clipgen.decode_video(g, video), lines, text)
            # all-max levels set every data bit but no pad bit
            full = clipgen.encode_video(
                g, [[(top, top, top)] * g.lights] * g.lines)
            frame = full + bytes([clipgen.SILENCE]) * clipgen.AUDIO_BYTES
            clip = clipgen.HEADER + frame
            self.assertEqual(clipgen.pad_bits_set(g, clip), 0, text)
            ones = sum(bin(b).count("1") for b in full)
            self.assertEqual(ones, g.lines * g.depth * 3 * g.lights, text)

    def test_pad_bits_lead_and_bgr_order(self):
        # 85 LEDs: 32-byte planes, 1 leading pad bit, then rim LED B,G,R
        g = clipgen.parse_geometry("85x1x6")
        rim_red = [(63, 0, 0)] + [(0, 0, 0)] * 84
        video = clipgen.encode_video(g, [rim_red])
        for p in range(6):
            plane = video[p * 32:(p + 1) * 32]
            self.assertEqual(plane[0], 0b0001_0000)  # pad, B, G, [R]
            self.assertEqual(plane[1:], bytes(31))
        rim_blue = [(0, 0, 63)] + [(0, 0, 0)] * 84
        self.assertEqual(clipgen.encode_video(g, [rim_blue])[0], 0b0100_0000)
        hub_red = [(0, 0, 0)] * 84 + [(63, 0, 0)]
        plane = clipgen.encode_video(g, [hub_red])[:32]
        self.assertEqual(plane[31], 0b0000_0001)
        hub_blue = [(0, 0, 0)] * 84 + [(0, 0, 63)]
        self.assertEqual(clipgen.encode_video(g, [hub_blue])[31], 0b0000_0100)
        # no pad bits: 48 LEDs, rim green is the second bit
        g48 = clipgen.parse_geometry("48x1x5")
        rim_green = [(0, 31, 0)] + [(0, 0, 0)] * 47
        self.assertEqual(clipgen.encode_video(g48, [rim_green])[0], 0b0100_0000)
        # wide planes: 64 LEDs in 32 bytes, 64 leading pad bits
        g32 = clipgen.parse_geometry("64x1x6:32")
        plane = clipgen.encode_video(g32, [[(0, 0, 63)] + [(0, 0, 0)] * 63])
        self.assertEqual(plane[:8], bytes(8))
        self.assertEqual(plane[8], 0b1000_0000)

    def test_msb_plane_first(self):
        g = clipgen.parse_geometry("48x1x6")
        rim = lambda level: [(0, 0, level)] + [(0, 0, 0)] * 47  # noqa: E731
        msb = clipgen.encode_video(g, [rim(32)])
        lsb = clipgen.encode_video(g, [rim(1)])
        self.assertEqual([msb[p * 18] for p in range(6)], [0x80, 0, 0, 0, 0, 0])
        self.assertEqual([lsb[p * 18] for p in range(6)], [0, 0, 0, 0, 0, 0x80])

    def test_pad_bits_set_counts_dirty_planes(self):
        g = clipgen.parse_geometry("85x400x6")
        clip = bytearray(clipgen.HEADER + bytes(2 * g.frame_bytes))
        self.assertEqual(clipgen.pad_bits_set(g, clip), 0)
        clip[812 + g.frame_bytes + 5 * 32] = 0x80   # frame 1, plane 5
        clip[812 + 7 * 32] = 0x7F                   # data bits only
        self.assertEqual(clipgen.pad_bits_set(g, clip), 1)
        g32 = clipgen.parse_geometry("64x2x6:32")
        clip = bytearray(clipgen.HEADER + bytes(g32.frame_bytes))
        clip[812 + 32 + 7] = 0x01                   # plane 1, pad byte 7
        clip[812 + 64 + 8] = 0xFF                   # plane 2, data byte
        self.assertEqual(clipgen.pad_bits_set(g32, clip), 1)


class ClipTests(unittest.TestCase):
    def test_header(self):
        self.assertEqual(len(clipgen.HEADER), 812)
        self.assertEqual(hashlib.sha256(clipgen.HEADER).hexdigest(),
                         clipgen.HEADER_SHA256)
        self.assertEqual(clipgen.HEADER[:2], b"\xee\x37")

    @unittest.skipUnless(os.path.exists(SW130), "no ~/Downloads/SW130.BIN")
    def test_header_matches_sw130(self):
        with open(SW130, "rb") as f:
            self.assertEqual(f.read(812), clipgen.HEADER)

    def test_padding(self):
        self.assertEqual(clipgen.pad_to_window(b""), b"")
        one = clipgen.pad_to_window(b"\x01")
        self.assertEqual(len(one), 28_800)
        self.assertEqual(one[1:], bytes(28_799))
        full = bytes(range(256)) * 225  # 57,600 B, already 2 windows
        self.assertEqual(clipgen.pad_to_window(full), full)
        more = clipgen.pad_to_window(full + b"\x02")
        self.assertEqual(len(more), 86_400)

    def test_crc_of_padded_bytes(self):
        data = os.urandom(50_000)
        padded = data + bytes(57_600 - 50_000)
        self.assertEqual(clipgen.crc32_padded(data), zlib.crc32(padded))
        self.assertEqual(clipgen.crc32_padded(padded), zlib.crc32(padded))

    def test_build_clip_layout(self):
        g = clipgen.parse_geometry("64x520x6")
        clip = clipgen.build_clip(g, 60, "black", "tone:440")
        raw = 812 + 60 * 76_480
        self.assertEqual(len(clip), 160 * 28_800)
        self.assertGreater(len(clip), raw)
        self.assertEqual(clip[:812], clipgen.HEADER)
        self.assertEqual(clip[raw:], bytes(len(clip) - raw))
        self.assertEqual(clipgen.frames_in(g, len(clip)), 60)
        self.assertEqual(clip[812:812 + g.video_bytes], bytes(g.video_bytes))
        audio = clipgen.clip_audio(clip, g, 60)
        self.assertEqual(len(audio), 60 * 1600)
        beeps = clipgen.detect_beeps(audio)
        self.assertAlmostEqual(beeps.freq_hz, 440, delta=3)

    def test_video_patterns(self):
        g = clipgen.parse_geometry("48x300x5")
        white = (31, 31, 31)
        rings = clipgen.video_pattern(g, "rings")
        for line in rings:
            self.assertEqual(line[0], white)                 # rim ring
            for led in clipgen.RING_LEDS:
                self.assertEqual(line[led], white)
            self.assertEqual(line[g.lights - 1], (0, 0, 0))  # hub dark
        spokes = clipgen.video_pattern(g, "spokes")
        self.assertEqual(spokes[0], [white] * 48)            # full spoke
        quarter = spokes[300 // 4]
        self.assertEqual(quarter[0], white)                  # short spoke at rim
        self.assertEqual(quarter[47], (0, 0, 0))             # not to the hub
        self.assertEqual(spokes[300 // 2], [(0, 0, 0)] * 48)
        solid = clipgen.video_pattern(g, "solid:1,2,3")
        self.assertEqual(solid[17][9], (1, 2, 3))
        self.assertEqual(clipgen.video_pattern(g, "black")[5][5], (0, 0, 0))
        with self.assertRaises(ValueError):
            clipgen.video_pattern(g, "solid:32,0,0")         # 5-bit max 31
        with self.assertRaises(ValueError):
            clipgen.video_pattern(g, "plaid")

    def test_audio_specs(self):
        n = 32 * 1600
        self.assertEqual(clipgen.audio_samples("silence", n), b"\x80" * n)
        tone = clipgen.audio_samples("tone:1000", n)
        self.assertEqual(len(tone), n)
        self.assertAlmostEqual(clipgen.detect_beeps(tone).freq_hz, 1000, delta=3)
        beeps = clipgen.detect_beeps(clipgen.audio_samples("beeps:3:800", 3 * n))
        self.assertEqual(beeps.count, 3)
        self.assertAlmostEqual(beeps.freq_hz, 800, delta=3)
        self.assertEqual(beeps.per_cycle, [3, 3, 3])
        with self.assertRaises(ValueError):
            clipgen.audio_samples("beeps:11", n)              # > one cycle
        with self.assertRaises(ValueError):
            clipgen.audio_samples("noise", n)


class SyncClipTests(unittest.TestCase):
    """flash:N video and burst:N audio, the av_offset.py sync clip."""

    def test_flash_frames_are_white_and_only_those(self):
        g = clipgen.parse_geometry("8x12x3")
        frames = 70
        clip = clipgen.build_clip(g, frames, "flash:32", "silence")
        white = [(7, 7, 7)] * 8
        black = [(0, 0, 0)] * 8
        lit = []
        for f in range(frames):
            start = g.frame_offset(f)
            lines = clipgen.decode_video(g, clip[start:start + g.video_bytes])
            if lines == [white] * 12:
                lit.append(f)
            else:
                self.assertEqual(lines, [black] * 12, f)
        self.assertEqual(lit, [0, 32, 64])
        self.assertEqual(clipgen.video_pattern(g, "flash:5", 10), [white] * 12)
        self.assertEqual(clipgen.video_pattern(g, "flash:5", 11), [black] * 12)
        self.assertEqual(clipgen.video_pattern(g, "flash:1", 7), [white] * 12)
        for bad in ("flash:0", "flash:", "flash:x", "flash:-3", "flash:3:4"):
            with self.assertRaises(ValueError, msg=bad):
                clipgen.video_pattern(g, bad)
            with self.assertRaises(ValueError, msg=bad):
                clipgen.build_clip(g, 4, bad)

    def test_burst_lines_up_with_the_flash_frames(self):
        g = clipgen.parse_geometry("64x520x6")
        frames = 100
        clip = clipgen.build_clip(g, frames, "flash:32", "burst:32")
        audio = clipgen.clip_audio(clip, g, frames)
        self.assertEqual(len(audio), frames * 1600)
        burst = clipgen.ramped_tone(2000.0, 1600)
        for f in range(frames):
            start = g.frame_offset(f)
            slot = audio[f * 1600:(f + 1) * 1600]
            flash = clip[start:start + g.video_bytes] != bytes(g.video_bytes)
            self.assertEqual(flash, f % 32 == 0, f)
            if flash:
                self.assertEqual(slot, burst, f)              # the whole 50 ms slot
            else:
                self.assertEqual(slot, b"\x80" * 1600, f)     # silence elsewhere
        # the burst sounds from the slot's first sample to its last: only the
        # 2 ms ramp ends are quiet, and it peaks at full amplitude
        loud = [i for i, b in enumerate(burst) if abs(b - 0x80) > 8]
        self.assertLess(loud[0], clipgen.RAMP)
        self.assertGreater(loud[-1], 1600 - clipgen.RAMP)
        self.assertEqual(max(burst) - 0x80, clipgen.AMPLITUDE)
        self.assertAlmostEqual(clipgen.dominant_frequency(
            [b - 0x80 for b in burst], [(0, 1600)]), 2000, delta=10)

    def test_burst_specs(self):
        n = 5 * 1600
        x = clipgen.audio_samples("burst:2:1000", n)
        self.assertEqual(x, clipgen.burst_samples(2, 1000.0, n))
        tone = clipgen.ramped_tone(1000.0, 1600)
        self.assertEqual(x, tone + b"\x80" * 1600 + tone + b"\x80" * 1600 + tone)
        self.assertEqual(clipgen.audio_samples("burst:3", n),
                         clipgen.burst_samples(3, clipgen.BURST_HZ, n))
        self.assertEqual(clipgen.BURST_HZ, 2000.0)
        short = clipgen.audio_samples("burst:1", 800)       # a cut-off burst
        self.assertEqual(short, clipgen.ramped_tone(2000.0, 1600)[:800])
        for bad in ("burst", "burst:0", "burst:x", "burst:2:5", "burst:2:2000:1"):
            with self.assertRaises(ValueError, msg=bad):
                clipgen.audio_samples(bad, n)

    def test_beeps_unchanged_by_the_ramp_helper(self):
        # beep_samples now shares ramped_tone with the bursts; the probe tests
        # pin its bytes, this pins the envelope
        beep = clipgen.ramped_tone(700.0, clipgen.BEEP_SAMPLES)
        x = clipgen.beep_samples(2, 700.0, clipgen.CYCLE_SAMPLES)
        self.assertEqual(x[:clipgen.BEEP_SAMPLES], beep)
        self.assertEqual(x[2 * clipgen.BEEP_SAMPLES:3 * clipgen.BEEP_SAMPLES], beep)
        self.assertEqual(beep[0], 0x80)
        self.assertEqual(beep[-1], 0x80)

    def test_lengths_and_padding_unchanged(self):
        for text in ("64x520x6", "85x400x6", "48x300x5"):
            g = clipgen.parse_geometry(text)
            for frames in (1, 33, 160):
                plain = clipgen.build_clip(g, frames)
                sync = clipgen.build_clip(g, frames, "flash:32", "burst:32:2000")
                self.assertEqual(len(sync), len(plain), (text, frames))
                self.assertEqual(len(sync) % 28_800, 0)
                self.assertEqual(sync[:812], clipgen.HEADER)
                raw = 812 + frames * g.frame_bytes
                self.assertEqual(sync[raw:], bytes(len(sync) - raw))
                self.assertEqual(clipgen.frames_in(g, len(sync)), frames)
                self.assertEqual(clipgen.pad_bits_set(g, sync), 0)
                # frames that do not flash are the black clip's frames
                for f in (1, frames - 1):
                    if f % 32:
                        a, b = g.frame_offset(f), g.frame_offset(f) + g.video_bytes
                        self.assertEqual(sync[a:b], plain[a:b])

    def test_sync_clip_cli(self):
        # the README's 06-Sync.bin: 8 s, a flash and a burst every 1.6 s
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "06-Sync.bin")
            with contextlib.redirect_stdout(out):
                code = clipgen.main(["clip", "--geom", "64x520x6", "--seconds", "8",
                                     "--video", "flash:32", "--audio", "burst:32:2000", path])
            self.assertEqual(code, 0)
            with open(path, "rb") as f:
                data = f.read()
        text = out.getvalue()
        self.assertIn("frames 160, seconds 8.00", text)
        self.assertIn("flash frames 0, 32, 64, ...: every 1.60 s, 5 in the clip", text)
        self.assertIn(f"{zlib.crc32(data):08x}", text)
        self.assertNotIn("note:", text)
        self.assertEqual(len(data), 28_800 * -(-(812 + 160 * 76_480) // 28_800))
        # still gets the short-clip note
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as tmp:
            with contextlib.redirect_stdout(out):
                clipgen.main(["clip", "--geom", "64x520x6", "--frames", "20",
                              "--video", "flash:32", "--audio", "burst:32",
                              os.path.join(tmp, "s.bin")])
        self.assertIn("1 in the clip", out.getvalue())
        self.assertIn("note: 0.95 s at the shortest", out.getvalue())   # 19 frames at 85x400x6


class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data, cls.rows = clipgen.build_probe()

    def test_size_header_crc(self):
        self.assertEqual(len(self.data), 3_456_000)
        self.assertEqual(self.data[:812], clipgen.HEADER)
        self.assertEqual(clipgen.crc32_padded(self.data), zlib.crc32(self.data))

    def test_frames_and_totals(self):
        got = [(r.number, r.frames, r.total) for r in self.rows]
        self.assertEqual(sorted(got), [(1, 120, 6), (2, 101, 5), (3, 45, 2),
                                       (4, 44, 2), (5, 34, 1), (6, 24, 1)])
        for r in self.rows:
            self.assertGreaterEqual(r.seconds, 1.2)

    def test_self_decode_names_each_candidate(self):
        for c in clipgen.CANDIDATES:
            n = clipgen.frames_in(c.geom, len(self.data))
            beeps = clipgen.detect_beeps(clipgen.clip_audio(self.data, c.geom, n))
            self.assertEqual(beeps.count, c.number, c.geom.name)
            self.assertAlmostEqual(beeps.freq_hz, c.pitch, delta=5, msg=c.geom.name)
            # every cycle, the partial last one included, reads like the
            # candidate's own undisturbed stream
            clean = clipgen.detect_beeps(
                clipgen.beep_samples(c.number, c.pitch, n * clipgen.AUDIO_BYTES))
            self.assertEqual(beeps.per_cycle, clean.per_cycle, c.geom.name)
        for r in self.rows:
            self.assertEqual(r.beeps, r.number)
            self.assertGreater(r.intact, 0.9)
            self.assertGreaterEqual(r.worst_beep, 0.6)

    def test_long_strides_hear_little_foreign_sound(self):
        # 4 (F-S12+, the PRO+ / SW130 format) and 5 and 6 (about one cycle
        # each) hear no other candidate's audio, which a listener could count
        # as one more beep. 3 (F-S12, what the MCU reports) hears only the
        # one 40 ms clash with candidate 4 described at PROBE_ORDER.
        rows = {r.number: r for r in self.rows}
        for number in (4, 5, 6):
            self.assertEqual(rows[number].strays, [], number)
        self.assertEqual(rows[3].strays, [(1, 410, 450)])
        self.assertEqual(rows[3].stray_from, [(4,)])
        self.assertEqual(rows[3].cuts, [])
        for r in self.rows:
            n = clipgen.frames_in(r.geom, len(self.data))
            want = clipgen.beep_samples(r.number, r.pitch, n * clipgen.AUDIO_BYTES)
            got = clipgen.clip_audio(self.data, r.geom, n)
            self.assertEqual((r.strays, r.cuts), clipgen.foreign_spans(want, got))

    def test_short_strides_strays_are_pinned(self):
        # 1 and 2 (the 48-LED strides) take most of the overlap damage; pin
        # it, and which candidate's audio each stray is, so that a change to
        # the write order or the cycle shows up here
        rows = {r.number: r for r in self.rows}
        self.assertEqual(rows[1].strays, [
            (0, 500, 519), (1, 80, 93), (1, 313, 350), (1, 550, 578),
            (2, 150, 185), (2, 875, 900), (3, 500, 519), (3, 1041, 1050)])
        self.assertEqual(rows[1].stray_from,
                         [(4,), (6,), (5,), (2,), (6,), (2,), (3,), (5,)])
        self.assertEqual(rows[1].cuts, [(1, 50, 80)])
        # candidate 2's first burst: 1,300 Hz between its two beeps, then
        # 900 Hz twice in the quiet after it
        self.assertEqual(rows[2].strays, [
            (0, 100, 133), (0, 407, 448), (0, 850, 895), (1, 817, 828),
            (2, 800, 828), (2, 922, 950), (2, 1250, 1275)])
        self.assertEqual(rows[2].stray_from,
                         [(5,), (3,), (3,), (6,), (3,), (4,), (3,)])
        self.assertEqual(rows[2].cuts, [(1, 160, 190)])

    def test_stray_sources_are_the_last_writers(self):
        # independent of build_probe: replay the write order byte by byte
        owner = bytearray(len(self.data))
        by_number = {c.number: c for c in clipgen.CANDIDATES}
        for number in clipgen.PROBE_ORDER:
            g = by_number[number].geom
            for f in range(clipgen.frames_in(g, len(self.data))):
                off = g.audio_offset(f)
                owner[off:off + 1600] = bytes([number]) * 1600
        ms = clipgen.SAMPLE_RATE // 1000
        for r in self.rows:
            want = clipgen.beep_samples(r.number, r.pitch, r.frames * 1600)
            got = clipgen.clip_audio(self.data, r.geom, r.frames)
            self.assertEqual(len(r.stray_from), len(r.strays), r.number)
            for (cycle, s, e), source in zip(r.strays, r.stray_from):
                a = cycle * clipgen.CYCLE_SAMPLES + s * ms
                b = min(len(want), cycle * clipgen.CYCLE_SAMPLES + e * ms)
                who = {owner[r.geom.audio_offset(t // 1600) + t % 1600]
                       for t in range(a, b) if want[t] != got[t]}
                self.assertEqual(source, tuple(sorted(who)), (r.number, cycle, s))
                self.assertNotIn(r.number, source)

    def test_every_stride_reads_other_audio_as_video(self):
        # no byte outside the slots is set (test_non_slot_bytes_are_black),
        # yet each stride's video region covers other candidates' audio
        # slots, so the fan shows coloured wedges whatever stride it uses
        for r in self.rows:
            g = r.geom
            lit = sum(g.video_bytes
                      - self.data[g.frame_offset(f):g.frame_offset(f) + g.video_bytes].count(0)
                      for f in range(r.frames))
            self.assertAlmostEqual(r.lit, lit / (r.frames * g.video_bytes), msg=r.number)
            self.assertGreater(r.lit, 0.1, r.number)

    def test_probe_needs_a_frame_per_candidate(self):
        # candidate 6 needs 812 + 138,880 B, i.e. 5 windows, for one frame
        for windows in (4, 1, 0, -1):
            with self.assertRaisesRegex(ValueError, "at least 5 windows"):
                clipgen.build_probe(windows)
        _, rows = clipgen.build_probe(5)
        self.assertEqual(min(r.frames for r in rows), 1)

    def test_per_block_pitch_keeps_a_beep_with_a_foreign_tail(self):
        n = 2 * clipgen.CYCLE_SAMPLES
        audio = bytearray(clipgen.beep_samples(2, 600, n))
        tail = clipgen.tone_samples(1500, 30 * 32)          # last 30 ms of beep 1
        start = clipgen.CYCLE_SAMPLES + clipgen.BEEP_SAMPLES - len(tail)
        audio[start:start + len(tail)] = tail
        self.assertEqual(clipgen.detect_beeps(bytes(audio)).per_cycle, [2, 2])

    def test_foreign_blip_in_a_gap_does_not_join_two_beeps(self):
        # a probe overlap can leave 40 ms of another candidate's beep in the
        # 80 ms gap, 20 ms from each beep (candidate 2, cycle 0, at 120 windows)
        n = 2 * clipgen.CYCLE_SAMPLES
        audio = bytearray(clipgen.beep_samples(2, 700, n))
        blip = clipgen.tone_samples(1300, 40 * 32)
        for cycle in range(0, n, clipgen.CYCLE_SAMPLES):
            start = cycle + clipgen.BEEP_SAMPLES + 20 * 32
            audio[start:start + len(blip)] = blip
        beeps = clipgen.detect_beeps(bytes(audio))
        self.assertEqual(beeps.per_cycle, [2, 2])
        self.assertAlmostEqual(beeps.freq_hz, 700, delta=5)

    def test_foreign_spans_name_cycle_and_milliseconds(self):
        ms = clipgen.SAMPLE_RATE // 1000
        cycle = clipgen.CYCLE_SAMPLES
        want = clipgen.beep_samples(2, 700, 2 * cycle)
        got = bytearray(want)
        got[100 * ms:130 * ms] = clipgen.tone_samples(1300, 30 * ms)   # in the gap
        got[cycle + 170 * ms:cycle + 190 * ms] = bytes([clipgen.SILENCE]) * (20 * ms)
        got[cycle + 900 * ms:cycle + 905 * ms] = b"\x81" * (5 * ms)     # rest of cycle 1
        strays, cuts = clipgen.foreign_spans(want, bytes(got))
        self.assertEqual(strays, [(0, 100, 130), (1, 900, 905)])
        self.assertEqual(cuts, [(1, 170, 190)])
        self.assertEqual(clipgen.foreign_spans(want, want), ([], []))

    def test_non_slot_bytes_are_black(self):
        slots = bytearray(len(self.data))
        for c in clipgen.CANDIDATES:
            for i in range(clipgen.frames_in(c.geom, len(self.data))):
                off = c.geom.audio_offset(i)
                slots[off:off + 1600] = b"\x01" * 1600
        stray = [i for i in range(812, len(self.data))
                 if not slots[i] and self.data[i]]
        self.assertEqual(stray, [])


class OutputTests(unittest.TestCase):
    def test_png_disc(self):
        g = clipgen.parse_geometry("8x40x2")
        video = clipgen.encode_video(g, clipgen.video_pattern(g, "spokes"))
        width, height, rows = read_png(clipgen.render_png(g, video, scale=4))
        self.assertEqual((width, height), (64, 64))
        c = width // 2
        top = rows[2][3 * c:3 * c + 3]           # 12 o'clock, near the rim
        bottom = rows[height - 3][3 * c:3 * c + 3]
        self.assertEqual(top, b"\xff\xff\xff")   # line 0 spoke
        self.assertEqual(bottom, b"\x00\x00\x00")
        right = rows[c][3 * (width - 3):3 * (width - 2)]
        self.assertEqual(right, b"\xff\xff\xff")  # lines/4 = 3 o'clock (clockwise)
        corner = rows[0][0:3]
        self.assertNotEqual(corner, b"\xff\xff\xff")  # outside the disc

    def test_png_colour_order(self):
        g = clipgen.parse_geometry("4x8x2")
        lines = [[(3, 0, 0)] * 4] * 8                 # red everywhere
        _, _, rows = read_png(clipgen.render_png(
            g, clipgen.encode_video(g, lines), scale=4))
        self.assertEqual(rows[16][3 * 16:3 * 16 + 3], b"\xff\x00\x00")

    def test_wav(self):
        audio = clipgen.audio_samples("tone:440", 3200)
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "a.wav")
            clipgen.write_wav(path, audio)
            with wave.open(path, "rb") as w:
                self.assertEqual((w.getnchannels(), w.getsampwidth(),
                                  w.getframerate(), w.getnframes()),
                                 (1, 1, 32_000, 3200))
                self.assertEqual(w.readframes(3200), audio)


class CliTests(unittest.TestCase):
    def run_main(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = clipgen.main(list(argv))
        return code, out.getvalue()

    def test_clip_then_crc_and_inspect(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "c.bin")
            code, out = self.run_main("clip", "--geom", "48x300x5",
                                      "--frames", "40", "--video", "rings",
                                      "--audio", "beeps:2:600", path)
            self.assertEqual(code, 0)
            with open(path, "rb") as f:
                data = f.read()
            crc = f"{zlib.crc32(data):08x}"
            self.assertIn(crc, out)
            self.assertEqual(len(data) % 28_800, 0)
            code, out = self.run_main("crc", path)
            self.assertEqual((code, out.split()[0]), (0, crc))
            png = os.path.join(tmp, "f.png")
            wav = os.path.join(tmp, "a.wav")
            code, out = self.run_main("inspect", "--geom", "48x300x5",
                                      "--png", "3", png, "--wav", wav, path)
            self.assertEqual(code, 0)
            self.assertIn("frames 40", out)
            self.assertIn("pad bits: none in this geometry", out)
            self.assertIn("beeps 2", out)
            self.assertTrue(os.path.getsize(png) > 0)
            with wave.open(wav, "rb") as w:
                self.assertEqual(w.getnframes(), 40 * 1600)

    def test_clip_seconds(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "c.bin")
            code, out = self.run_main("clip", "--geom", "85x400x6",
                                      "--seconds", "1.5", path)
            self.assertEqual(code, 0)
            self.assertIn("frames 30", out)
            self.assertEqual(os.path.getsize(path),
                             clipgen.WINDOW_BYTES * -(-(812 + 30 * 78_400) // 28_800))

    def test_clip_notes_the_fan_strides(self):
        # 3 s of 48x300x5 is 22 frames (1.10 s) at the PRO+ and F-S12 strides:
        # short enough that the MCU may leave it unlisted
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "c.bin")
            code, out = self.run_main("clip", "--geom", "48x300x5",
                                      "--seconds", "3", path)
            self.assertEqual(code, 0)
            self.assertIn("frames 60, seconds 3.00", out)
            self.assertIn("85x400x6 (78,400 B frames): frames 22, seconds 1.10", out)
            self.assertIn("64x520x6 (76,480 B frames): frames 22, seconds 1.10", out)
            self.assertIn("note: 1.10 s at the shortest", out)
            # 6 s is 44 and 45 frames there
            code, out = self.run_main("clip", "--geom", "48x300x5",
                                      "--seconds", "6", path)
            self.assertEqual(code, 0)
            self.assertIn("85x400x6 (78,400 B frames): frames 44, seconds 2.20", out)
            self.assertIn("64x520x6 (76,480 B frames): frames 45, seconds 2.25", out)
            self.assertNotIn("note:", out)
            # short in its own geometry still gets the note
            code, out = self.run_main("clip", "--geom", "85x400x6",
                                      "--frames", "20", path)
            self.assertIn("note: 1.00 s at the shortest", out)

    def test_probe_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "p.bin")
            code, out = self.run_main("probe", path)
            self.assertEqual(code, 0)
            with open(path, "rb") as f:
                data = f.read()
            self.assertEqual(len(data), 120 * 28_800)
            self.assertIn(f"{zlib.crc32(data):08x}", out)
            self.assertNotIn("MISMATCH", out)
            self.assertIn("3: foreign sound c1 410-450 ms (4: 1100 Hz); cut none", out)
            self.assertIn("2: foreign sound c0 100-133 ms (5: 1300 Hz), "
                          "c0 407-448 ms (3: 900 Hz), c0 850-895 ms (3: 900 Hz), ", out)
            self.assertIn("4: foreign sound none; cut c1 550-560 ms", out)
            self.assertIn("6: foreign sound none; cut none", out)
            # the video is not black: say so, with each stride's share
            self.assertIn("video: ", out)
            self.assertIn("4 14.1%", out)
            self.assertIn("not corruption", out)
            # 60 windows: candidate 6 gets 0.6 s, too short for 6 beeps
            code, out = self.run_main("probe", "--windows", "60", path)
            self.assertEqual(code, 1)
            self.assertIn("MISMATCH", out)
            # 4 windows: candidate 6 gets no frame, a clean error
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code, out = self.run_main("probe", "--windows", "4", path)
            self.assertEqual(code, 2)
            self.assertIn("error: 4 windows", err.getvalue())


@unittest.skipUnless(os.path.exists(SW130), "no ~/Downloads/SW130.BIN")
class Sw130Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(SW130, "rb") as f:
            cls.data = f.read()

    def test_inspect_as_pro_plus(self):
        g = clipgen.parse_geometry("85x400x6")
        report = clipgen.inspect_clip(self.data, g)
        self.assertEqual(report.frames, 78)
        self.assertEqual(report.pad_planes, 0)
        self.assertEqual(report.crc, 0xF11AF02B)
        for frame, mean, step in report.audio:
            self.assertLess(abs(mean - 128), 10, frame)
            self.assertLess(step, 6, frame)
        self.assertLess(report.median_step, 3)

    def test_wrong_stride_is_rough(self):
        g = clipgen.parse_geometry("64x520x6")
        self.assertGreater(clipgen.inspect_clip(self.data, g).median_step, 10)


if __name__ == "__main__":
    unittest.main()
