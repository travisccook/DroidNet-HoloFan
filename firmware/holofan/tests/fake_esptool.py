#!/usr/bin/env python3
"""Stand-in for esptool and espefuse in tests: logs argv (one JSON list per line
to $FAKE_LOG) and behaves per env:
  FAKE_MAC (default 02:00:00:00:00:01), FAKE_FLASH (default 2MB),
  FAKE_EFUSE (JSON object merged over a good C2 summary), FAKE_FAIL (a
  subcommand that exits 2), FAKE_FAIL_NTH (fail only that call number of
  FAKE_FAIL, 1-based), FAKE_UNSTABLE=1 (the second read_flash differs).
Never touches a serial port."""
import json
import os
import sys

argv = sys.argv[1:]
log = os.environ.get("FAKE_LOG")
if log:
    with open(log, "a") as f:
        f.write(json.dumps(argv) + "\n")
SUBS = ("read_mac", "flash_id", "read_flash", "write_flash", "summary", "chip_id")
sub = next((a for a in argv if a in SUBS), None)
state = (log or "/dev/null") + ".count"
counts = {}
if log and os.path.exists(state):
    with open(state) as f:
        counts = json.load(f)
counts[sub] = counts.get(sub, 0) + 1
if log:
    with open(state, "w") as f:
        json.dump(counts, f)
if os.environ.get("FAKE_FAIL") == sub and int(os.environ.get("FAKE_FAIL_NTH", counts[sub])) == counts[sub]:
    print(f"A fatal error occurred: fake {sub} failure")
    sys.exit(2)
if sub == "read_mac":
    print("Chip is ESP32-C2 (QFN24) (revision v1.2)")
    print("MAC: " + os.environ.get("FAKE_MAC", "02:00:00:00:00:01"))
elif sub == "flash_id":
    print("Manufacturer: a1\nDevice: 4015\nDetected flash size: " + os.environ.get("FAKE_FLASH", "2MB"))
elif sub == "read_flash":
    i = argv.index("read_flash")
    size, path = int(argv[i + 2], 0), argv[i + 3]
    data = bytearray((n * 7) & 0xFF for n in range(size))
    if os.environ.get("FAKE_UNSTABLE") == "1" and counts[sub] == 2:
        data[1234] ^= 0xFF
    with open(path, "wb") as f:
        f.write(bytes(data))
elif sub == "summary":
    efuse = {"SPI_BOOT_CRYPT_CNT": {"value": "Disable"}, "SECURE_BOOT_EN": {"value": False},
             "DIS_DOWNLOAD_MODE": {"value": False}, "ENABLE_SECURITY_DOWNLOAD": {"value": False}}
    efuse.update(json.loads(os.environ.get("FAKE_EFUSE", "{}")))
    out = argv[argv.index("--file") + 1]
    with open(out, "w") as f:
        json.dump(efuse, f)
sys.exit(0)
