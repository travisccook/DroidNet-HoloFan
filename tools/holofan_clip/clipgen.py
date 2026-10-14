#!/usr/bin/env python3
"""HoloFan test clips for any frame geometry, a stride probe, and inspection.

Python 3.11 standard library only. The clip layout below is the one vendor
clips have; docs/HOW_IT_WORKS.md ("Clips") has the header, frame size, audio
and padding as measured on a vendor clip. The SW130 tests check the pad bits
and the audio of the 85x400x6 stride:

    file    812-byte constant header (no geometry or length in it), then
            frames, zero-padded to a multiple of 28,800 bytes
    frame   LINES lines x DEPTH bit-planes of video, then 1,600 audio bytes
    plane   ceil(3*LIGHTS/8) bytes (or PLANEBYTES): leading zero pad bits,
            then per LED (rim first, hub last) its B, G, R bits, packed
            MSB-first; plane 0 carries the most significant level bit
    line l  angle 2*pi*l/LINES
    audio   unsigned 8-bit mono PCM, 32,000 Hz, 0x80 = silence, 20 fps

Geometry is LIGHTSxLINESxDEPTH[:PLANEBYTES]: 85x400x6 (Pixel Arc ATOM PRO+,
vendor app F-S12+, 78,400 B frames), 64x520x6 (vendor app F-S12, the model
this fan reports, 76,480 B), 48x300x5 (ATOM SE, 28,600 B).

    clipgen.py clip --geom 64x520x6 --seconds 3 --video rings --audio tone:440 out.bin
    clipgen.py clip --geom 64x520x6 --seconds 8 --video flash:32 --audio burst:32:2000 sync.bin
    clipgen.py probe [--windows 120] [--order 1,2,3,4,5,6] probe.bin
    clipgen.py inspect --geom 85x400x6 [--wav a.wav] [--png 40 f.png] SW130.BIN
    clipgen.py crc file.bin

The probe carries a different beep pattern in the audio slots of each
candidate frame stride (CANDIDATES): played on the fan, the number of beeps
per 1.6 s and their pitch name the stride the MCU uses. Its report lists,
per candidate, the other candidates' sound and the beep cuts that slot
overlaps leave in that stride's playback. Its bytes outside the audio slots
are 0, but every stride reads the other candidates' slots as LED data, so
the fan shows coloured wedges, striped or speckled like snow, whatever the
stride: judge the probe by ear only. "crc" is what firmware 0.7.2 reports
as crc= in /api/info "last".

flash:N lights every LED full white in frames 0, N, 2N, ... (black
otherwise) and burst:N puts a 50 ms tone in the audio of exactly those
frames: an audio/video sync clip for tools/bench/av_offset.py (a phone video
of the fan gives the delay between the flash and the burst).
"""
import argparse
import collections
import math
import re
import struct
import sys
import wave
import zlib
from dataclasses import dataclass

HEADER_LEN = 812
HEADER_SHA256 = "2490260f72703e40982554bc0251bf0cb482a73e2c78b016794a7ba158a5407a"
# The vendor converter's constant 812-byte header, as published in
# github.com/zackmcmurrin-dev/holo-fan-linux (MIT, protocol/ee37-header.bin);
# equal to the first 812 bytes of every Pixel Arc SW clip.
HEADER = bytes.fromhex(
    "ee3799b7747775b7717770b7b277b3b77b777ab7b877b9b7bd77bcb77e777fb7"
    "6f776eb7ac77adb7a977a8b76a776bb7a377a2b7607761b7657764b7a677a7b7"
    "477746b7847785b7817780b7427743b78b778ab7487749b74d774cb78e778fb7"
    "9f779eb75c775db7597758b79a779bb7537752b7907791b7957794b7567757b7"
    "177716b7d477d5b7d177d0b7127713b7db77dab7187719b71d771cb7de77dfb7"
    "cf77ceb70c770db7097708b7ca77cbb7037702b7c077c1b7c577c4b7067707b7"
    "e777e6b7247725b7217720b7e277e3b72b772ab7e877e9b7ed77ecb72e772fb7"
    "3f773eb7fc77fdb7f977f8b73a773bb7f377f2b7307731b7357734b7f677f7f7"
    "37f6f7343735f7313730f7f237f3f73b373af7f837f9f7fd37fcf73e373ff72f"
    "372ef7ec37edf7e937e8f72a372bf7e337e2f7203721f7253724f7e637e7f707"
    "3706f7c437c5f7c137c0f7023703f7cb37caf7083709f70d370cf7ce37cff7df"
    "37def71c371df7193718f7da37dbf7133712f7d037d1f7d537d4f7163717f757"
    "3756f7943795f7913790f7523753f79b379af7583759f75d375cf79e379ff78f"
    "378ef74c374df7493748f78a378bf7433742f7803781f7853784f7463747f7a7"
    "37a6f7643765f7613760f7a237a3f76b376af7a837a9f7ad37acf76e376ff77f"
    "377ef7bc37bdf7b937b8f77a377bf7b337b2f7703771f7753774f7b637b7b777"
    "b6b7747775b7717770b7b277b3b77b777ab7b877b9b7bd77bcb77e777fb76f77"
    "6eb7ac77adb7a977a8b76a776bb7a377a2b7607761b7657764b7a677a7b74777"
    "46b7847785b7817780b7427743b78b778ab7487749b74d774cb78e778fb79f77"
    "9eb75c775db7597758b79a779bb7537752b7907791b7957794b7567757b71777"
    "16b7d477d5b7d177d0b7127713b7db77dab7187719b71d771cb7de77dfb7cf77"
    "ceb70c770db7097708b7ca77cbb7037702b7c077c1b7c577c4b7067707b7e777"
    "e6b7247725b7217720b7e277e3b72b772ab7e877e9b7ed77ecb72e772fb73f77"
    "3eb7fc77fdb7f977f8b73a773bb7f377f2b7307731b7357734b7f677f7f737f6"
    "f7343735f7313730f7f237f3f73b373af7f837f9f7fd37fcf73e373ff72f372e"
    "f7ec37edf7e937e8f72a372b"
)

AUDIO_BYTES = 1600            # per frame
FPS = 20
SAMPLE_RATE = AUDIO_BYTES * FPS
MS = SAMPLE_RATE // 1000      # samples per millisecond
SILENCE = 0x80
WINDOW_BYTES = 28_800         # the fan's upload window; files pad to it
LISTED_SECONDS = 1.2          # a margin, not a measurement: clips under "~1 s"
                              # may never be listed, and an unlisted clip breaks
                              # every later upload until a format; the benches of
                              # 2026-10-06 saw only 1-frame clips unlisted (the
                              # 20-frame threshold test aborted)

AMPLITUDE = 96                # tone and beep peak, in u8 steps around 0x80
RAMP = 64                     # 2 ms raised-cosine edges on each beep
CYCLE_FRAMES = 32             # beep pattern repeats every 1.6 s
CYCLE_SAMPLES = CYCLE_FRAMES * AUDIO_BYTES
BEEP_SAMPLES = SAMPLE_RATE * 80 // 1000   # 80 ms on, then 80 ms off
BURST_SAMPLES = AUDIO_BYTES   # a sync burst fills one frame's audio (50 ms)
BURST_HZ = 2000.0
MAX_BEEPS = (CYCLE_SAMPLES - BEEP_SAMPLES) // (2 * BEEP_SAMPLES) + 1

RING_LEDS = (0, 8, 16, 32)    # rim ring + rings 8, 16, 32 LEDs in (gaps 8, 8, 16)
SPOKE_HALF = 1                # spokes are 3 lines wide
BACKGROUND = b"\x28\x28\x28"  # PNG pixels outside the disc


@dataclass(frozen=True)
class Geometry:
    lights: int
    lines: int
    depth: int
    plane_bytes: int

    @property
    def pad_bits(self):
        return 8 * self.plane_bytes - 3 * self.lights

    @property
    def line_bytes(self):
        return self.depth * self.plane_bytes

    @property
    def video_bytes(self):
        return self.lines * self.line_bytes

    @property
    def frame_bytes(self):
        return self.video_bytes + AUDIO_BYTES

    @property
    def max_level(self):
        return (1 << self.depth) - 1

    @property
    def name(self):
        base = f"{self.lights}x{self.lines}x{self.depth}"
        if self.plane_bytes == min_plane_bytes(self.lights):
            return base
        return f"{base}:{self.plane_bytes}"

    def frame_offset(self, frame):
        return HEADER_LEN + frame * self.frame_bytes

    def audio_offset(self, frame):
        return self.frame_offset(frame) + self.video_bytes


def min_plane_bytes(lights):
    return (3 * lights + 7) // 8


def parse_geometry(text):
    """LIGHTSxLINESxDEPTH[:PLANEBYTES] -> Geometry (ValueError if malformed)."""
    m = re.fullmatch(r"(\d+)x(\d+)x(\d+)(?::(\d+))?", text.strip())
    if not m:
        raise ValueError(f"geometry {text!r} is not LIGHTSxLINESxDEPTH[:PLANEBYTES]")
    lights, lines, depth = (int(v) for v in m.group(1, 2, 3))
    if lights < 1 or lines < 1:
        raise ValueError(f"geometry {text!r}: lights and lines must be >= 1")
    if not 1 <= depth <= 8:
        raise ValueError(f"geometry {text!r}: depth must be 1..8")
    plane = int(m.group(4)) if m.group(4) else min_plane_bytes(lights)
    if plane < min_plane_bytes(lights):
        raise ValueError(f"geometry {text!r}: {lights} LEDs need "
                         f"{min_plane_bytes(lights)}-byte planes")
    return Geometry(lights, lines, depth, plane)


def frames_in(geom, size):
    """Whole frames in a file of ``size`` bytes."""
    return max(0, (size - HEADER_LEN) // geom.frame_bytes)


# ---------------------------------------------------------------- video codec

def encode_line(geom, leds):
    """One line from ``lights`` (r, g, b) levels, rim LED first."""
    if len(leds) != geom.lights:
        raise ValueError(f"line has {len(leds)} LEDs, geometry {geom.lights}")
    top = geom.max_level
    for led in leds:
        if len(led) != 3 or not all(0 <= v <= top for v in led):
            raise ValueError(f"LED level {led} outside 0..{top}")
    out = bytearray()
    for plane in range(geom.depth):
        shift = geom.depth - 1 - plane
        bits = 0
        for r, g, b in leds:
            bits = (bits << 3 | ((b >> shift) & 1) << 2
                    | ((g >> shift) & 1) << 1 | ((r >> shift) & 1))
        out += bits.to_bytes(geom.plane_bytes, "big")  # pad bits lead as zeros
    return bytes(out)


def decode_line(geom, data):
    """(r, g, b) levels of one encoded line, rim LED first."""
    leds = [[0, 0, 0] for _ in range(geom.lights)]
    pb = geom.plane_bytes
    for plane in range(geom.depth):
        bits = int.from_bytes(data[plane * pb:(plane + 1) * pb], "big")
        weight = 1 << (geom.depth - 1 - plane)
        for j in range(geom.lights):
            bgr = bits >> 3 * (geom.lights - 1 - j) & 7
            if bgr & 4:
                leds[j][2] += weight
            if bgr & 2:
                leds[j][1] += weight
            if bgr & 1:
                leds[j][0] += weight
    return [tuple(led) for led in leds]


def encode_video(geom, lines):
    """The video part of one frame from ``lines`` lines of LED levels."""
    if len(lines) != geom.lines:
        raise ValueError(f"{len(lines)} lines, geometry {geom.lines}")
    cache = {}
    out = []
    for leds in lines:
        key = tuple(leds)
        if key not in cache:
            cache[key] = encode_line(geom, leds)
        out.append(cache[key])
    return b"".join(out)


def decode_video(geom, data):
    lb = geom.line_bytes
    return [decode_line(geom, data[i * lb:(i + 1) * lb]) for i in range(geom.lines)]


def pad_bits_set(geom, data):
    """Planes (over all whole frames of a clip) with a pad bit set."""
    pad, pb = geom.pad_bits, geom.plane_bytes
    if pad == 0:
        return 0
    whole, part = divmod(pad, 8)
    mask = (0xFF << (8 - part)) & 0xFF
    table = bytes(b & mask for b in range(256))
    planes = geom.lines * geom.depth
    dirty = 0
    for f in range(frames_in(geom, len(data))):
        start = geom.frame_offset(f)
        video = bytes(data[start:start + geom.video_bytes])
        cols = [video[j::pb] for j in range(whole)]
        if part:
            cols.append(video[whole::pb].translate(table))
        flags = bytearray(planes)
        for col in cols:
            if any(col):
                for i, b in enumerate(col):
                    if b:
                        flags[i] = 1
        dirty += flags.count(1)
    return dirty


# -------------------------------------------------------------- test content

def _period(spec, kind):
    """N of a 'KIND:N...' spec (frames between two lit frames, >= 1)."""
    try:
        n = int(spec.split(":")[1])
    except (IndexError, ValueError):
        n = 0
    if n < 1:
        raise ValueError(f"{spec!r}: want {kind}:N with N >= 1 frames")
    return n


def flash_period(spec):
    """N of a flash:N video spec, None for a still pattern."""
    if spec.startswith("flash:"):
        if spec.count(":") != 1:
            raise ValueError(f"video {spec!r}: want flash:N")
        return _period(spec, "flash")
    return None


def video_pattern(geom, spec, frame=0):
    """LED levels per line of frame ``frame`` for black | rings | spokes |
    solid:R,G,B | flash:N (all LEDs white in frames 0, N, 2N, ..., else off)."""
    top = geom.max_level
    off, white = (0, 0, 0), (top, top, top)
    period = flash_period(spec)
    if period is not None:
        led = white if frame % period == 0 else off
        return [[led] * geom.lights for _ in range(geom.lines)]
    if spec == "black":
        return [[off] * geom.lights for _ in range(geom.lines)]
    if spec == "rings":
        line = [white if j in RING_LEDS else off for j in range(geom.lights)]
        return [list(line) for _ in range(geom.lines)]
    if spec == "spokes":
        lines = [[off] * geom.lights for _ in range(geom.lines)]
        short = [white] * max(1, geom.lights // 2) + [off] * (geom.lights - max(1, geom.lights // 2))
        for d in range(-SPOKE_HALF, SPOKE_HALF + 1):
            lines[(geom.lines // 4 + d) % geom.lines] = list(short)
        for d in range(-SPOKE_HALF, SPOKE_HALF + 1):
            lines[d % geom.lines] = [white] * geom.lights
        return lines
    if spec.startswith("solid:"):
        try:
            led = tuple(int(v) for v in spec[6:].split(","))
        except ValueError:
            led = ()
        if len(led) != 3 or not all(0 <= v <= top for v in led):
            raise ValueError(f"{spec!r}: want solid:R,G,B with levels 0..{top}")
        return [[led] * geom.lights for _ in range(geom.lines)]
    raise ValueError(f"video {spec!r}: want black, rings, spokes, solid:R,G,B or flash:N")


def _u8(v):
    return max(0, min(255, SILENCE + round(v)))


def tone_samples(hz, n, rate=SAMPLE_RATE):
    w = 2 * math.pi * hz / rate
    return bytes(_u8(AMPLITUDE * math.sin(w * t)) for t in range(n))


def ramped_tone(hz, length, rate=SAMPLE_RATE):
    """``length`` samples of tone with RAMP-sample raised-cosine edges (its
    first and last samples are silence)."""
    w = 2 * math.pi * hz / rate
    out = bytearray()
    for t in range(length):
        edge = min(t, length - 1 - t)
        env = 0.5 - 0.5 * math.cos(math.pi * edge / RAMP) if edge < RAMP else 1.0
        out.append(_u8(AMPLITUDE * env * math.sin(w * t)))
    return bytes(out)


def beep_samples(count, hz, n, rate=SAMPLE_RATE):
    """``count`` beeps (80 ms on / 80 ms off) at the start of every 1.6 s."""
    if not 1 <= count <= MAX_BEEPS:
        raise ValueError(f"beep count {count}: want 1..{MAX_BEEPS}")
    beep = ramped_tone(hz, BEEP_SAMPLES, rate)
    out = bytearray([SILENCE]) * n
    for cycle in range(0, n, CYCLE_SAMPLES):
        for k in range(count):
            s = cycle + 2 * k * BEEP_SAMPLES
            if s < n:
                out[s:s + BEEP_SAMPLES] = beep[:n - s]
    return bytes(out)


def burst_samples(period, hz, n, rate=SAMPLE_RATE):
    """A 50 ms tone burst filling the audio of frames 0, ``period``,
    2*``period``, ... (the frames flash:``period`` lights), silence elsewhere."""
    if period < 1:
        raise ValueError(f"burst period {period}: want N >= 1 frames")
    burst = ramped_tone(hz, BURST_SAMPLES, rate)
    out = bytearray([SILENCE]) * n
    for s in range(0, n, period * AUDIO_BYTES):
        out[s:s + BURST_SAMPLES] = burst[:n - s]
    return bytes(out)


def _hz(text):
    hz = float(text)
    if not 20 <= hz < SAMPLE_RATE / 2:
        raise ValueError(f"frequency {text}: want 20..{SAMPLE_RATE // 2} Hz")
    return hz


def audio_samples(spec, n):
    """``n`` audio bytes for silence | tone:HZ | beeps:COUNT[:HZ] | burst:N[:HZ]."""
    parts = spec.split(":")
    try:
        if parts == ["silence"]:
            return bytes([SILENCE]) * n
        if parts[0] == "tone" and len(parts) == 2:
            return tone_samples(_hz(parts[1]), n)
        if parts[0] == "beeps" and len(parts) in (2, 3):
            hz = _hz(parts[2]) if len(parts) == 3 else 1000.0
            return beep_samples(int(parts[1]), hz, n)
        if parts[0] == "burst" and len(parts) in (2, 3):
            hz = _hz(parts[2]) if len(parts) == 3 else BURST_HZ
            return burst_samples(_period(spec, "burst"), hz, n)
    except ValueError as e:
        raise ValueError(f"audio {spec!r}: {e}") from None
    raise ValueError(f"audio {spec!r}: want silence, tone:HZ, beeps:COUNT[:HZ] "
                     f"or burst:N[:HZ]")


def pad_to_window(data):
    return bytes(data) + bytes(-len(data) % WINDOW_BYTES)


def crc32_padded(data):
    """zlib CRC32 of ``data`` zero-padded to a multiple of 28,800 bytes."""
    return zlib.crc32(bytes(-len(data) % WINDOW_BYTES), zlib.crc32(data))


def build_clip(geom, frames, video="black", audio="silence"):
    """Header + ``frames`` frames, zero-padded to the upload window."""
    if frames < 1:
        raise ValueError("a clip needs at least one frame")
    period = flash_period(video)
    pictures = {}                    # one picture for a still pattern, two for flash:N

    def picture(f):
        key = period is not None and f % period == 0
        if key not in pictures:
            pictures[key] = encode_video(geom, video_pattern(geom, video, f))
        return pictures[key]

    picture(0)                       # a bad video spec fails before the audio
    sound = audio_samples(audio, frames * AUDIO_BYTES)
    out = bytearray(HEADER)
    for f in range(frames):
        out += picture(f)
        out += sound[f * AUDIO_BYTES:(f + 1) * AUDIO_BYTES]
    return pad_to_window(out)


def clip_audio(data, geom, frames=None):
    """The audio stream of a clip read with ``geom``'s frame stride."""
    if frames is None:
        frames = frames_in(geom, len(data))
    return b"".join(bytes(data[geom.audio_offset(f):geom.audio_offset(f) + AUDIO_BYTES])
                    for f in range(frames))


# ------------------------------------------------------------ audio analysis

BLOCK = 320                  # 10 ms
MERGE_BLOCKS = 2             # a gap of <= 20 ms does not end a beep
MIN_BEEP_BLOCKS = 3          # a beep has >= 30 ms at the pitch; less is a click
TONE_FRACTION = 0.5          # share of a block's energy at the dominant pitch

Beeps = collections.namedtuple("Beeps", "count freq_hz per_cycle")


def goertzel_power(x, hz, rate=SAMPLE_RATE):
    """|X(hz)|^2 of centred samples ``x``."""
    c = 2 * math.cos(2 * math.pi * hz / rate)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    return s1 * s1 + s2 * s2 - c * s1 * s2


def tone_fraction(x, hz):
    """About 1 for a pure tone at ``hz``, about 0 for anything else."""
    energy = sum(v * v for v in x)
    return 2 * goertzel_power(x, hz) / (len(x) * energy) if energy else 0.0


def _blocks_in(runs, size, limit):
    """Up to ``limit`` evenly spread blocks of ``size`` samples inside runs."""
    spans = [(s + i, s + i + size) for s, e in runs for i in range(0, e - s - size + 1, size)]
    if len(spans) <= limit:
        return spans
    return [spans[i * len(spans) // limit] for i in range(limit)]


def dominant_frequency(x, runs, lo=50, hi=4000):
    """Strongest frequency in the sample ranges ``runs`` (Goertzel scan)."""
    coarse = [x[s:e] for s, e in _blocks_in(runs, 512, 8)]
    if not coarse:
        coarse = [x[s:e] for s, e in runs]
    best = max(range(lo, hi + 1, 10),
               key=lambda f: sum(goertzel_power(b, f) for b in coarse))
    fine = [x[s:e] for s, e in _blocks_in(runs, 2048, 4)] or coarse
    return max(range(max(lo, best - 10), best + 11),
               key=lambda f: sum(goertzel_power(b, f) for b in fine))


def _runs(flags):
    """[start, end) block runs of true ``flags``, gaps <= MERGE_BLOCKS merged,
    kept when at least MIN_BEEP_BLOCKS of their blocks are true."""
    runs = []
    for i, on in enumerate(flags):
        if not on:
            continue
        if runs and i - runs[-1][1] <= MERGE_BLOCKS:
            runs[-1][1] = i + 1
            runs[-1][2] += 1
        else:
            runs.append([i, i + 1, 1])
    return [(s, e) for s, e, count in runs if count >= MIN_BEEP_BLOCKS]


def detect_beeps(audio, cycle=CYCLE_SAMPLES):
    """Beeps per cycle and their pitch in a u8 audio stream.

    The pitch is the dominant frequency of the loud 10 ms blocks. A beep is
    then a run of loud blocks whose energy is mostly at that pitch (gaps
    <= 20 ms merged) with at least 30 ms of such blocks. Blocks at another
    pitch count as gap: a beep that a probe overlap clipped and gave a
    foreign tail still counts once, and a foreign blip in the 80 ms between
    two beeps does not join them. ``count`` is the most common count over
    the complete cycles (the first, partial cycle when the stream is
    shorter than one).
    """
    x = [b - SILENCE for b in audio]
    cycles = max(1, -(-len(x) // cycle))
    rms = [math.sqrt(sum(v * v for v in x[i:i + BLOCK]) / BLOCK)
           for i in range(0, len(x) - BLOCK + 1, BLOCK)]
    if not rms or max(rms) < 2:
        return Beeps(0, None, [0] * cycles)
    threshold = max(2.0, 0.25 * max(rms))
    loud = [level >= threshold for level in rms]
    runs = [(s * BLOCK, e * BLOCK) for s, e in _runs(loud)]
    if not runs:
        return Beeps(0, None, [0] * cycles)
    freq = dominant_frequency(x, runs)
    tonal = [on and tone_fraction(x[i * BLOCK:(i + 1) * BLOCK], freq) >= TONE_FRACTION
             for i, on in enumerate(loud)]
    per_cycle = [0] * cycles
    for s, _ in _runs(tonal):
        per_cycle[s * BLOCK // cycle] += 1
    complete = per_cycle[:len(x) // cycle] or per_cycle[:1]
    count = collections.Counter(complete).most_common(1)[0][0]
    return Beeps(count, freq, per_cycle)


# --------------------------------------------------------------------- probe

Candidate = collections.namedtuple("Candidate", "number geom pitch")
CANDIDATES = tuple(Candidate(n, parse_geometry(g), hz) for n, g, hz in (
    (1, "48x300x5", 500),      # ATOM SE, F-MINI12      28,600 B
    (2, "48x300x6", 700),      # 48 LEDs at 6 bits      34,000 B
    (3, "64x520x6", 900),      # vendor app F-S12       76,480 B
    (4, "85x400x6", 1100),     # F-S12+ / ATOM PRO+     78,400 B (SW130)
    (5, "64x520x6:32", 1300),  # F-S12 in 32-byte planes 101,440 B
    (6, "116x520x6", 1500),    # app fallback, F1       138,880 B
))
# The strides this fan most likely plays: 85x400x6, the PRO+ format of SW130
# (expected), and 64x520x6, the F-S12 the MCU reports. "clip" gives a clip's
# length at both, since a clip that is short at the fan's stride may go
# unlisted (LISTED_SECONDS).
FAN_STRIDES = (parse_geometry("85x400x6"), parse_geometry("64x520x6"))
PROBE_WINDOWS = 120
# the probe must hold one frame of every candidate (5 windows)
PROBE_MIN_WINDOWS = max(-(-(HEADER_LEN + c.geom.frame_bytes) // WINDOW_BYTES)
                        for c in CANDIDATES)
# Where slots overlap, the later candidate wins. At 120 windows a beep meets
# another candidate's beep, or the quiet within 160 ms of its beeps, in 7
# places, so no order is free of damage: of all 720 orders the best keeps
# 62.9 % of its most damaged beep, and 1..6 is one of them (no beep split
# or dropped). Then 4, 5 and 6 hear no foreign sound, and 1 and 2 (3 to 4
# cycles each to compare) take most of it. The one clash of the pair that
# decides H1' is candidate 4's beep 3 in its second cycle against the 80 ms
# after candidate 3's third beep: candidate 3 hears 40 ms of 1,100 Hz 10 ms
# after its third beep in cycle 1 (a listener may count a fourth). Writing
# 3 after 4 would keep candidate 3 clean but leave that beep of candidate 4
# 30 ms plus a separate 10 ms (50 %). "probe" lists every foreign sound and
# cut per candidate.
PROBE_ORDER = (1, 2, 3, 4, 5, 6)

ProbeRow = collections.namedtuple(
    "ProbeRow", "number geom pitch frames seconds total shared intact worst_beep "
                "beeps freq_hz lit strays stray_from cuts")


def _worst_beep(want, got, count):
    """Smallest share of any one beep's samples that reads back unchanged."""
    worst = 1.0
    for cycle in range(0, len(want), CYCLE_SAMPLES):
        for k in range(count):
            s = cycle + 2 * k * BEEP_SAMPLES
            e = min(len(want), s + BEEP_SAMPLES)
            if s < e:
                same = sum(a == b for a, b in zip(want[s:e], got[s:e]))
                worst = min(worst, same / (e - s))
    return worst


def _diff_runs(want, got):
    """[start, end) sample runs, in 1 ms steps, where ``got`` differs from
    ``want``: (runs where ``want`` is silent, runs inside its beeps)."""
    quiet = bytes([SILENCE]) * MS
    runs = ([], [])
    for s in range(0, len(want), MS):
        w = want[s:s + MS]
        if w == got[s:s + MS]:
            continue
        kind = runs[0] if w == quiet[:len(w)] else runs[1]
        e = min(len(want), s + MS)
        if kind and kind[-1][1] == s:
            kind[-1][1] = e
        else:
            kind.append([s, e])
    return runs


def _ms_spans(runs):
    """Sample runs as (cycle, start ms, end ms) from the start of that cycle."""
    return [(s // CYCLE_SAMPLES, s % CYCLE_SAMPLES // MS,
             (s % CYCLE_SAMPLES + e - s) // MS) for s, e in runs]


def foreign_spans(want, got):
    """Where ``got`` differs from ``want``, in 1 ms steps: (strays, cuts).

    strays: sound where ``want`` is silent (another candidate's audio);
    cuts: beep samples that read back changed. Each is a list of
    (cycle, start ms, end ms) with times from the start of that 1.6 s cycle.
    """
    return tuple(_ms_spans(runs) for runs in _diff_runs(want, got))


def build_probe(windows=PROBE_WINDOWS, order=PROBE_ORDER):
    """(file bytes, one ProbeRow per candidate) for the stride probe.

    Per row, ``stray_from`` gives for each stray the candidates whose audio
    it is, and ``lit`` the share of non-zero bytes in the video that the
    candidate's stride reads (other candidates' audio slots).
    """
    if sorted(order) != [c.number for c in CANDIDATES]:
        raise ValueError(f"order {order}: want a permutation of 1..{len(CANDIDATES)}")
    if windows < PROBE_MIN_WINDOWS:
        raise ValueError(f"{windows} windows: the probe needs at least "
                         f"{PROBE_MIN_WINDOWS} windows (one frame of every candidate)")
    size = windows * WINDOW_BYTES
    buf = bytearray(size)
    buf[:HEADER_LEN] = HEADER
    cover = bytearray(size)
    owner = bytearray(size)          # the candidate whose audio each byte holds
    plus_one = bytes(min(255, b + 1) for b in range(256))
    by_number = {c.number: c for c in CANDIDATES}
    streams = {}
    for c in CANDIDATES:
        n = frames_in(c.geom, size)
        streams[c.number] = (n, beep_samples(c.number, c.pitch, n * AUDIO_BYTES))
        for f in range(n):
            o = c.geom.audio_offset(f)
            cover[o:o + AUDIO_BYTES] = cover[o:o + AUDIO_BYTES].translate(plus_one)
    for number in order:
        geom = by_number[number].geom
        n, audio = streams[number]
        mark = bytes([number]) * AUDIO_BYTES
        for f in range(n):
            o = geom.audio_offset(f)
            buf[o:o + AUDIO_BYTES] = audio[f * AUDIO_BYTES:(f + 1) * AUDIO_BYTES]
            owner[o:o + AUDIO_BYTES] = mark
    rows = []
    for c in CANDIDATES:
        g = c.geom
        n, audio = streams[c.number]
        got = clip_audio(buf, g, n)
        intact = sum(a == b for a, b in zip(got, audio)) / len(audio)
        alone = sum(cover[g.audio_offset(f):g.audio_offset(f) + AUDIO_BYTES].count(1)
                    for f in range(n))
        beeps = detect_beeps(got)
        stray_runs, cut_runs = _diff_runs(audio, got)
        stray_from = [tuple(sorted({owner[g.audio_offset(t // AUDIO_BYTES) + t % AUDIO_BYTES]
                                    for t in range(s, e) if got[t] != audio[t]}))
                      for s, e in stray_runs]
        lit = sum(g.video_bytes - buf[g.frame_offset(f):g.frame_offset(f) + g.video_bytes].count(0)
                  for f in range(n)) / (n * g.video_bytes)
        rows.append(ProbeRow(
            number=c.number, geom=g, pitch=c.pitch, frames=n, seconds=n / FPS,
            total=n // FPS, shared=1 - alone / len(audio), intact=intact,
            worst_beep=_worst_beep(audio, got, c.number), beeps=beeps.count,
            freq_hz=beeps.freq_hz, lit=lit, strays=_ms_spans(stray_runs),
            stray_from=stray_from, cuts=_ms_spans(cut_runs)))
    return bytes(buf), rows


# ------------------------------------------------------------------- outputs

def png_bytes(width, height, rows):
    """A minimal 8-bit RGB PNG from ``height`` rows of 3*``width`` bytes."""
    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body)))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def render_png(geom, video, scale=None):
    """One frame's video as a disc: rim outside, line 0 at 12 o'clock,
    lines increasing clockwise, one LED = ``scale`` pixels of radius."""
    top = geom.max_level
    rgb = [bytes(v * 255 // top for led in line for v in led)
           for line in decode_video(geom, video)]
    s = scale or max(1, 300 // geom.lights)
    size = 2 * geom.lights * s
    half = size / 2
    rows = []
    for y in range(size):
        dy = y + 0.5 - half
        row = bytearray()
        for x in range(size):
            dx = x + 0.5 - half
            ring = int(math.hypot(dx, dy) / s)
            if ring >= geom.lights:
                row += BACKGROUND
                continue
            led = geom.lights - 1 - ring
            angle = math.atan2(dx, -dy) % (2 * math.pi)
            line = int(angle / (2 * math.pi) * geom.lines + 0.5) % geom.lines
            row += rgb[line][3 * led:3 * led + 3]
        rows.append(row)
    return png_bytes(size, size, rows)


def write_wav(path, audio):
    """u8 mono PCM at 32 kHz (8-bit WAV samples are unsigned, as on the fan)."""
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(1)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(audio)


# ------------------------------------------------------------------- inspect

Report = collections.namedtuple(
    "Report", "size frames seconds tail tail_zero pad_planes audio median_step "
              "smooth beeps crc")


def _step(a):
    return sum(abs(a[i] - a[i - 1]) for i in range(1, len(a))) / (len(a) - 1)


def inspect_clip(data, geom, sampled=8):
    """Measure ``data`` as a clip of geometry ``geom``."""
    n = frames_in(geom, len(data))
    tail = bytes(data[geom.frame_offset(n):]) if len(data) > HEADER_LEN else b""
    steps, audio = [], []
    picks = sorted({round(i * (n - 1) / max(1, min(n, sampled) - 1))
                    for i in range(min(n, sampled))}) if n else []
    for f in range(n):
        a = data[geom.audio_offset(f):geom.audio_offset(f) + AUDIO_BYTES]
        steps.append(_step(a))
        if f in picks:
            audio.append((f, sum(a) / len(a), steps[-1]))
    median = sorted(steps)[len(steps) // 2] if steps else 0.0
    return Report(len(data), n, n / FPS, len(tail), not any(tail),
                  pad_bits_set(geom, data), audio, median,
                  sum(s < 6 for s in steps), detect_beeps(clip_audio(data, geom, n)),
                  crc32_padded(data))


# ----------------------------------------------------------------------- CLI

def _windows(size):
    return -(-size // WINDOW_BYTES)


def _cmd_clip(args):
    geom = parse_geometry(args.geom)
    frames = args.frames if args.frames is not None else round(args.seconds * FPS)
    data = build_clip(geom, frames, args.video, args.audio)
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"wrote {args.out}")
    print(f"  geometry {geom.name}: frame {geom.frame_bytes:,} B (video "
          f"{geom.video_bytes:,} + audio {AUDIO_BYTES:,}), pad bits {geom.pad_bits}")
    print(f"  video {args.video}, audio {args.audio}")
    period = flash_period(args.video)
    if period is not None:
        print(f"  flash frames 0, {period}, {2 * period}, ...: every {period / FPS:.2f} s, "
              f"{-(-frames // period)} in the clip")
    print(f"  frames {frames}, seconds {frames / FPS:.2f}, size {len(data):,} B "
          f"= {len(data) // WINDOW_BYTES} windows")
    print(f"  crc32 {crc32_padded(data):08x}")
    print("  read with the fan's likely strides (PRO+ expected, F-S12 reported):")
    shortest = frames / FPS
    for g in FAN_STRIDES:
        n = frames_in(g, len(data))
        shortest = min(shortest, n / FPS)
        print(f"    {g.name} ({g.frame_bytes:,} B frames): frames {n}, seconds {n / FPS:.2f}")
    if shortest < LISTED_SECONDS:
        print(f"  note: {shortest:.2f} s at the shortest, under {LISTED_SECONDS} s: the "
              f"MCU may leave it unlisted, and an unlisted clip breaks every later "
              f"upload until a format")
    return 0


def _cmd_probe(args):
    order = tuple(int(v) for v in args.order.split(","))
    data, rows = build_probe(args.windows, order)
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"wrote {args.out}: {len(data):,} B = {args.windows} windows, "
          f"crc32 {crc32_padded(data):08x}")
    print(f"write order {','.join(map(str, order))} (where audio slots overlap "
          f"the later candidate wins); beeps 80 ms on/off, cycle 1.6 s")
    print("  cand  geometry      frame B  pitch  frames  seconds  total  "
          "shared  intact  worst beep  decoded")
    ok = True
    for r in rows:
        good = (r.beeps == r.number and r.freq_hz is not None
                and abs(r.freq_hz - r.pitch) <= 5)
        ok &= good
        freq = f"{r.freq_hz} Hz" if r.freq_hz is not None else "no tone"
        note = "" if good else "  MISMATCH"
        if r.seconds < LISTED_SECONDS:
            note += f"  (under {LISTED_SECONDS} s, may not be listed)"
        print(f"  {r.number:4}  {r.geom.name:12} {r.geom.frame_bytes:8,}  {r.pitch:5}  "
              f"{r.frames:6}  {r.seconds:7.2f}  {r.total:5}  {r.shared:6.1%}  "
              f"{r.intact:6.1%}  {r.worst_beep:10.1%}  "
              f"{r.beeps} beep{'s' * (r.beeps != 1)} @ {freq}{note}")
    print("  total = PlayInfo total under the floor rule (frames // 20)")
    print("  shared = slot bytes inside another candidate's slots; intact = slot "
          "bytes that read back unchanged with the candidate's stride; worst beep "
          "= the same for its most damaged beep")
    print("  what each stride plays besides its own beeps (cycle, ms from the "
          "cycle start, whose audio it is); cut = beep samples another candidate "
          "overwrote:")
    pitch = {c.number: c.pitch for c in CANDIDATES}
    for r in rows:
        print(f"  {r.number}: foreign sound {_spans_text(r.strays, r.stray_from, pitch)}; "
              f"cut {_spans_text(r.cuts)}")
    print("  video: no byte outside the audio slots is set, but each stride reads the "
          "other candidates' audio slots as LED data (non-zero video bytes: "
          + ", ".join(f"{r.number} {r.lit:.1%}" for r in rows) + "),")
    print("    so the fan shows coloured wedges, striped or speckled like snow, "
          "whatever the stride: expected, not corruption; judge the probe by ear")
    return 0 if ok else 1


def _spans_text(spans, sources=(), pitch=None):
    """'c0 100-133 ms (5: 1300 Hz), ...' (sources: candidate numbers per span)."""
    out = []
    for i, (c, s, e) in enumerate(spans):
        whose = ", ".join(f"{n}: {pitch[n]} Hz" for n in sources[i]) if sources else ""
        out.append(f"c{c} {s}-{e} ms" + (f" ({whose})" if whose else ""))
    return ", ".join(out) or "none"


def _cmd_inspect(args):
    geom = parse_geometry(args.geom)
    with open(args.path, "rb") as f:
        data = f.read()
    r = inspect_clip(data, geom)
    print(f"{args.path}: size {r.size:,} B, geometry {geom.name} "
          f"(frame {geom.frame_bytes:,} B)")
    print(f"  frames {r.frames}, seconds {r.seconds:.2f}, PlayInfo total under "
          f"the floor rule {r.frames // FPS}")
    print(f"  after the last frame {r.tail:,} B ({'all zero' if r.tail_zero else 'NOT zero'}); "
          f"size {'is' if r.size % WINDOW_BYTES == 0 else 'is NOT'} a multiple of "
          f"{WINDOW_BYTES:,} ({_windows(r.size)} windows), crc32 padded {r.crc:08x}")
    header = "matches" if data[:HEADER_LEN] == HEADER else "DIFFERS from"
    print(f"  header {header} the vendor constant")
    if geom.pad_bits == 0:
        print("  pad bits: none in this geometry")
    else:
        print(f"  pad bits ({geom.pad_bits} per plane): {r.pad_planes} of "
              f"{r.frames * geom.lines * geom.depth:,} planes have one set")
    for f, mean, step in r.audio:
        print(f"  audio frame {f:4}: mean {mean:6.1f}  mean|step| {step:6.2f}")
    print(f"  audio over all frames: median mean|step| {r.median_step:.2f}, "
          f"smooth (mean|step| < 6) {r.smooth}/{r.frames}")
    freq = f"{r.beeps.freq_hz} Hz" if r.beeps.freq_hz is not None else "none"
    print(f"  beeps {r.beeps.count} per 1.6 s cycle (per cycle {r.beeps.per_cycle}), "
          f"dominant {freq}")
    if args.wav:
        write_wav(args.wav, clip_audio(data, geom, r.frames))
        print(f"  wrote {args.wav}: {r.frames * AUDIO_BYTES:,} samples, 32 kHz u8 mono")
    if args.png:
        n = int(args.png[0])
        if not 0 <= n < r.frames:
            print(f"  --png: frame {n} not in 0..{r.frames - 1}")
            return 1
        start = geom.frame_offset(n)
        with open(args.png[1], "wb") as f:
            f.write(render_png(geom, data[start:start + geom.video_bytes]))
        print(f"  wrote {args.png[1]}: frame {n}, B,G,R order, line 0 at 12 o'clock")
    return 0


def _cmd_crc(args):
    with open(args.path, "rb") as f:
        data = f.read()
    print(f"{crc32_padded(data):08x}  {len(data):,} B padded to "
          f"{_windows(len(data)) * WINDOW_BYTES:,} B ({_windows(len(data))} windows)  "
          f"{args.path}")
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("clip", help="build a test clip")
    c.add_argument("--geom", required=True, help="LIGHTSxLINESxDEPTH[:PLANEBYTES]")
    length = c.add_mutually_exclusive_group(required=True)
    length.add_argument("--seconds", type=float)
    length.add_argument("--frames", type=int)
    c.add_argument("--video", default="black",
                   help="black | rings | spokes | solid:R,G,B | flash:N (all LEDs white "
                        "in frames 0, N, 2N, ...)")
    c.add_argument("--audio", default="silence",
                   help="silence | tone:HZ | beeps:COUNT[:HZ] | burst:N[:HZ] (50 ms tone, "
                        "default 2000 Hz, in the audio of frames 0, N, 2N, ...)")
    c.add_argument("out")
    c.set_defaults(run=_cmd_clip)
    pr = sub.add_parser("probe", help="build the frame-stride probe")
    pr.add_argument("--windows", type=int, default=PROBE_WINDOWS,
                    help=f"size in {WINDOW_BYTES:,}-byte windows "
                         f"(at least {PROBE_MIN_WINDOWS})")
    pr.add_argument("--order", default=",".join(map(str, PROBE_ORDER)),
                    help="write order of the candidates, e.g. 1,2,3,4,5,6")
    pr.add_argument("out")
    pr.set_defaults(run=_cmd_probe)
    i = sub.add_parser("inspect", help="measure a clip with a given geometry")
    i.add_argument("--geom", required=True)
    i.add_argument("--wav", metavar="OUT")
    i.add_argument("--png", nargs=2, metavar=("N", "OUT"))
    i.add_argument("path")
    i.set_defaults(run=_cmd_inspect)
    k = sub.add_parser("crc", help="CRC32 of a file padded to 28,800 bytes")
    k.add_argument("path")
    k.set_defaults(run=_cmd_crc)
    args = p.parse_args(argv)
    try:
        return args.run(args)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
