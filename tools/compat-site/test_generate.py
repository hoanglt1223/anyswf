"""Tests for generate.py: python3 -m unittest discover -s tools/compat-site"""

import pathlib
import tempfile
import unittest

import generate

GOOD = """title = "Demo"
kind = "test-suite"
swf = "tests/swf-src/Demo.as"
swf_version = 8
actionscript = "AS2"
stage = "perfect"
targets = ["pc", "psp-ppsspp"]
evidence = "trace matches"
updated = 2026-10-25
"""


class LoadTests(unittest.TestCase):
    def load(self, *texts):
        with tempfile.TemporaryDirectory() as d:
            for i, t in enumerate(texts):
                pathlib.Path(d, f"e{i}.toml").write_text(t, encoding="utf-8")
            return generate.load(pathlib.Path(d))

    def test_valid_entry(self):
        (e,) = self.load(GOOD)
        self.assertEqual(e["id"], "e0")
        html = generate.page("AnySWF", [e], "now")
        self.assertIn("SWF 8 AS2", html)
        self.assertIn("pc, psp-ppsspp", html)

    def test_rejections(self):
        bad = [
            GOOD.replace('stage = "perfect"', 'stage = "great"'),
            GOOD.replace('kind = "test-suite"', 'kind = "commercial"'),
            GOOD.replace('kind = "test-suite"', 'kind = "game"'),  # a game must not name a swf
            GOOD.replace('"psp-ppsspp"', '"ps2"'),
            GOOD.replace('"AS2"', '"AS3"'),
            GOOD.replace("swf_version = 8", 'swf_version = "8"'),
            GOOD + 'swf_sha256 = "abc"\n',
            GOOD + "unimplemented = [1]\n",
            GOOD.replace('title = "Demo"\n', ""),
        ]
        for text in bad:
            with self.subTest(text=text), self.assertRaises(generate.CompatError):
                self.load(text)
        with self.assertRaises(generate.CompatError):
            self.load()

    def test_repository_entries_are_valid(self):
        root = pathlib.Path(__file__).resolve().parents[2]
        self.assertTrue(generate.load(root / "compat"))


if __name__ == "__main__":
    unittest.main()
