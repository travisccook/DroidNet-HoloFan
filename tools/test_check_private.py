#!/usr/bin/env python3
"""Tests for check_private.py. Run: python3 -B -m unittest discover -s tools -p 'test_check_private.py' -v
(in the private repo: -s tools/release)."""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_private  # noqa: E402

HOME_IP = "192.168." + "1.23"
TEN_IP = "10." + "1.2.3"
REAL_MAC = "00:00:5e:" + "00:53:01"       # RFC 7042 documentation MAC
LOCAL_MAC = "02:12:" + "34:00:00:10"
USER_PATH = "/Us" + "ers/alice/x"
MAIL = "alice" + "@" + "gmail.com"


class ScanText(unittest.TestCase):
    def kinds(self, text, local=()):
        return [m for _, m in check_private.scan_text(text, list(local))]

    def test_private_ipv4_is_flagged(self):
        self.assertEqual(len(self.kinds(f"fan at {HOME_IP}\n")), 1)
        self.assertEqual(len(self.kinds(f"x {TEN_IP} y")), 1)

    def test_private_ipv4_ending_a_sentence_is_flagged(self):
        self.assertEqual(len(self.kinds(f"the fan was at {HOME_IP}.")), 1)

    def test_hyphen_mac_flagged_local_hyphen_mac_passes(self):
        self.assertEqual(len(self.kinds("MAC " + REAL_MAC.replace(":", "-").upper())), 1)
        self.assertEqual(self.kinds("src " + LOCAL_MAC.replace(":", "-")), [])

    def test_fan_ap_and_public_and_versions_pass(self):
        self.assertEqual(self.kinds("GET http://192.168.4.1/info from 192.168.4.2"), [])
        self.assertEqual(self.kinds("dns 8.8.8.8; Arduino 3.3.12; pioarduino 55.03.312-1; v5.4.2"), [])

    def test_real_mac_flagged_local_and_broadcast_pass(self):
        self.assertEqual(len(self.kinds(f"MAC: {REAL_MAC}")), 1)
        self.assertEqual(self.kinds(f"src MAC: {LOCAL_MAC}; ff:ff:ff:ff:ff:ff; 00:00:00:00:00:00"), [])

    def test_times_and_clip_header_hex_pass(self):
        self.assertEqual(self.kinds("19:22:55.000 rece [ETM] WCB16 came ONLINE"), [])
        self.assertEqual(self.kinds('    "ee3799b7747775b7717770b7b277b3b77b777ab7b877b9b7bd77bcb77e777fb7"'), [])

    def test_personal_path_and_email_flagged_examples_pass(self):
        self.assertEqual(len(self.kinds(USER_PATH)), 1)
        self.assertEqual(len(self.kinds(MAIL)), 1)
        self.assertEqual(self.kinds("me@example.com noreply@anthropic.com @dataclass"), [])

    def test_local_literal_case_insensitive_by_number_never_value(self):
        found = check_private.scan_text("Line one\nthe Secret-Word here\n", ["nothing", "secret-word"])
        self.assertEqual(found, [(2, "local pattern #2")])

    def test_line_numbers(self):
        found = check_private.scan_text(f"a\nb\n{HOME_IP}\n", [])
        self.assertEqual(found[0][0], 3)


class LoadLocal(unittest.TestCase):
    def test_comments_blanks_and_missing_file(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "patterns.txt")
            with open(p, "w") as f:
                f.write("# comment\n\n  alpha  \nbeta\n")
            self.assertEqual(check_private.load_local(p), ["alpha", "beta"])
            self.assertEqual(check_private.load_local(os.path.join(d, "none.txt")), [])


class MainInRepo(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = self.tmp.name
        subprocess.run(["git", "init", "-q", self.repo], check=True)
        self.pat = os.path.join(self.repo, "..", os.path.basename(self.repo) + "-patterns.txt")
        with open(self.pat, "w") as f:
            f.write("zebra-crossing\n")

    def tearDown(self):
        os.remove(self.pat)
        self.tmp.cleanup()

    def run_main(self, *args):
        env = dict(os.environ, DROIDNET_PRIVATE_PATTERNS=self.pat)
        return subprocess.run([sys.executable, os.path.join(HERE, "check_private.py"), *args],
                              cwd=self.repo, env=env, capture_output=True, text=True)

    def write(self, name, text):
        with open(os.path.join(self.repo, name), "w") as f:
            f.write(text)

    def test_clean_then_dirty_untracked_file(self):
        self.write("a.txt", "hello\n")
        self.assertEqual(self.run_main().returncode, 0)
        self.write("b.txt", "near the Zebra-Crossing\n")
        r = self.run_main()
        self.assertEqual(r.returncode, 1)
        self.assertIn("b.txt:1: local pattern #1", r.stdout)
        self.assertNotIn("Zebra", r.stdout + r.stderr)

    def test_staged_scans_the_index_not_the_working_tree(self):
        self.write("c.txt", f"at {HOME_IP}\n")
        subprocess.run(["git", "add", "c.txt"], cwd=self.repo, check=True)
        self.write("c.txt", "clean now\n")
        self.assertEqual(self.run_main("--staged").returncode, 1)
        self.assertEqual(self.run_main("c.txt").returncode, 0)

    def test_ignored_files_are_skipped(self):
        self.write(".gitignore", "build/\n")
        os.makedirs(os.path.join(self.repo, "build"))
        self.write("build/x.txt", f"{HOME_IP}\n")
        self.assertEqual(self.run_main().returncode, 0)


if __name__ == "__main__":
    unittest.main()
