"""No network, tool execution, game or installation. Strict transaction fixtures."""
import copy
import hashlib
import io
import json
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import managed_tools as tools

ROOT = Path(__file__).resolve().parents[2] / "build/managed-tests"
ROOT.mkdir(parents=True, exist_ok=True)


def archive(entries):
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w") as z:
        for name, data in entries:
            if isinstance(name, str):
                raw = zipfile.ZipInfo(name)
                raw.filename = name
                name = raw
            z.writestr(name, data)
    return output.getvalue()


class Managed(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / "private-data"
        self.source = self.root / "source"
        (self.source / "tools").mkdir(parents=True)

    def fixture(self, content=b"synthetic interpreter never executed"):
        payload = archive([("python.exe", content), ("LICENSE.txt", b"fixture notice")])
        item = {"id": "python", "version": "fixture", "url": "https://www.python.org/fixture.zip",
                "sha256": hashlib.sha256(payload).hexdigest(), "max_download_bytes": len(payload),
                "max_unpacked_bytes": 10000, "max_entries": 20, "archive_root": "",
                "required": ["python.exe"], "license": "fixture only",
                "license_source": "https://www.python.org/", "metadata_source": "https://www.python.org/"}
        value = {"schema": 1, "owner": tools.OWNER, "abi": "fixture-v1", "artifacts": [item]}
        def fetch(spec, target, progress, cancelled):
            Path(target).write_bytes(payload)
        return value, fetch

    def install(self, value=None, fetch=None, **kwargs):
        if value is None:
            value, fetch = self.fixture()
        return tools.install(self.data, self.source, value, fetch=fetch, validate=lambda p: None, **kwargs)

    def test_pins_have_official_hashes_bounds_and_explicit_versions(self):
        value = tools.manifest()
        pins = {item["id"]: item for item in value["artifacts"]}
        self.assertEqual(len(pins), 8)
        self.assertEqual(pins["cmake"]["version"], "3.31.12")
        self.assertEqual(pins["ninja"]["version"], "1.13.2")
        self.assertEqual(pins["python"]["version"], "3.13.16")
        self.assertIn("ucrt", pins["llvm"]["version"])
        self.assertLess(sum(p["max_download_bytes"] for p in pins.values()), 270_000_000)

    def test_transaction_reuse_hashes_and_notices(self):
        value, fetch = self.fixture()
        result = self.install(value, fetch)
        self.assertTrue(tools.complete(result, tools.identity(value, self.source)))
        self.assertEqual((result / "output/python/LICENSE.txt").read_bytes(), b"fixture notice")
        result2 = self.install(value, lambda *a:self.fail("must reuse verified archive/output"))
        self.assertEqual(result, result2)

    def test_tampered_output_is_not_reused_and_preserved_for_inspection(self):
        value, fetch = self.fixture()
        result = self.install(value, fetch)
        (result / "output/python/python.exe").write_bytes(b"tampered")
        self.install(value, lambda *a:self.fail("verified download cache should be reused"))
        self.assertTrue(tools.complete(result, tools.identity(value, self.source)))
        self.assertEqual(len(list(result.parent.glob(result.name + ".invalid-*"))), 1)

    def test_checksum_failure_never_promotes_and_previous_stays_verified(self):
        good, fetch = self.fixture()
        previous = self.install(good, fetch)
        changed, _ = self.fixture(b"new fixture")
        def invalid(spec, target, *args):
            Path(target).write_bytes(b"wrong bytes")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.install(changed, invalid)
        self.assertTrue(tools.complete(previous, tools.identity(good, self.source)))
        self.assertFalse((previous.parent / tools.identity(changed, self.source)[:24]).exists())

    def test_cancel_before_promotion_preserves_previous(self):
        good, fetch = self.fixture()
        previous = self.install(good, fetch)
        changed, new_fetch = self.fixture(b"new fixture")
        done = []
        def validate(output):
            done.append(True)
        with self.assertRaises(InterruptedError):
            tools.install(self.data, self.source, changed, fetch=new_fetch,
                          validate=validate, cancelled=lambda:bool(done))
        self.assertTrue(tools.complete(previous, tools.identity(good, self.source)))
        self.assertFalse((previous.parent / tools.identity(changed, self.source)[:24]).exists())

    def test_probe_failure_cannot_certify_tools(self):
        value, fetch = self.fixture()
        def invalid(_):
            raise RuntimeError("synthetic import failure")
        with self.assertRaisesRegex(RuntimeError, "import failure"):
            tools.install(self.data, self.source, value, fetch=fetch, validate=invalid)
        self.assertFalse((self.data / "managed-tools" / tools.identity(value, self.source)[:24]).exists())

    def test_bad_members_preflight_before_any_extraction(self):
        names = ["../outside", "/absolute", "C:/drive", "x\\y", "x:stream", "CON.txt",
                 "folder./x", "folder /x", "x/../y", "a//b", "a?b", "nul"]
        value, _ = self.fixture()
        item = value["artifacts"][0]
        for number, name in enumerate(names):
            with self.subTest(name=name):
                file = self.root / ("bad" + str(number) + ".zip")
                file.write_bytes(archive([(name, b"x")]))
                target = self.root / ("out" + str(number))
                with self.assertRaises(ValueError):
                    tools.unpack(file, target, item)
                self.assertFalse(target.exists())

    def test_case_parent_file_duplicate_and_symlink_refused(self):
        value, _ = self.fixture()
        for entries in [[("A", b"x"), ("a", b"y")], [("Dir/a", b"x"), ("dir/b", b"y")],
                        [("x", b"x"), ("x/y", b"y")]]:
            file = self.root / "collision.zip"
            file.write_bytes(archive(entries))
            with self.assertRaises(ValueError):
                tools.inspect_zip(file, value["artifacts"][0])
        link = zipfile.ZipInfo("link")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        file.write_bytes(archive([(link, b"../outside")]))
        with self.assertRaises(ValueError):
            tools.inspect_zip(file, value["artifacts"][0])

    def test_entry_and_unpacked_budgets(self):
        value, _ = self.fixture()
        file = self.root / "budget.zip"
        file.write_bytes(archive([("a", b"a"*100), ("b", b"b")]))
        item = copy.deepcopy(value["artifacts"][0])
        item["max_unpacked_bytes"] = 50
        with self.assertRaises(ValueError):
            tools.inspect_zip(file, item)
        item["max_unpacked_bytes"] = 1000
        item["max_entries"] = 1
        with self.assertRaises(ValueError):
            tools.inspect_zip(file, item)

    def test_url_and_environment_do_not_inherit_sdk(self):
        for url in ("http://www.python.org/a", "https://evil.invalid/a", "https://user@github.com/a",
                    "https://github.com:444/a"):
            with self.assertRaises(ValueError):
                tools.check_url(url)
        import os
        from unittest.mock import patch
        with patch.dict(os.environ, {"INCLUDE":"untrusted SDK", "LIB":"untrusted libs", "CL":"override",
                                     "HW_VCVARS":"override", "PATH":"untrusted"}):
            env = tools.environment(self.root)
        self.assertNotIn("INCLUDE", env)
        self.assertNotIn("LIB", env)
        self.assertNotIn("CL", env)
        self.assertNotIn("HW_VCVARS", env)
        self.assertNotIn("untrusted", env["PATH"])
        self.assertIn("x86_64", str(tools.paths(self.root)["cc"]))


if __name__ == "__main__":
    unittest.main()
