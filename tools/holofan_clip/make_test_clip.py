#!/usr/bin/env python3
"""Build the smallest playable HoloFan clip for bench tests.

Usage: make_test_clip.py <ee37-header.bin> <out.bin> [frames]

The 812-byte EE37 header comes from github.com/zackmcmurrin-dev/holo-fan-linux
(protocol/ee37-header.bin; not committed here). Output: header + one black
frame (76,800 bytes of 0x00 video, 1,600 bytes of 0x80 silent audio) =
79,212 bytes, unpadded: the fan pads to 86,400 on upload. [frames] (default
1) repeats the black frame; at 20 fps the MCU's list (`s`) appears to skip
clips under about a second (bench 2026-10-06), so use 30 for a listed clip
(docs/HOW_IT_WORKS.md, "Clips").
"""
import sys

HEADER_LEN = 812
VIDEO_LEN = 76800
AUDIO_LEN = 1600


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__)
        return 2
    with open(argv[1], "rb") as f:
        header = f.read()
    if len(header) != HEADER_LEN or header[:2] != b"\xee\x37":
        print(f"not an EE37 header: {len(header)} bytes, starts {header[:2].hex()}")
        return 1
    frames = int(argv[3]) if len(argv) == 4 else 1
    clip = header + (bytes(VIDEO_LEN) + bytes([0x80]) * AUDIO_LEN) * frames
    with open(argv[2], "wb") as f:
        f.write(clip)
    print(f"wrote {argv[2]}: {len(clip)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
