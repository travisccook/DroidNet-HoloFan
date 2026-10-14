#!/usr/bin/env python3
"""Check, back up, first-flash and restore the HoloFan's ESP32-C2 through a
3.3 V USB-serial adapter on the fan's five-hole header (README, "Connecting
the fan"). Run it with ESP-IDF's Python after `. $IDF_PATH/export.sh`.

The adapter has no auto-reset: hold GPIO9 (hole 2) to GND while you apply
3.3 V, so the chip waits in download mode, then run one of:

    python fanflash.py check   PORT       read-only: chip, flash size, eFuse gate
    python fanflash.py backup  PORT DIR   read-only on the fan: read all 2 MB twice,
                                          keep it as DIR/stock-<mac>.bin if both match
    python fanflash.py first   PORT DIR   write bootloader, partition table, otadata
                                          and app; never NVS; needs this chip's backup
    python fanflash.py restore PORT DIR   write this chip's stock image back (2 MB)
    python fanflash.py app     PORT DIR   recovery only: the app into ota_0 (0x10000);
                                          needs this chip's backup

Exit 0 done, 2 usage, 3 refused. ESPTOOL / ESPEFUSE replace the esptool and
espefuse commands (the tests use a fake); HOLOFAN_BUILD replaces build/.
"""
import datetime
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

CHIP, BAUD, FLASH_SIZE = "esp32c2", "115200", 0x200000
HERE = os.path.dirname(os.path.abspath(__file__))
FIRST_IMAGES = [("0x0", "bootloader/bootloader.bin"),
                ("0x8000", "partition_table/partition-table.bin"),
                ("0x1F0000", "ota_data_initial.bin"),
                ("0x10000", "holofan.bin")]
# eFuse -> (accepted values, what any other value means)
GATE = {"SPI_BOOT_CRYPT_CNT": (("Disable", 0, 3), "flash encryption is on"),
        "SECURE_BOOT_EN": ((False, 0), "secure boot is on"),
        "DIS_DOWNLOAD_MODE": ((False, 0), "download mode is disabled"),
        "ENABLE_SECURITY_DOWNLOAD": ((False, 0), "secure download mode is on")}


class Refuse(Exception):
    pass


def build_dir():
    return os.environ.get("HOLOFAN_BUILD", os.path.join(HERE, "build"))


def command(env_name, module):
    return shlex.split(os.environ.get(env_name, f"{shlex.quote(sys.executable)} -m {module}"))


def esptool(port, *args, capture=True):
    cmd = command("ESPTOOL", "esptool") + ["--chip", CHIP, "-p", port, "-b", BAUD,
                                           "--before", "no_reset", "--after", "no_reset", *args]
    r = subprocess.run(cmd, capture_output=capture, text=True)
    if r.returncode != 0:
        tail = ((r.stdout or "") + (r.stderr or ""))[-800:]
        raise Refuse(f"esptool {args[0]} failed (exit {r.returncode}); check the wiring and GPIO9\n{tail}")
    return r.stdout or ""


def read_mac(port):
    m = re.search(r"MAC:\s*([0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5})", esptool(port, "read_mac"))
    if not m:
        raise Refuse("esptool read_mac printed no MAC")
    return m.group(1).lower()


def flash_size(port):
    m = re.search(r"Detected flash size:\s*(\S+)", esptool(port, "flash_id"))
    return m.group(1) if m else "unknown"


def read_efuses(port):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "efuse.json")
        cmd = command("ESPEFUSE", "espefuse") + ["--chip", CHIP, "--port", port, "--baud", BAUD,
                                                 "--before", "no_reset", "summary", "--format", "json",
                                                 "--file", out]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(out):
            raise Refuse(f"espefuse summary failed (exit {r.returncode})\n{(r.stdout + r.stderr)[-800:]}")
        with open(out) as f:
            return json.load(f)


def efuse_problems(efuses):
    problems = []
    for name, (ok, why) in GATE.items():
        if name not in efuses:
            problems.append(f"{name} is missing from the eFuse summary")
            continue
        value = efuses[name].get("value")
        if not any(value == v and type(value) is type(v) for v in ok):
            problems.append(f"{name} = {value!r}: {why}")
    return problems


def check(port):
    mac, size, efuses = read_mac(port), flash_size(port), read_efuses(port)
    problems = efuse_problems(efuses)
    if size != "2MB":
        problems.append(f"flash size {size}, expected 2MB")
    print(f"chip ESP32-C2, MAC {mac}, flash {size}")
    if problems:
        raise Refuse("this fan cannot take the HoloFan firmware:\n  " + "\n  ".join(problems))
    print("eFuse gate: no flash encryption, no secure boot, download mode enabled: OK")
    return {"mac": mac, "flash_size": size, "efuses": efuses}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            h.update(block)
    return h.hexdigest()


def backup_paths(directory, mac):
    stem = os.path.join(directory, "stock-" + mac.replace(":", ""))
    return stem + ".bin", stem + ".json"


def backup(port, directory):
    info = check(port)
    os.makedirs(directory, exist_ok=True)
    bin_path, meta_path = backup_paths(directory, info["mac"])
    if os.path.exists(bin_path) or os.path.exists(meta_path):
        raise Refuse(f"{bin_path} already exists: a stock backup is never overwritten")
    reads = [os.path.join(directory, f".read{n}.tmp") for n in (1, 2)]
    try:
        for path in reads:
            print(f"reading 2 MB into {path} ...")
            esptool(port, "read_flash", "0", hex(FLASH_SIZE), path, capture=False)
        sizes = [os.path.getsize(p) for p in reads]
        sums = [sha256(p) for p in reads]
        if sizes != [FLASH_SIZE, FLASH_SIZE]:
            raise Refuse(f"read sizes {sizes}, expected {FLASH_SIZE}")
        if sums[0] != sums[1]:
            raise Refuse("the two reads differ: check the wiring and run backup again")
        os.replace(reads[0], bin_path)
    finally:
        for path in reads:
            if os.path.exists(path):
                os.remove(path)
    meta = {"mac": info["mac"], "sha256": sums[0], "size": FLASH_SIZE, "flash_size": info["flash_size"],
            "efuses": info["efuses"], "date": datetime.datetime.now().isoformat(timespec="seconds")}
    with open(meta_path, "w") as f:
        json.dump(meta, f, indent=2)
    print(f"backup OK: {bin_path} (sha256 {sums[0]}). Keep it safe: it is the only way back to stock.")


def verified_backup(directory, mac):
    bin_path, meta_path = backup_paths(directory, mac)
    if not (os.path.exists(bin_path) and os.path.exists(meta_path)):
        raise Refuse(f"no backup of chip {mac} in {directory}: run backup first")
    with open(meta_path) as f:
        meta = json.load(f)
    if meta.get("mac") != mac or os.path.getsize(bin_path) != FLASH_SIZE or sha256(bin_path) != meta.get("sha256"):
        raise Refuse(f"the backup {bin_path} is damaged (size or sha256 differs from {meta_path})")
    return bin_path


def first(port, directory):
    images = [(off, os.path.join(build_dir(), rel)) for off, rel in FIRST_IMAGES]
    missing = [p for _, p in images if not os.path.exists(p)]
    if missing:
        raise Refuse("missing " + ", ".join(missing) + ": run ./build.sh first")
    mac = read_mac(port)
    verified_backup(directory, mac)
    args = ["write_flash", "--flash_mode", "dio", "--flash_freq", "60m", "--flash_size", "2MB"]
    for off, path in images:
        args += [off, path]
    print("writing bootloader 0x0, partition table 0x8000, otadata 0x1F0000, app 0x10000 (NVS untouched)")
    esptool(port, *args, capture=False)
    print("done. Remove GPIO9 from GND, then remove and reapply 3.3 V.")


def restore(port, directory):
    path = verified_backup(directory, read_mac(port))
    print(f"writing the stock image {path} at 0x0 (2 MB)")
    esptool(port, "write_flash", "--flash_size", "keep", "0x0", path, capture=False)
    print("done. Remove GPIO9 from GND, then remove and reapply 3.3 V.")


def app(port, directory):
    path = os.path.join(build_dir(), "holofan.bin")
    if not os.path.exists(path):
        raise Refuse(f"missing {path}: run ./build.sh first")
    verified_backup(directory, read_mac(port))
    print("recovery: app into ota_0 only; if the fan boots ota_1 this changes nothing, prefer an OTA")
    esptool(port, "write_flash", "0x10000", path, capture=False)


def main(argv):
    actions = {"check": (check, 1), "backup": (backup, 2), "first": (first, 2), "restore": (restore, 2), "app": (app, 2)}
    if not argv or argv[0] not in actions or len(argv) - 1 != actions[argv[0]][1]:
        print(__doc__)
        return 2
    fn, _ = actions[argv[0]]
    try:
        fn(*argv[1:])
    except Refuse as e:
        print(f"REFUSED: {e}")
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
