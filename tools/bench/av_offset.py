#!/usr/bin/env python3
"""Audio/video offset of the HoloFan from a phone video of the sync clip.

Usage: av_offset.py VIDEO [--flash-period 1.6] [--hz 2000] [--rel-db 6] [--min-pairs 5]
                    [--calibrate VIDEO2 [--cal-clap]]

The sync clip (clipgen 'flash:32' video, 'burst:32:2000' audio; tools/README.md)
lights every LED white for one 50 ms frame every 1.6 s, and its audio holds a
50 ms 2 kHz burst in exactly those frames. Film the fan playing it with the
phone (mov or mp4, 60 fps, dim room, the fan filling the frame, phone about
1 m from the fan's speaker, which adds ~3 ms of sound travel per metre; 30 s
or more). This prints the delay of each burst after its flash and the
median: about 0 for a wired audio path, about 100-250 ms if the audio goes
over Bluetooth A2DP. Needs ffmpeg (brew install ffmpeg), numpy and scipy.

How it measures (ffmpeg decodes; numpy/scipy analyse):
  audio  mono 16 kHz, timed from the start of the file (a late audio track is
         padded); the band hz/1.25..hz*1.25 (zero-phase filter); level of 5 ms
         windows every 1 ms. A burst is the loudest window of its period, at
         least 12 dB over the band's median; its onset is the centre of the
         first 5 ms window (of 3 in a row) within --rel-db of that loudest one.
  video  each frame scaled to 32x32 grey, mean brightness minus a running
         median (the baseline), times from the frame pts. The rotor only shows
         a flash during part of each rotation, so a flash's frames vary in
         brightness: each flash is the brightness peak of its period, and its
         onset is where the brightness first rose half way from the baseline
         to that peak (interpolated between frames).
  pairs  each flash with the nearest burst within +-period/2 (one to one).
         Offsets beyond half a period cannot be told from the next flash.
A 30 fps video has ~33 ms resolution (a flash falls anywhere between two
frames); 60 fps (~17 ms) is recommended, and the median over many flashes
narrows it further.

--calibrate VIDEO2 measures the phone's own A/V offset (its audio and video
pipelines) and subtracts it: film a phone-screen flash with a beep (a
clapperboard / sync app on a second phone held near the camera), 5 or more,
at least 1 s apart, measured the same way (the onset of a loud broadband
sound, high-passed at 200 Hz, against the onset of the brightness change).
With --cal-clap VIDEO2 is hand claps instead (5 or more, 1 s apart, hands
filling the frame): the visual time is the frame with the largest change
from the frame before (the hands' last fast motion), good to about a frame.

Exit status: 0 measured, 1 the video cannot be read, 2 fewer than
--min-pairs flash/burst pairs (in VIDEO or VIDEO2).
"""
import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from scipy import ndimage, signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import silences  # noqa: E402  (tool lookup and WAV reading)

RATE = 16000              # decoded audio, Hz
SIZE = 32                 # frames are scaled to SIZE x SIZE grey
PERIOD_S = 1.6            # clipgen flash:32 at 20 fps
HZ = 2000.0               # clipgen burst default
WIN_S = 0.005             # audio level window
HOP_S = 0.001             # and its step
RUN = 3                   # an onset needs this many windows in a row over the threshold
SEARCH_S = 0.1            # an onset is at most this long before the burst's loudest window
MIN_SNR_DB = 12.0         # a burst's loudest window over the band's median level
PEAK_SPREAD_DB = 20.0     # bursts more than this under the loud ones are not counted
HIGHPASS_HZ = 200.0       # calibration audio: broadband, without rumble
VIDEO_SNR = 8.0           # a flash over the robust noise of the brightness
VIDEO_MIN = 1.0           # and at least this many grey levels over the baseline
VIDEO_REL = 0.25          # and at least this share of the typical flash
CLAP_REL = 0.5            # claps: other motion (the hands parting) is slower
FRACTION = 0.5            # onset: half way from the baseline to the flash's peak
CAL_SPACING_S = 0.8       # calibration events are at least 1 s apart
FAST_FPS = 50             # under this, warn about the frame resolution

Analysis = collections.namedtuple(
    "Analysis", "name seconds fps audio_s video_s pairs snr_db timing")


# ------------------------------------------------------------------ decode

def ffmpeg():
    path = silences.tool("ffmpeg")
    if not path:
        raise RuntimeError("ffmpeg not found (brew install ffmpeg)")
    return path


def extract_audio(path, tmp):
    """Mono RATE samples (float -1..1) of the first audio stream; sample 0 is
    the start of the file (aresample first_pts=0 pads a late track and trims
    encoder delay, so audio and frame times share one clock)."""
    out = os.path.join(tmp, "audio.wav")
    r = subprocess.run([ffmpeg(), "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                        "-i", path, "-map", "0:a:0", "-vn",
                        "-af", f"aresample={RATE}:async=1:first_pts=0", "-ac", "1",
                        "-c:a", "pcm_s16le", out], capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        raise RuntimeError(f"cannot read the audio of {path}: {r.stderr.strip()[:300]}")
    x, rate = silences.read_wav(out)
    if rate != RATE:
        raise RuntimeError(f"ffmpeg gave {rate} Hz audio, expected {RATE}")
    return x


SHOWINFO = re.compile(r"\bn:\s*(\d+)\s+pts:\s*(\S+)\s+pts_time:")
TIMEBASE = re.compile(r"config in time_base:\s*(\d+)/(\d+),\s*frame_rate:\s*(\d+)/(\d+)")


def frame_times(log, count):
    """Frame times in s from ffmpeg's showinfo lines (pts x time base), or
    from the frame rate when a pts is missing; and a note when it fell back."""
    base = TIMEBASE.search(log)
    rows = SHOWINFO.findall(log)
    if base and len(rows) == count and all(re.fullmatch(r"-?\d+", p) for _, p in rows):
        num, den = int(base[1]), int(base[2])
        return np.array([int(p) * num / den for _, p in rows]), None
    if base and int(base[3]) > 0:
        fps = int(base[3]) / int(base[4])
        return np.arange(count) / fps, f"frame times from the frame rate ({fps:.3f} fps), not the pts"
    raise RuntimeError("no frame timestamps in ffmpeg's showinfo output")


def extract_video(path, tmp):
    """(frame times in s, frames as an (n, SIZE, SIZE) float array, note)."""
    out = os.path.join(tmp, "video.gray")
    r = subprocess.run([ffmpeg(), "-nostdin", "-hide_banner", "-loglevel", "info", "-y",
                        "-i", path, "-map", "0:v:0", "-an", "-sn", "-dn",
                        "-vf", f"scale=w={SIZE}:h={SIZE}:flags=area,format=gray,showinfo",
                        "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "gray", out],
                       capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        tail = "\n".join(r.stderr.strip().splitlines()[-3:])
        raise RuntimeError(f"cannot read the video of {path}: {tail[:300]}")
    raw = np.fromfile(out, dtype=np.uint8)
    count = len(raw) // (SIZE * SIZE)
    if count < 3:
        raise RuntimeError(f"{path}: only {count} video frames")
    frames = raw[:count * SIZE * SIZE].reshape(count, SIZE, SIZE).astype(np.float64)
    t, note = frame_times(r.stderr, count)
    if not np.median(np.diff(t)) > 0:
        raise RuntimeError(f"{path}: the frame timestamps do not increase")
    return t, frames, note


# ---------------------------------------------------------------- analysis

def levels(x, rate=RATE, hz=None, highpass=None):
    """(window centre times, level in dB) of WIN_S windows every HOP_S, in the
    band hz/1.25..hz*1.25 or above ``highpass`` Hz (zero-phase filters, so no
    delay), else the whole signal."""
    x = np.asarray(x, dtype=np.float64)
    if hz is not None:
        sos = signal.butter(4, [hz / 1.25, hz * 1.25], btype="bandpass", fs=rate, output="sos")
        x = signal.sosfiltfilt(sos, x)
    elif highpass is not None:
        x = signal.sosfiltfilt(signal.butter(4, highpass, btype="highpass", fs=rate, output="sos"), x)
    n, hop = int(round(WIN_S * rate)), int(round(HOP_S * rate))
    if len(x) < n:
        return np.zeros(0), np.zeros(0)
    c = np.concatenate(([0.0], np.cumsum(x * x)))
    starts = np.arange(0, len(x) - n + 1, hop)
    power = (c[starts + n] - c[starts]) / n
    return (starts + n / 2) / rate, 10 * np.log10(np.maximum(power, 1e-12))


def audio_onsets(t, level, spacing, rel_db=6.0):
    """(onset times in s, loudest event over the median in dB) of the sound
    events in a level series: the loudest window within +-spacing/2, at least
    MIN_SNR_DB over the median and within PEAK_SPREAD_DB of the loud ones;
    the onset is the first window of RUN in a row within rel_db of it."""
    if len(level) < 2:
        return [], 0.0
    hop = t[1] - t[0]
    floor = float(np.median(level))
    snr = float(level.max() - floor)
    peaks, _ = signal.find_peaks(level, height=floor + MIN_SNR_DB,
                                 distance=max(1, int(spacing / 2 / hop)))
    if len(peaks):
        top = np.percentile(level[peaks], 90)
        peaks = peaks[level[peaks] >= top - PEAK_SPREAD_DB]
    onsets = []
    for p in peaks:
        thr = level[p] - rel_db
        k = p
        for i in range(max(0, p - int(round(SEARCH_S / hop))), p + 1):
            if np.all(level[i:i + RUN] >= thr):
                k = i
                break
        onsets.append(float(t[k]))
    return onsets, snr


def brightness(frames):
    return frames.reshape(len(frames), -1).mean(axis=1)


def picture_change(frames):
    """Mean absolute change of each frame from the one before (0 for the first)."""
    d = np.abs(np.diff(frames, axis=0)).reshape(len(frames) - 1, -1).mean(axis=1)
    return np.concatenate(([0.0], d))


def video_onsets(t, y, spacing, at_peak=False):
    """Event times in s in a per-frame series ``y`` (brightness, or change).

    ``y`` minus a running median over 2 x spacing; an event is the peak of
    its +-spacing/2 that stands VIDEO_SNR x the noise and VIDEO_MIN over that
    baseline, and at least VIDEO_REL (CLAP_REL with ``at_peak``) of the
    typical (90th percentile) event. The noise comes from the median
    frame-to-frame step, which events and smooth motion (a clap video moves
    most of the time) hardly change. The event's time is where ``y`` first
    rose FRACTION of the way to its peak, interpolated between frames, or
    with ``at_peak`` the peak frame itself."""
    t = np.asarray(t, dtype=np.float64)
    dt = float(np.median(np.diff(t)))
    width = max(3, int(2 * spacing / dt) | 1)
    d = np.asarray(y, dtype=np.float64) - ndimage.median_filter(y, size=width, mode="nearest")
    noise = 1.4826 * float(np.median(np.abs(np.diff(d)))) / np.sqrt(2)
    peaks, _ = signal.find_peaks(d, height=max(VIDEO_SNR * noise, VIDEO_MIN),
                                 distance=max(1, int(spacing / 2 / dt)))
    if len(peaks):
        rel = CLAP_REL if at_peak else VIDEO_REL
        peaks = peaks[d[peaks] >= rel * np.percentile(d[peaks], 90)]
    out = []
    for p in peaks:
        if at_peak:
            out.append(float(t[p]))
            continue
        level = FRACTION * d[p]
        k = p
        while k > 0 and d[k - 1] >= level:
            k -= 1
        if k == 0:
            continue                          # rose before the first frame: no onset
        f = (level - d[k - 1]) / (d[k] - d[k - 1])
        out.append(float(t[k - 1] + f * (t[k] - t[k - 1])))
    return out


def pair(video, audio, window):
    """[(video s, audio s)] one to one, nearest first, |audio - video| < window,
    in video order."""
    cands = sorted((abs(a - v), i, j) for i, v in enumerate(video)
                   for j, a in enumerate(audio) if abs(a - v) < window)
    used_v, used_a, out = set(), set(), []
    for _, i, j in cands:
        if i not in used_v and j not in used_a:
            used_v.add(i)
            used_a.add(j)
            out.append((video[i], audio[j]))
    return sorted(out)


def offsets_ms(pairs):
    return np.array([(a - v) * 1000 for v, a in pairs])


def analyse(path, spacing, hz=None, rel_db=6.0, clap=False):
    """Measure one video: hz set, the fan (band audio, brightness onsets);
    hz None, a calibration (broadband audio, brightness or with ``clap`` the
    largest picture change)."""
    with tempfile.TemporaryDirectory() as tmp:
        x = extract_audio(path, tmp)
        t, frames, note = extract_video(path, tmp)
    ta, level = levels(x, hz=hz, highpass=None if hz else HIGHPASS_HZ)
    audio, snr = audio_onsets(ta, level, spacing, rel_db)
    y = picture_change(frames) if clap else brightness(frames)
    video = video_onsets(t, y, spacing, at_peak=clap)
    fps = 1 / float(np.median(np.diff(t)))
    return Analysis(os.path.basename(path), max(len(x) / RATE, float(t[-1])), fps,
                    audio, video, pair(video, audio, spacing / 2), snr, note)


# ------------------------------------------------------------------- print

def fmt_ms(v):
    return f"{v:+.0f}"


def report(a, what, spacing, band, seen="flashes", heard="bursts"):
    """Printed lines for one Analysis (``seen``, ``heard``: its event names)."""
    frame_ms = 1000 / a.fps
    out = [f"{what} {a.name}: {a.seconds:.1f} s, video {a.fps:.2f} fps "
           f"(frame {frame_ms:.1f} ms), audio {band}"]
    if a.timing:
        out.append(f"  note: {a.timing}")
    if a.fps < FAST_FPS:
        out.append(f"  note: {a.fps:.0f} fps: each flash time is good to about "
                   f"+-{frame_ms / 2:.0f} ms (one frame is {frame_ms:.0f} ms); record at 60 fps")
    out.append(f"  {seen} {len(a.video_s)}, {heard} {len(a.audio_s)} "
               f"(loudest {a.snr_db:.0f} dB over the band's median)")
    out.append(f"  pairs {len(a.pairs)} (nearest within +-{spacing / 2:.2f} s):")
    if a.pairs:
        out.append("       video s    audio s   audio-video ms")
    for v, s in a.pairs:
        out.append(f"    {v:9.3f}  {s:9.3f}   {fmt_ms((s - v) * 1000):>6}")
    lone_v = len(a.video_s) - len(a.pairs)
    lone_a = len(a.audio_s) - len(a.pairs)
    if lone_v or lone_a:
        out.append(f"  unpaired: {lone_v} {seen}, {lone_a} {heard}")
    if a.pairs:
        ms = offsets_ms(a.pairs)
        q1, med, q3 = np.percentile(ms, [25, 50, 75])
        out.append(f"  audio minus video: median {fmt_ms(med)} ms, IQR {fmt_ms(q1)}..{fmt_ms(q3)} ms, "
                   f"range {fmt_ms(ms.min())}..{fmt_ms(ms.max())} ms, {len(ms)} pairs")
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("video", help="phone video (mov/mp4) of the fan playing the sync clip")
    p.add_argument("--flash-period", type=float, default=PERIOD_S, metavar="S",
                   help="seconds between flashes (default 1.6: clipgen flash:32 at 20 fps)")
    p.add_argument("--hz", type=float, default=HZ, help="burst frequency, Hz (default 2000)")
    p.add_argument("--rel-db", type=float, default=6.0, metavar="DB",
                   help="audio onset: dB under the burst's loudest 5 ms window (default 6)")
    p.add_argument("--min-pairs", type=int, default=5, metavar="N",
                   help="fewest pairs to report, in each video (default 5)")
    p.add_argument("--calibrate", metavar="VIDEO2",
                   help="phone A/V reference video (screen flash + beep, or --cal-clap)")
    p.add_argument("--cal-clap", action="store_true", help="VIDEO2 is hand claps, not a flash")
    args = p.parse_args(argv)
    try:
        if args.flash_period <= 0.2:
            raise ValueError("--flash-period must be over 0.2 s")
        if args.min_pairs < 1:
            raise ValueError("--min-pairs must be at least 1")
        if not 100 <= args.hz <= RATE / 2 / 1.25 - 1:
            raise ValueError(f"--hz must be 100..{RATE / 2 / 1.25 - 1:.0f}")
        for path in (args.video, args.calibrate):
            if path and not os.path.exists(path):
                raise ValueError(f"no such file: {path}")
        fan = analyse(args.video, args.flash_period, args.hz, args.rel_db)
        cal = analyse(args.calibrate, CAL_SPACING_S, None, args.rel_db, args.cal_clap) \
            if args.calibrate else None
    except (ValueError, RuntimeError) as e:
        print(f"error: {e}")
        return 1
    band = f"{args.hz / 1.25:.0f}-{args.hz * 1.25:.0f} Hz"
    print("\n".join(report(fan, "fan", args.flash_period, band)))
    if cal:
        visual = "largest picture change (clap)" if args.cal_clap else "brightness onset"
        print("\n".join(report(cal, "calibration", CAL_SPACING_S,
                               f"above {HIGHPASS_HZ:.0f} Hz; video {visual}",
                               "claps" if args.cal_clap else "flashes", "sounds")))
    short = [f"{what} {a.name}: {len(a.pairs)}" for what, a in (("fan", fan), ("calibration", cal))
             if a and len(a.pairs) < args.min_pairs]
    if short:
        print(f"too few pairs (want {args.min_pairs}): " + "; ".join(short))
        print("  check the band level above, the fan filling the frame, and that the video "
              "covers 5 or more flashes")
        return 2
    fan_ms = float(np.median(offsets_ms(fan.pairs)))
    if cal:
        cal_ms = float(np.median(offsets_ms(cal.pairs)))
        print(f"result: audio minus video {fmt_ms(fan_ms - cal_ms)} ms "
              f"(fan {fmt_ms(fan_ms)} ms, minus the phone's own {fmt_ms(cal_ms)} ms)")
    else:
        print(f"result: audio minus video {fmt_ms(fan_ms)} ms (uncalibrated: includes the phone's "
              f"own A/V offset; --calibrate removes it)")
    print("  positive: the sound comes after the flash. Bluetooth A2DP typically adds "
          "100-250 ms, a wired path about 0.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
