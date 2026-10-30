"""R5 setup UX contracts: numbered steps and one-sentence failure recovery. No game, tools or network."""
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import setup_bridge as b

ROOT = Path(__file__).resolve().parents[2] / "build/ui-tests"
ROOT.mkdir(parents=True, exist_ok=True)


class Recovery(unittest.TestCase):
    def test_each_known_failure_maps_to_one_sentence_and_a_window_step(self):
        cases = {
            "Expected exactly one base-game program for title 0100AE00096EA000 or 01002AB007FD2000; "
            "updates/bundles unsupported": ("base game", "inputs"),
            "Japanese version (title 01002AB007FD2000) is not supported yet: main build ID 00, SHA-256 00 does not "
            "match a tested version. No code generated": ("Japanese version", "inputs"),
            "Unsupported ExeFS module set": ("supported game version", "inputs"),
            "Cannot decrypt NCA header; check your own dump and keys": ("prod.keys", "inputs"),
            "Required decryption key is missing; supply a complete own prod.keys and ticket": ("prod.keys", "inputs"),
            "Your game or key file moved. Pick it again, then Prepare.": ("moved", "inputs"),
            "Private tools are missing or changed. Choose Install private tools, then recheck.": ("Install", "install"),
            "Private tool certificate changed. Choose Install private tools.": ("Install", "install"),
            "Private toolchain certificate is missing or changed; run setup tools verification": ("Install", "install"),
            "Your game is running. Close it, then try again.": ("Close it", "close"),
            "No verified prepared game. Choose Prepare first.": ("Prepare", "prepare"),
            "Command '[...build.py...]' returned non-zero exit status 1.": ("build did not finish", "prepare"),
            "Cancelled before updating the playable version.": ("Cancelled", "prepare"),
            "Something nobody anticipated\nsecond line": ("Something nobody anticipated", "prepare"),
            "": ("unknown error", "prepare"),
        }
        for detail, (fragment, step) in cases.items():
            message, found = b.recovery(detail)
            self.assertIn(fragment, message, detail)
            self.assertEqual(found, step, detail)
            self.assertLess(len(message), 200, detail)
            self.assertTrue(message.endswith("."), message)
            self.assertNotIn("\n", message)

    def test_failed_event_carries_next_step_and_full_detail_stays_in_log(self):
        with tempfile.TemporaryDirectory(dir=ROOT) as temp, patch.object(sys, "argv", ["bridge", "--action", "status", "--data", temp]):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = b.main()
        self.assertEqual(code, 1)
        events = [json.loads(line) for line in output.getvalue().splitlines()]
        failed = [e for e in events if e["type"] == "failed"]
        self.assertEqual(len(failed), 1)
        self.assertEqual(failed[0]["next"], "prepare")
        self.assertEqual(failed[0]["message"], "No prepared game yet. Choose Prepare.")
        self.assertTrue(any(e["type"] == "log" and "No verified prepared game" in e["message"] for e in events))

    def test_stages_are_numbered_in_order_and_ninja_lines_stay_plain_logs(self):
        lines = ["prerequisites verified: ...", "validating NSP, own keys, title, module set and supported-version SHA-256",
                 "supported version: x; hashing complete NSP for cache identity", "exefs: starting (partial output is never considered complete)",
                 "generated: verified cache", "build: compiling complete native program via tools/build.py (shared machine build lock)",
                 "[12/1480] Building CXX object ...", "ready: C:\\x\\play\\hwder.exe"]
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            for line in lines:
                b.stage_log(line)
        events = [json.loads(line) for line in output.getvalue().splitlines()]
        stages = [e for e in events if e["type"] == "stage"]
        self.assertEqual([e["step"] for e in stages], [1, 2, 2, 3, 4, 5, 6])
        self.assertTrue(all(e["steps"] == 6 for e in stages))
        self.assertEqual(stages[0]["message"], "Step 1 of 6: Checking requirements")
        self.assertEqual(stages[-1]["stage"], "Finishing setup")
        self.assertEqual([e["message"] for e in events if e["type"] == "log"], lines)
        self.assertIsNone(b.stage_for("[12/1480] Building CXX object ..."))
        self.assertEqual(b.stage_for("private-tools: Checking private tool files: 5 files hashed"), None)

    def test_private_tool_stage_events_have_no_step_number(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            b.stage_log("private-tools: Checking private tool files: 5 files hashed")
        event = json.loads(output.getvalue())
        self.assertEqual(event["type"], "stage")
        self.assertEqual(event["stage"], "tools")
        self.assertNotIn("step", event)


if __name__ == "__main__":
    unittest.main()
