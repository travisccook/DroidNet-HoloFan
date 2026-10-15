"""README.md carries a copy of docs/SERIAL_COMMANDS.md under "Command
reference" (headings one level down, without the page title). This test fails
when the two drift apart: change both together."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
START = "<!-- copy of docs/SERIAL_COMMANDS.md: change both together -->"
END = "<!-- end of the copy -->"


def reference_from_doc():
    lines = (ROOT / "docs" / "SERIAL_COMMANDS.md").read_text().splitlines()[1:]
    return "\n".join("#" + line if line.startswith("## ") else line
                     for line in lines).strip()


def reference_in_readme():
    text = (ROOT / "README.md").read_text()
    if START not in text or END not in text:
        return ""
    return text[text.index(START) + len(START):text.index(END)].strip()


class ReadmeCommandReferenceTest(unittest.TestCase):
    def test_readme_carries_the_full_command_reference(self):
        self.assertEqual(reference_in_readme(), reference_from_doc())


if __name__ == "__main__":
    unittest.main()
