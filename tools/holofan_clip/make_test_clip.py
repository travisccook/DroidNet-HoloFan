#!/usr/bin/env python3
"""Build a small black, silent HoloFan clip for bench tests.

Usage: make_test_clip.py <ee37-header.bin> <out.bin> [frames] [--allow-unlisted]

The 812-byte EE37 header comes from github.com/zackmcmurrin-dev/holo-fan-linux
(protocol/ee37-header.bin; not committed here). Output: the header, then
[frames] black frames (default 30, 1.5 s at 20 frames/s), unpadded: the fan
pads to a multiple of 28,800 bytes on upload.

A frame is this fan's (Atom PRO, F-S12, 64x520x6; docs/HOW_IT_WORKS.md,
"Clip format"): 520 lines x 6 bit-planes x 24 bytes = 74,880 bytes of 0x00
video, then 1,600 bytes of 0x80 silent audio = 76,480 bytes. The default clip
is 812 + 30 x 76,480 = 2,295,212 bytes.

Fewer than 30 frames are refused: the MCU writes a clip shorter than about one
second (20 frames) to the card but never lists it, and it cannot then be
deleted except by formatting the card (docs/HOW_IT_WORKS.md, "Clip upload over
SPI"). --allow-unlisted makes such a clip anyway, for a card you are ready to
format.

Exit 0 written, 1 bad header file, 2 usage or refused.
"""
import sys

HEADER_LEN = 812
# docs/HOW_IT_WORKS.md, "Clip format (F-S12)": 64 LEDs x 3 colours = 192 bits
# = 24 bytes per plane, 6 planes per line, 520 lines per frame.
LEDS, LINES, PLANES = 64, 520, 6
PLANE_BYTES = LEDS * 3 // 8
VIDEO_LEN = LINES * PLANES * PLANE_BYTES        # 74,880
AUDIO_LEN = 1600
FRAME_LEN = VIDEO_LEN + AUDIO_LEN               # 76,480
# docs/HOW_IT_WORKS.md: under about 20 frames a clip is never listed; "make
# test clips at least 30 frames long".
MIN_LISTED_FRAMES = 30
DEFAULT_FRAMES = MIN_LISTED_FRAMES
OVERRIDE = "--allow-unlisted"


def build(header, frames):
    return header + (bytes(VIDEO_LEN) + bytes([0x80]) * AUDIO_LEN) * frames


def main(argv):
    args = [a for a in argv[1:] if a != OVERRIDE]
    allow = OVERRIDE in argv[1:]
    if len(args) not in (2, 3) or any(a.startswith("-") for a in args):
        print(__doc__)
        return 2
    frames = DEFAULT_FRAMES
    if len(args) == 3:
        try:
            frames = int(args[2])
        except ValueError:
            frames = 0
        if frames < 1:
            print(f"frames must be a whole number from 1, not {args[2]!r}")
            return 2
    if frames < MIN_LISTED_FRAMES and not allow:
        print(f"refused: {frames} frames is under {MIN_LISTED_FRAMES}. The fan's MCU stores a clip that short "
              f"but never lists it, and it can then be removed only by formatting the card. "
              f"Use {MIN_LISTED_FRAMES} or more, or add {OVERRIDE} if that is what you want.")
        return 2
    with open(args[0], "rb") as f:
        header = f.read()
    if len(header) != HEADER_LEN or header[:2] != b"\xee\x37":
        print(f"not an EE37 header: {len(header)} bytes, starts {header[:2].hex()}")
        return 1
    clip = build(header, frames)
    with open(args[1], "wb") as f:
        f.write(clip)
    print(f"wrote {args[1]}: {len(clip)} bytes, {frames} frames of {FRAME_LEN} bytes")
    if frames < MIN_LISTED_FRAMES:
        print(f"WARNING: under {MIN_LISTED_FRAMES} frames: the fan will not list this clip, "
              "and only formatting the card removes it")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
