#!/usr/bin/env python3
"""Find the silences in a phone recording of the HoloFan (a phone's voice
memo .m4a, or anything ffmpeg or afconvert can read).

Usage: silences.py RECORDING [--start HH:MM:SS(.s) | --sync HH:MM:SS(.s)@OFFSET]
                   [--min 1.5] [--rel-db 25 | --abs-db D] [--window 50] [--hz F] [--levels]

The recording is decoded to mono 16 kHz WAV in a temp dir (ffmpeg, else
afconvert) and cut into --window ms windows; each window's RMS level is taken
in dBFS (with --hz F only the band F/1.25..F*1.25, e.g. 2000 for the beeps or
440 for the rings, which ignores the rotor's broadband whoosh). A silence is a
run of windows under the threshold (--rel-db under the 90th-percentile window
level, or --abs-db) lasting at least --min seconds.

Why --min 1.5: 04-Beeps is clipgen 'beeps:10:2000', 80 ms beeps 80 ms apart,
10 per 1.6 s (tools/holofan_clip/clipgen.py), so its own gaps are 80 ms (a
loop restart adds a little); the silences hunted last ~10-12 s. A clip with
fewer beeps per 1.6 s has gaps of up to 1.52 s (one beep): use --min 2 there.

Times are recording time (from the first sample). With --start (the wall
clock of the first sample) or --sync WALL@OFFSET (recording time OFFSET, in s
or M:SS.s, was wall clock WALL; e.g. the beeps stop at the meshlog's
'[mcu] mculog:pause.' line after a #HLFP) they are also given in wall clock,
to set against the meshlog (hfbench.py summary). --levels prints the median
and 90th-percentile level per 5 s (volume 2 vs 3 steps, rings vs beeps).
"""
import argparse
import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import warnings

import numpy as np
from scipy.io import wavfile

RATE = 16000
DEFAULT_MIN_S = 1.5
BLOCK_S = 5.0
FLOOR_DB = -120.0


# ------------------------------------------------------------------ decode

def tool(name):
    """Path of a command-line tool (Homebrew's ffmpeg may not be on PATH)."""
    for p in (shutil.which(name), f"/opt/homebrew/bin/{name}", f"/usr/local/bin/{name}", f"/usr/bin/{name}"):
        if p and os.path.exists(p):
            return p
    return None


def decode(path, tmp):
    """Mono 16 kHz samples (float, -1..1) of any audio file, via ffmpeg or afconvert."""
    out = os.path.join(tmp, "mono16k.wav")
    tried = []
    ff = tool("ffmpeg")
    if ff:
        r = subprocess.run([ff, "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", path,
                            "-vn", "-ac", "1", "-ar", str(RATE), "-c:a", "pcm_s16le", out],
                           capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(out):
            return read_wav(out)
        tried.append(f"ffmpeg: {r.stderr.strip()[:200]}")
    af = tool("afconvert")
    if af:
        r = subprocess.run([af, "-f", "WAVE", "-d", f"LEI16@{RATE}", "-c", "1", path, out],
                           capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(out):
            return read_wav(out)
        tried.append(f"afconvert: {(r.stderr or r.stdout).strip()[:200]}")
    raise RuntimeError(f"cannot decode {path}: " + ("; ".join(tried) or "neither ffmpeg nor afconvert found"))


def read_wav(path):
    """Samples of a PCM WAV as mono float -1..1, and its rate. scipy reads the
    WAVE_FORMAT_EXTENSIBLE files afconvert writes (Python 3.11's wave cannot)."""
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")      # afconvert adds chunks scipy skips with a warning
        rate, x = wavfile.read(path)
    if x.dtype == np.uint8:
        x = (x.astype(np.float64) - 128) / 128
    elif x.dtype.kind == "i":
        x = x.astype(np.float64) / float(np.iinfo(x.dtype).max + 1)
    else:
        x = x.astype(np.float64)
    if x.ndim == 2:
        x = x.mean(axis=1)
    return x, rate


def creation_time(path):
    """The file's creation_time tag (UTC) as a local datetime, or None."""
    fp = tool("ffprobe")
    if not fp:
        return None
    r = subprocess.run([fp, "-v", "error", "-show_entries", "format_tags=creation_time", "-of", "json", path],
                       capture_output=True, text=True)
    try:
        tag = json.loads(r.stdout)["format"]["tags"]["creation_time"]
        return datetime.datetime.fromisoformat(tag).astimezone()
    except (ValueError, KeyError, TypeError):
        return None


# ---------------------------------------------------------------- analysis

def window_levels(x, rate, window_ms=50.0, hz=None):
    """Level of each whole window in dBFS (a full-scale sine is -3 dB), and the
    window length in s. With hz: only the band hz/1.25..hz*1.25 (Parseval over
    an rfft, so an in-band sine reads the same as without hz)."""
    n = max(1, int(round(rate * window_ms / 1000)))
    k = len(x) // n
    w = np.asarray(x[:k * n], dtype=np.float64).reshape(k, n)
    if hz is None:
        p = np.mean(w * w, axis=1)
    else:
        f = np.fft.rfftfreq(n, 1.0 / rate)
        band = (f >= hz / 1.25) & (f <= hz * 1.25) & (f > 0) & (f < rate / 2)
        spec = np.fft.rfft(w, axis=1)[:, band]
        p = 2 * np.sum(np.abs(spec) ** 2, axis=1) / (n * n)
    return 10 * np.log10(np.maximum(p, 10 ** (FLOOR_DB / 10))), n / rate


def threshold(levels, rel_db=25.0, abs_db=None):
    """(threshold, reference): abs_db, or rel_db under the 90th percentile."""
    if abs_db is not None:
        return float(abs_db), None
    ref = float(np.percentile(levels, 90)) if len(levels) else FLOOR_DB
    return ref - rel_db, ref


def find_silences(levels, step, thr, min_s=DEFAULT_MIN_S):
    """[(start_s, end_s)] of the runs of windows under thr lasting >= min_s."""
    out, i, n = [], 0, len(levels)
    quiet = np.asarray(levels) < thr
    while i < n:
        if not quiet[i]:
            i += 1
            continue
        j = i
        while j < n and quiet[j]:
            j += 1
        if (j - i) * step >= min_s - 1e-9:
            out.append((i * step, j * step))
        i = j
    return out


def block_levels(levels, step, block_s=BLOCK_S):
    """[(start_s, median dB, p90 dB)] per block_s of windows."""
    per = max(1, int(round(block_s / step)))
    out = []
    for i in range(0, len(levels), per):
        b = levels[i:i + per]
        out.append((i * step, float(np.median(b)), float(np.percentile(b, 90))))
    return out


# ------------------------------------------------------------------- times

def parse_clock(text):
    """'HH:MM', 'HH:MM:SS' or 'HH:MM:SS.s' -> seconds since midnight."""
    m = re.fullmatch(r"(\d{1,2}):(\d\d)(?::(\d\d(?:\.\d+)?))?", text.strip())
    if not m or int(m[1]) > 23 or int(m[2]) > 59:
        raise ValueError(f"time {text!r}: want HH:MM[:SS[.s]]")
    return int(m[1]) * 3600 + int(m[2]) * 60 + float(m[3] or 0)


def parse_offset(text):
    """Recording time '84.7', '1:24.7' or '0:01:24.7' -> seconds."""
    parts = text.strip().split(":")
    if not 1 <= len(parts) <= 3 or not all(re.fullmatch(r"\d+(?:\.\d+)?", p) for p in parts):
        raise ValueError(f"offset {text!r}: want S, M:SS or H:MM:SS (decimals allowed)")
    s = 0.0
    for p in parts:
        s = s * 60 + float(p)
    return s


def sync_start(spec):
    """'WALL@OFFSET' -> the wall clock (s since midnight) of the first sample."""
    wall, sep, off = spec.partition("@")
    if not sep:
        raise ValueError(f"--sync {spec!r}: want HH:MM:SS(.s)@OFFSET")
    return (parse_clock(wall) - parse_offset(off)) % 86400


def fmt_clock(t, places=1):
    t = round(t % 86400, places) % 86400
    h, rest = divmod(t, 3600)
    m, s = divmod(rest, 60)
    return f"{int(h):02d}:{int(m):02d}:{s:0{3 + places}.{places}f}"


def fmt_rec(t):
    m, s = divmod(round(t, 2), 60)
    return f"{int(m)}:{s:05.2f}"


# -------------------------------------------------------------------- main

def report(x, rate, args, start=None, name="recording"):
    """The printed lines for samples x."""
    levels, step = window_levels(x, rate, args.window, args.hz)
    thr, ref = threshold(levels, args.rel_db, args.abs_db)
    dur = len(x) / rate
    band = f", band {args.hz / 1.25:.0f}-{args.hz * 1.25:.0f} Hz" if args.hz else ""
    how = f"reference (p90) {ref:.1f} dBFS, threshold {thr:.1f} dBFS ({args.rel_db:g} dB under)" if ref is not None \
        else f"threshold {thr:.1f} dBFS (--abs-db)"
    out = [f"{name}: {dur:.1f} s, window {step * 1000:.0f} ms{band}, {how}, min {args.min:g} s"]
    if start is not None:
        out.append(f"first sample at {fmt_clock(start)} (wall clock)")
    found = find_silences(levels, step, thr, args.min)
    out.append(f"silences: {len(found)}")
    if found:
        out.append("   #   rec start    rec end    length" + ("   wall start   wall end" if start is not None else ""))
    for i, (a, b) in enumerate(found, 1):
        line = f"  {i:2d}  {fmt_rec(a):>9}  {fmt_rec(b):>9}  {b - a:6.2f} s"
        if start is not None:
            line += f"   {fmt_clock(start + a)}   {fmt_clock(start + b)}"
        if a <= 0:
            line += "   (from the start)"
        if b >= len(levels) * step - 1e-9:
            line += "   (to the end)"
        out.append(line)
    if args.levels:
        blocks = block_levels(levels, step)
        top = max((m for _, m, _ in blocks), default=0.0)
        out.append(f"levels per {BLOCK_S:g} s (dBFS):")
        out.append("     rec" + ("        wall" if start is not None else "") + "    median    p90")
        for t, med, p90 in blocks:
            bar = "#" * max(0, int(round((med - (top - 40)) / 2)))
            wall = f"  {fmt_clock(start + t)}" if start is not None else ""
            out.append(f"  {fmt_rec(t):>7}{wall}  {med:7.1f}  {p90:6.1f}  {bar}")
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("recording")
    p.add_argument("--start", help="wall clock of the first sample, HH:MM:SS(.s)")
    p.add_argument("--sync", help="WALL@OFFSET: recording time OFFSET was wall clock WALL")
    p.add_argument("--min", type=float, default=DEFAULT_MIN_S, help="shortest silence, s (default 1.5)")
    p.add_argument("--rel-db", type=float, default=25.0, help="threshold under the p90 level, dB (default 25)")
    p.add_argument("--abs-db", type=float, help="absolute threshold, dBFS (overrides --rel-db)")
    p.add_argument("--window", type=float, default=50.0, help="window, ms (default 50)")
    p.add_argument("--hz", type=float, help="measure only the band around this frequency (2000 beeps, 440 rings)")
    p.add_argument("--levels", action="store_true", help="median and p90 level per 5 s")
    args = p.parse_args(argv)
    try:
        if args.start and args.sync:
            raise ValueError("give --start or --sync, not both")
        start = parse_clock(args.start) if args.start else sync_start(args.sync) if args.sync else None
        if not os.path.exists(args.recording):
            raise ValueError(f"no such file: {args.recording}")
        with tempfile.TemporaryDirectory() as tmp:
            x, rate = decode(args.recording, tmp)
    except (ValueError, RuntimeError) as e:
        print(f"error: {e}")
        return 1
    print("\n".join(report(x, rate, args, start, os.path.basename(args.recording))))
    if start is None:
        ct = creation_time(args.recording)
        if ct:
            tod = ct.hour * 3600 + ct.minute * 60 + ct.second + ct.microsecond / 1e6
            print(f"file creation_time {ct:%Y-%m-%d %H:%M:%S} local (unverified: the start or the end?): "
                  f"--start {fmt_clock(tod)} if the start, --start {fmt_clock(tod - len(x) / rate)} if the end; "
                  f"--sync from a known event is safer")
    return 0


if __name__ == "__main__":
    sys.exit(main())
