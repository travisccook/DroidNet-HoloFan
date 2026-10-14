#!/usr/bin/env python3
"""Tests for fanflash.py against fake_esptool.py (no hardware, no serial port).
Run: python3 -B -m unittest discover -s firmware/holofan/tests -v"""
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "fanflash.py")
FAKE = f"{sys.executable} {os.path.join(HERE, 'fake_esptool.py')}"
MAC = "02:00:00:00:00:01"
STEM = "stock-020000000001"


class FanFlash(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = self.tmp.name
        self.backups = os.path.join(self.d, "backups")
        self.log = os.path.join(self.d, "calls.log")
        self.build = os.path.join(self.d, "build")
        for rel in ("bootloader/bootloader.bin", "partition_table/partition-table.bin",
                    "ota_data_initial.bin", "holofan.bin"):
            os.makedirs(os.path.dirname(os.path.join(self.build, rel)), exist_ok=True)
            with open(os.path.join(self.build, rel), "wb") as f:
                f.write(b"\x00" * 16)

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *args, **env):
        e = dict(os.environ, ESPTOOL=FAKE, ESPEFUSE=FAKE, FAKE_LOG=self.log, HOLOFAN_BUILD=self.build)
        e.update(env)
        return subprocess.run([sys.executable, TOOL, *args], env=e, capture_output=True, text=True)

    def calls(self):
        if not os.path.exists(self.log):
            return []
        with open(self.log) as f:
            return [json.loads(line) for line in f]

    def writes(self):
        return [c for c in self.calls() if "write_flash" in c]

    def make_backup(self, **env):
        r = self.run_tool("backup", "PORT", self.backups, **env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    # check
    def test_check_passes_a_good_fan(self):
        r = self.run_tool("check", "PORT")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn(MAC, r.stdout)

    def test_check_refuses_flash_encryption(self):
        r = self.run_tool("check", "PORT", FAKE_EFUSE='{"SPI_BOOT_CRYPT_CNT": {"value": "Enable"}}')
        self.assertEqual(r.returncode, 3)
        self.assertIn("flash encryption", r.stdout)

    def test_check_refuses_secure_boot_disabled_download_and_4mb(self):
        for env in ({"FAKE_EFUSE": '{"SECURE_BOOT_EN": {"value": true}}'},
                    {"FAKE_EFUSE": '{"DIS_DOWNLOAD_MODE": {"value": true}}'},
                    {"FAKE_EFUSE": '{"ENABLE_SECURITY_DOWNLOAD": {"value": true}}'},
                    {"FAKE_FLASH": "4MB"}):
            self.assertEqual(self.run_tool("check", "PORT", **env).returncode, 3, env)

    def test_check_accepts_integer_efuse_values(self):
        r = self.run_tool("check", "PORT", FAKE_EFUSE='{"SPI_BOOT_CRYPT_CNT": {"value": 0}, '
                                                      '"SECURE_BOOT_EN": {"value": 0}}')
        self.assertEqual(r.returncode, 0, r.stdout)

    # backup
    def test_backup_reads_twice_and_keeps_a_verified_image(self):
        self.make_backup()
        reads = [c for c in self.calls() if "read_flash" in c]
        self.assertEqual(len(reads), 2)
        self.assertEqual(reads[0][reads[0].index("read_flash") + 1:][:2], ["0", "0x200000"])
        bin_path = os.path.join(self.backups, STEM + ".bin")
        with open(os.path.join(self.backups, STEM + ".json")) as f:
            meta = json.load(f)
        self.assertEqual(os.path.getsize(bin_path), 0x200000)
        with open(bin_path, "rb") as f:
            self.assertEqual(meta["sha256"], hashlib.sha256(f.read()).hexdigest())
        self.assertEqual(meta["mac"], MAC)
        self.assertEqual(sorted(os.listdir(self.backups)), [STEM + ".bin", STEM + ".json"])
        self.assertEqual(self.writes(), [])

    def test_backup_refuses_when_reads_differ(self):
        r = self.run_tool("backup", "PORT", self.backups, FAKE_UNSTABLE="1")
        self.assertEqual(r.returncode, 3)
        self.assertIn("differ", r.stdout)
        self.assertEqual(os.listdir(self.backups), [])

    def test_backup_second_read_failing_leaves_nothing(self):
        r = self.run_tool("backup", "PORT", self.backups, FAKE_FAIL="read_flash", FAKE_FAIL_NTH="2")
        self.assertEqual(r.returncode, 3)
        self.assertEqual(os.listdir(self.backups), [])

    def test_backup_never_overwrites(self):
        self.make_backup()
        r = self.run_tool("backup", "PORT", self.backups)
        self.assertEqual(r.returncode, 3)
        self.assertIn("already", r.stdout)

    def test_backup_refuses_a_locked_fan(self):
        r = self.run_tool("backup", "PORT", self.backups, FAKE_EFUSE='{"SECURE_BOOT_EN": {"value": true}}')
        self.assertEqual(r.returncode, 3)
        self.assertEqual([c for c in self.calls() if "read_flash" in c], [])

    # first
    def test_first_refuses_without_a_backup(self):
        r = self.run_tool("first", "PORT", self.backups)
        self.assertEqual(r.returncode, 3)
        self.assertIn("backup", r.stdout)
        self.assertEqual(self.writes(), [])

    def test_first_refuses_a_backup_of_another_chip(self):
        self.make_backup(FAKE_MAC="02:00:00:00:00:99")
        r = self.run_tool("first", "PORT", self.backups)
        self.assertEqual(r.returncode, 3)
        self.assertEqual(self.writes(), [])

    def test_first_refuses_a_damaged_backup(self):
        self.make_backup()
        with open(os.path.join(self.backups, STEM + ".bin"), "r+b") as f:
            f.seek(10)
            f.write(b"\xAA")
        r = self.run_tool("first", "PORT", self.backups)
        self.assertEqual(r.returncode, 3)
        self.assertIn("damaged", r.stdout)
        self.assertEqual(self.writes(), [])

    def test_first_refuses_missing_build_images(self):
        self.make_backup()
        os.remove(os.path.join(self.build, "holofan.bin"))
        r = self.run_tool("first", "PORT", self.backups)
        self.assertEqual(r.returncode, 3)
        self.assertIn("build.sh", r.stdout)
        self.assertEqual(self.writes(), [])

    def test_first_writes_four_images_and_never_nvs(self):
        self.make_backup()
        r = self.run_tool("first", "PORT", self.backups)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        (w,) = self.writes()
        offsets = [a for a in w if a.startswith("0x")]
        self.assertEqual(offsets, ["0x0", "0x8000", "0x1F0000", "0x10000"])
        self.assertNotIn("0x9000", w)
        for flag in ("--before", "no_reset", "--after", "--chip", "esp32c2"):
            self.assertIn(flag, w)

    # restore and app
    def test_restore_writes_the_backup_at_zero(self):
        self.make_backup()
        r = self.run_tool("restore", "PORT", self.backups)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        (w,) = self.writes()
        self.assertEqual(w[-2:], ["0x0", os.path.join(self.backups, STEM + ".bin")])

    def test_restore_refuses_without_a_backup(self):
        self.assertEqual(self.run_tool("restore", "PORT", self.backups).returncode, 3)
        self.assertEqual(self.writes(), [])

    def test_app_refuses_without_a_backup(self):
        r = self.run_tool("app", "PORT", self.backups)
        self.assertEqual(r.returncode, 3)
        self.assertIn("backup", r.stdout)
        self.assertEqual(self.writes(), [])

    def test_app_writes_only_ota_0(self):
        self.make_backup()
        r = self.run_tool("app", "PORT", self.backups)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        (w,) = self.writes()
        self.assertEqual(w[-2:], ["0x10000", os.path.join(self.build, "holofan.bin")])

    def test_usage(self):
        self.assertEqual(self.run_tool().returncode, 2)


if __name__ == "__main__":
    unittest.main()
