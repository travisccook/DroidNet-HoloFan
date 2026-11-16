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
REAL_MAC = "24:6f:28:" + "a1:b2:c3"       # a vendor's prefix with an invented tail
DOC_MAC = "00:00:5e:" + "00:53:01"        # RFC 7042 documentation MAC
PAIRS = REAL_MAC.split(":")
WIN_PATH = "C:" + "\\Users" + "\\alice\\fan"
ROOT_PATH = "/ro" + "ot/fan-backup"
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

    def test_documentation_mac_passes_in_every_form(self):
        doc = DOC_MAC.split(":")
        for text in (DOC_MAC, DOC_MAC.upper().replace(":", "-"), " ".join(doc),
                     "{" + ", ".join("0x" + p.upper() for p in doc) + "}"):
            self.assertEqual(self.kinds(text), [], text)

    def test_mac_directly_after_a_label(self):
        for text in ("mac:" + REAL_MAC, "MAC=" + REAL_MAC, "bssid:" + REAL_MAC + ",", "(" + REAL_MAC + ")"):
            self.assertEqual(len(self.kinds(text)), 1, text)
        self.assertEqual(self.kinds("mac:" + LOCAL_MAC), [])

    def test_longer_runs_of_pairs_are_not_macs(self):
        self.assertEqual(self.kinds(REAL_MAC + ":00:11"), [])            # EUI-64, a hex dump
        self.assertEqual(self.kinds("00:11:" + REAL_MAC), [])
        self.assertEqual(self.kinds(" ".join(PAIRS + PAIRS)), [])
        self.assertEqual(self.kinds(", ".join("0x" + p for p in PAIRS + ["00"])), [])
        self.assertEqual(self.kinds(", ".join("0x" + p for p in ["00"] + PAIRS)), [])

    def test_mac_as_a_c_byte_array(self):
        for sep in (", ", ",", " , ", ",\t"):
            text = "uint8_t peer[6] = {" + sep.join("0x" + p.upper() for p in PAIRS) + "};"
            self.assertEqual(len(self.kinds(text)), 1, text)
        self.assertEqual(self.kinds("{0x02, 0xD3, 0x3B, 0x00, 0x00, 0x02}"), [])     # locally administered
        self.assertEqual(self.kinds("{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}"), [])
        self.assertEqual(self.kinds("{0x00, 0x00, 0x00, 0x00, 0x00, 0x00}"), [])

    def test_mac_with_spaces_or_cisco_dots(self):
        self.assertEqual(len(self.kinds("peer " + " ".join(PAIRS))), 1)
        cisco = "".join(PAIRS[0:2]) + "." + "".join(PAIRS[2:4]) + "." + "".join(PAIRS[4:6])
        self.assertEqual(len(self.kinds("sw1# " + cisco + " DYNAMIC")), 1)
        self.assertEqual(len(self.kinds(cisco.upper() + ".")), 1)                   # ends a sentence
        # without a letter a-f these are numbers, not addresses
        self.assertEqual(self.kinds("10 20 30 40 50 60"), [])
        self.assertEqual(self.kinds("took 1200.3400.5600 in total"), [])
        self.assertEqual(self.kinds("0212." + "3400.0010"), [])                     # locally administered

    def test_windows_and_root_home_paths(self):
        self.assertEqual(len(self.kinds(WIN_PATH)), 1)
        self.assertEqual(len(self.kinds(WIN_PATH.replace("\\", "\\\\"))), 1)         # as written in C or JSON
        self.assertEqual(len(self.kinds("d:" + WIN_PATH[2:].lower())), 1)
        self.assertEqual(len(self.kinds("backups in " + ROOT_PATH)), 1)
        self.assertEqual(self.kinds("C:\\Users\\<name>\\fan and C:\\Program Files\\x"), [])
        self.assertEqual(self.kinds("$IDF_PATH/root/x ../root/x <dir>/root/ squashfs-root/x /rootfs/ /root"), [])

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
            with self.assertRaises(check_private.Unusable):
                check_private.load_local(os.path.join(d, "none.txt"), required=True)
            with self.assertRaises(check_private.Unusable):
                check_private.load_local(d)                               # a directory: exists, unreadable
            with open(p, "wb") as f:
                f.write(b"caf\xe9\n")                                      # not UTF-8
            with self.assertRaises(check_private.Unusable):
                check_private.load_local(p)


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

    def run_main(self, *args, cwd=None, **env):
        env = {**os.environ, "DROIDNET_PRIVATE_PATTERNS": self.pat, **env}
        return subprocess.run([sys.executable, os.path.join(HERE, "check_private.py"), *args],
                              cwd=cwd or self.repo, env=env, capture_output=True, text=True)

    def git(self, *args):
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", *args],
                       cwd=self.repo, check=True, capture_output=True)

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


    def test_file_names_are_scanned(self):
        self.write("Zebra-Crossing-notes.txt", "clean\n")
        r = self.run_main()
        self.assertEqual(r.returncode, 1)
        self.assertIn("file name: local pattern #1", r.stdout)
        self.assertNotIn("zebra-crossing", (r.stdout + r.stderr).replace("Zebra-Crossing-notes.txt", ""))

    def test_staged_file_names_are_scanned(self):
        name = "stock-" + REAL_MAC.replace(":", "-") + ".txt"
        self.write(name, "clean\n")
        self.assertEqual(self.run_main("--staged").returncode, 0)            # not staged yet
        self.git("add", name)
        r = self.run_main("--staged")
        self.assertEqual(r.returncode, 1)
        self.assertIn("file name: hardware MAC", r.stdout)

    def test_whole_tree_from_a_subdirectory_scans_the_whole_repository(self):
        os.makedirs(os.path.join(self.repo, "docs"))
        self.write("docs/ok.txt", "clean\n")
        self.write("top.txt", f"at {HOME_IP}\n")
        r = self.run_main(cwd=os.path.join(self.repo, "docs"))
        self.assertEqual(r.returncode, 1)
        self.assertIn("top.txt:1: private IPv4", r.stdout)

    def test_staged_from_a_subdirectory(self):
        os.makedirs(os.path.join(self.repo, "docs"))
        self.write("top.txt", f"at {HOME_IP}\n")
        self.git("add", "top.txt")
        self.assertEqual(self.run_main("--staged", cwd=os.path.join(self.repo, "docs")).returncode, 1)

    def test_outside_a_repository_is_an_error_not_a_clean_result(self):
        with tempfile.TemporaryDirectory() as d:
            r = self.run_main(cwd=d, GIT_CEILING_DIRECTORIES=os.path.dirname(d))
            self.assertEqual(r.returncode, 2)

    def test_staged_type_change_is_scanned(self):
        self.write("link.txt", "clean\n")
        self.git("add", "link.txt")
        self.git("commit", "-q", "-m", "x")
        os.remove(os.path.join(self.repo, "link.txt"))
        os.symlink(USER_PATH, os.path.join(self.repo, "link.txt"))
        self.git("add", "link.txt")
        status = subprocess.run(["git", "diff", "--cached", "--name-status"], cwd=self.repo,
                                capture_output=True, text=True).stdout
        self.assertTrue(status.startswith("T"), status)
        r = self.run_main("--staged")
        self.assertEqual(r.returncode, 1)
        self.assertIn("link.txt:1: home path", r.stdout)

    def test_link_target_is_scanned_in_the_work_tree(self):
        os.symlink(USER_PATH, os.path.join(self.repo, "link.txt"))
        r = self.run_main()
        self.assertEqual(r.returncode, 1)
        self.assertIn("link.txt: link target: home path", r.stdout)

    def test_staged_with_file_arguments_is_an_error(self):
        self.write("c.txt", f"at {HOME_IP}\n")
        r = self.run_main("--staged", "c.txt")
        self.assertEqual(r.returncode, 2)
        self.assertIn("--staged", r.stderr)

    def test_configured_pattern_file_that_cannot_be_read_is_an_error(self):
        self.write("a.txt", "hello\n")
        for bad in (os.path.join(self.repo, "no-such-patterns.txt"), self.repo):
            for args in ((), ("--staged",), ("a.txt",)):
                r = self.run_main(*args, DROIDNET_PRIVATE_PATTERNS=bad)
                self.assertEqual(r.returncode, 2, (bad, args))
                self.assertIn("error", r.stderr)

    def test_unset_variable_without_a_default_file_is_only_a_note(self):
        self.write("a.txt", "hello\n")
        with tempfile.TemporaryDirectory() as home:
            env = {k: v for k, v in os.environ.items() if k != "DROIDNET_PRIVATE_PATTERNS"}
            env["HOME"] = home
            r = subprocess.run([sys.executable, os.path.join(HERE, "check_private.py")],
                               cwd=self.repo, env=env, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn("note: no local pattern file", r.stderr)

    def test_non_ascii_pattern_matches_utf8_files_and_bad_bytes_do_not_stop_the_scan(self):
        with open(self.pat, "w", encoding="utf-8") as f:
            f.write("Caf\u00e9-Dr\u00f6ide\n")
        with open(os.path.join(self.repo, "u.txt"), "wb") as f:
            f.write(b"\xff\xfe binary-ish\n" + "im caf\u00e9-dr\u00f6ide\n".encode("utf-8")
                    + b"\x80 at " + HOME_IP.encode() + b"\n")
        r = self.run_main()
        self.assertEqual(r.returncode, 1)
        self.assertIn("u.txt:2: local pattern #1", r.stdout)
        self.assertIn("u.txt:3: private IPv4", r.stdout)


if __name__ == "__main__":
    unittest.main()
