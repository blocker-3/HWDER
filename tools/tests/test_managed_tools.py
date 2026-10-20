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
from unittest.mock import patch
import subprocess
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

    def test_check_prepare_launch_are_verify_only_and_forward_owned_protocol(self):
        for action in ("check","prepare","launch"):
            argv=["managed_tools","--data",str(self.data),"--source",str(self.source),
                  "--bridge-action",action,"--nonce","a"*32,"--nsp","own.nsp","--keys","own.keys"]
            with patch.object(sys,"argv",argv),patch.object(tools,"installed",return_value=self.root/"toolset"), \
                 patch.object(tools,"install",side_effect=AssertionError("must not install")), \
                 patch.object(tools,"environment",return_value={"PATH":"private"}), \
                 patch.object(tools.subprocess,"run",return_value=subprocess.CompletedProcess([],0)) as child:
                self.assertEqual(tools.main(),0)
            args=child.call_args.args[0]
            self.assertEqual(args[1:5],["-B","-X","utf8","-u"])
            self.assertEqual(args[args.index("--action")+1],action)
            self.assertEqual(args[args.index("--nonce")+1],"a"*32)
            self.assertIn("--managed-toolchain",args)
            self.assertEqual(child.call_args.kwargs["env"],{"PATH":"private"})

    def test_real_image_gate_rejects_host_dll_or_wrong_stack(self):
        import pe_dependencies
        record=dict(imports=["kernel32.dll","ucrtbase.dll"],delay_imports=[],stack_reserve=16777216,dynamic_base=False)
        with patch.object(pe_dependencies,"inspect",return_value=record):
            self.assertEqual(tools.game_image("never executed"),record)
        for changed in ({"imports":["vcruntime140.dll"]},{"stack_reserve":1048576},{"dynamic_base":True}):
            with patch.object(pe_dependencies,"inspect",return_value={**record,**changed}):
                with self.assertRaisesRegex(ValueError,"certification"):
                    tools.game_image("never executed")

    def test_relative_tool_paths_are_absolute_for_cmake_and_children(self):
        self.assertTrue(all(path.is_absolute() for path in tools.paths("build/private-tools").values()))

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

    def test_windows_long_paths_without_global_policy_changes(self):
        value, _ = self.fixture()
        relative = "/".join(["nested-" + str(i) + "-" + "a"*30 for i in range(7)]) + "/notice.txt"
        file = self.root / "long.zip"
        file.write_bytes(archive([(relative, b"retained long-path notice")]))
        item = copy.deepcopy(value["artifacts"][0])
        item["required"] = [relative]
        destination = self.root / "long-output/output"
        tools.unpack(file, destination, item)
        self.addCleanup(lambda:shutil.rmtree(tools.filesystem_path(tools.owned_path(destination, self.root))))
        self.assertGreater(len(str(destination / relative)), 300)
        self.assertEqual(tools.filesystem_path(destination / relative).read_bytes(), b"retained long-path notice")
        self.assertIn(relative, tools.tool_hashes(destination))
        self.assertEqual(tools.owned_path(tools.filesystem_path(destination), self.root), destination)
        tools.first_run.atomic_json(destination.parent / "complete.json",
            {"owner":tools.OWNER,"key":"long-fixture","files":tools.tool_hashes(destination)})
        self.assertTrue(tools.complete(destination.parent, "long-fixture"))
        with self.assertRaises(ValueError):
            tools.owned_path(tools.filesystem_path(self.root / "../outside"), self.root)

    def test_linear_tree_hashes_keep_every_file_and_reject_redirected_entries(self):
        from unittest.mock import patch
        from types import SimpleNamespace
        output = self.root / "hash-fixture"
        (output / "nested/deeper").mkdir(parents=True)
        (output / "top").write_bytes(b"one")
        (output / "nested/deeper/leaf").write_bytes(b"two")
        self.assertEqual(tools.tool_hashes(output), tools.first_run.tree_hashes(output))
        # Tools run with -B; unexpected bytecode is an added file, not ignored.
        (output / "extra.pyc").write_bytes(b"not trusted")
        self.assertIn("extra.pyc",tools.tool_hashes(output))
        class Entries:
            def __enter__(self):
                return iter([SimpleNamespace(stat=lambda **kw:SimpleNamespace(st_mode=stat.S_IFDIR,st_file_attributes=0x400))])
            def __exit__(self,*args):
                pass
        with patch.object(tools.os,"scandir",return_value=Entries()):
            with self.assertRaisesRegex(ValueError,"Redirected"):
                tools.tool_hashes(output)

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
        for name in ("TEMP","TMP","TMPDIR"):
            self.assertEqual(env[name], str(self.root/"tmp"))


if __name__ == "__main__":
    unittest.main()
