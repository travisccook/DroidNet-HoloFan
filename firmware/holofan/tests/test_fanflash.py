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

    # an interrupted backup
    def half_backup(self, keep):
        self.make_backup()
        os.remove(os.path.join(self.backups, STEM + (".json" if keep == ".bin" else ".bin")))
        with open(os.path.join(self.backups, STEM + keep), "rb") as f:
            return f.read()

    def test_image_without_metadata_gets_one_clear_message_everywhere(self):
        before = self.half_backup(".bin")
        for action in ("backup", "first", "restore", "app"):
            r = self.run_tool(action, "PORT", self.backups)
            self.assertEqual(r.returncode, 3, action)
            self.assertIn("interrupted", r.stdout, action)
            self.assertIn("run backup again", r.stdout, action)
            self.assertNotIn("already exists", r.stdout, action)
            self.assertNotIn("no backup of chip", r.stdout, action)
        self.assertEqual(os.listdir(self.backups), [STEM + ".bin"])     # nothing deleted, nothing added
        with open(os.path.join(self.backups, STEM + ".bin"), "rb") as f:
            self.assertEqual(f.read(), before)                          # and not overwritten
        self.assertEqual(self.writes(), [])
        self.assertEqual([c for c in self.calls() if "read_flash" in c][2:], [])    # no new read either

    def test_metadata_without_image_is_reported(self):
        self.half_backup(".json")
        for action in ("backup", "first", "restore", "app"):
            r = self.run_tool(action, "PORT", self.backups)
            self.assertEqual(r.returncode, 3, action)
            self.assertIn("image is missing", r.stdout, action)
        self.assertEqual(os.listdir(self.backups), [STEM + ".json"])
        self.assertEqual(self.writes(), [])

    def test_unreadable_metadata_is_refused_not_a_traceback(self):
        self.make_backup()
        with open(os.path.join(self.backups, STEM + ".json"), "w") as f:
            f.write("{")
        r = self.run_tool("restore", "PORT", self.backups)
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertEqual(self.writes(), [])

    def test_backup_leaves_the_pair_or_nothing(self):
        # The metadata is complete before either file gets its name.
        self.make_backup()
        self.assertEqual(sorted(os.listdir(self.backups)), [STEM + ".bin", STEM + ".json"])
        r = self.run_tool("backup", "PORT", self.backups, FAKE_MAC="02:00:00:00:00:02",
                          FAKE_FAIL="read_flash", FAKE_FAIL_NTH="4")
        self.assertEqual(r.returncode, 3)
        self.assertEqual(sorted(os.listdir(self.backups)), [STEM + ".bin", STEM + ".json"])

    # never erase, never NVS
    ERASE_WORDS = ("erase_flash", "erase-flash", "erase_region", "erase-region",
                   "--erase-all", "--erase_all", "-e")

    def all_actions(self):
        self.make_backup()
        for action in ("first", "app", "restore"):
            r = self.run_tool(action, "PORT", self.backups)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return self.calls()

    def test_no_invocation_erases(self):
        calls = self.all_actions()
        self.assertEqual(len([c for c in calls if "write_flash" in c]), 3)
        for c in calls:
            for arg in c:
                self.assertNotIn(arg, self.ERASE_WORDS, c)
                self.assertNotIn("erase", arg.lower(), c)

    def test_the_erase_check_would_catch_each_spelling(self):
        # The fake has to keep accepting an erase flag on write_flash, or the
        # test above would pass for the wrong reason.
        for word in ("--erase-all", "-e"):
            r = subprocess.run(FAKE.split() + ["write_flash", word, "0x0", "x"],
                               env=dict(os.environ, FAKE_LOG=self.log), capture_output=True, text=True)
            self.assertEqual(r.returncode, 0)
        self.assertEqual([[a for a in c if a in self.ERASE_WORDS] for c in self.calls()],
                         [["--erase-all"], ["-e"]])

    def test_fake_refuses_unknown_subcommands(self):
        for argv in (["erase_flash"], ["erase_region", "0x9000", "0x6000"], ["--chip", "esp32c2"], []):
            for env in ({}, {"FAKE_FAIL": "read_mac"}):
                r = subprocess.run(FAKE.split() + argv, env=dict(os.environ, **env),
                                   capture_output=True, text=True)
                self.assertEqual(r.returncode, 2, (argv, env))

    @staticmethod
    def partitions():
        table = {}
        with open(os.path.join(HERE, "..", "partitions.csv")) as f:
            for line in f:
                cells = [c.strip() for c in line.split("#")[0].split(",")]
                if len(cells) >= 5:
                    table[cells[0]] = (int(cells[3], 0), int(cells[4], 0))
        return table

    @staticmethod
    def write_ranges(call):
        """(offset, end) of every image a write_flash call names."""
        args = call[call.index("write_flash") + 1:]
        out = []
        for i, a in enumerate(args):
            if a.lower().startswith("0x"):
                out.append((int(a, 16), int(a, 16) + os.path.getsize(args[i + 1])))
        return out

    def test_first_and_app_never_write_into_nvs(self):
        nvs_start, nvs_size = self.partitions()["nvs"]
        self.assertEqual((nvs_start, nvs_size), (0x9000, 0x6000))
        # every image as large as fanflash.py lets it be
        import importlib.util
        spec = importlib.util.spec_from_file_location("fanflash", TOOL)
        fanflash = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fanflash)
        for _, rel, room in fanflash.FIRST_IMAGES:
            with open(os.path.join(self.build, rel), "wb") as f:
                f.truncate(room)
        self.make_backup()
        for action, count in (("first", 4), ("app", 1)):
            os.remove(self.log)
            r = self.run_tool(action, "PORT", self.backups)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            (w,) = self.writes()
            ranges = self.write_ranges(w)
            self.assertEqual(len(ranges), count)
            for start, end in ranges:
                self.assertFalse(start < nvs_start + nvs_size and end > nvs_start,
                                 f"{action} writes {start:#x}-{end:#x}, inside nvs")

    def test_first_and_app_refuse_an_image_that_would_reach_past_its_region(self):
        self.make_backup()
        for action, rel, size in (("first", "bootloader/bootloader.bin", 0x8001),
                                  ("first", "partition_table/partition-table.bin", 0x1001),
                                  ("first", "ota_data_initial.bin", 0x2001),
                                  ("first", "holofan.bin", 0xF0001),
                                  ("app", "holofan.bin", 0xF0001)):
            path = os.path.join(self.build, rel)
            with open(path, "wb") as f:
                f.truncate(size)
            r = self.run_tool(action, "PORT", self.backups)
            self.assertEqual(r.returncode, 3, (action, rel))
            self.assertIn("Nothing was written", r.stdout)
            self.assertEqual(self.writes(), [])
            with open(path, "wb") as f:
                f.truncate(16)

    def test_first_offsets_match_partitions_csv(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("fanflash", TOOL)
        fanflash = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fanflash)
        parts = self.partitions()
        got = {rel: (int(off, 16), room) for off, rel, room in fanflash.FIRST_IMAGES}
        self.assertEqual(got["holofan.bin"], parts["ota_0"])
        self.assertEqual(got["ota_data_initial.bin"], parts["otadata"])
        # Fixed by the chip and ESP-IDF, not by partitions.csv: the ESP32-C2's
        # second-stage bootloader is at 0x0 and the partition table at 0x8000
        # (sdkconfig.defaults does not move CONFIG_PARTITION_TABLE_OFFSET).
        self.assertEqual(got["bootloader/bootloader.bin"], (0x0, 0x8000))
        self.assertEqual(got["partition_table/partition-table.bin"], (0x8000, 0x1000))
        self.assertEqual(len(got), 4)
        self.assertEqual(0x8000 + 0x1000, parts["nvs"][0])
        self.assertLessEqual(parts["otadata"][0] + parts["otadata"][1], fanflash.FLASH_SIZE)
        with open(os.path.join(HERE, "..", "sdkconfig.defaults")) as f:
            self.assertNotIn("CONFIG_PARTITION_TABLE_OFFSET", f.read())

    def test_restore_is_the_only_command_that_writes_nvs(self):
        nvs_start, nvs_size = self.partitions()["nvs"]
        self.make_backup()
        r = self.run_tool("restore", "PORT", self.backups)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("NVS", r.stdout)
        (w,) = self.writes()
        ((start, end),) = self.write_ranges(w)
        self.assertEqual((start, end), (0, 0x200000))       # the whole chip, NVS included
        self.assertTrue(start <= nvs_start and end >= nvs_start + nvs_size)

    def test_usage(self):
        self.assertEqual(self.run_tool().returncode, 2)


if __name__ == "__main__":
    unittest.main()
