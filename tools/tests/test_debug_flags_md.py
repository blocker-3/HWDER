"""The debug-switch table in docs/environment-variables.md must match the table in runtime/src/debug_flags.cpp."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gen_debug_flags_md as gen


class DebugFlagsDoc(unittest.TestCase):
    def test_table_parses(self):
        entries = gen.parse()
        names = [e[1] for e in entries]
        self.assertGreater(len(entries), 50)
        self.assertEqual(names, sorted(names), "debug_flags.cpp table is sorted by variable name")
        self.assertEqual(len(set(names)), len(names), "no duplicate variables")
        self.assertTrue(all(n.startswith("HWDER_") for n in names))
        self.assertTrue(all(e[2] for e in entries), "every switch has a meaning")

    def test_doc_section_is_current(self):
        text = gen.DOC.read_text(encoding="utf-8")
        section = gen.doc_section(text)
        self.assertIsNotNone(section, "docs/environment-variables.md lacks the debug-flags:begin/end markers")
        self.assertEqual(section, gen.generate(),
                         "docs/environment-variables.md debug-switch table is stale: run `python tools/gen_debug_flags_md.py --write`")

    def test_escaped_quotes_survive(self):
        meanings = {e[1]: e[2] for e in gen.parse()}
        self.assertIn('"1"', meanings["HWDER_CACHE_SAVE_SYNC"])


if __name__ == "__main__":
    unittest.main()
