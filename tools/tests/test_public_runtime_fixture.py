"""Source-only fixture boundaries. No compiler or output executable runs."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import public_runtime_fixture as fixture

ROOT=fixture.ROOT/"build/private-toolchain-data/probes/fixture-tests"
ROOT.mkdir(parents=True,exist_ok=True)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.output=Path(self.temp.name)/"generated-PUBLIC-NOT-A-GAME"

    def test_exact_guest_allowlist_empty_metadata_and_no_real_ready_marker(self):
        record=fixture.create(self.output)
        self.assertEqual(len(record["guest_bindings"]),12)
        self.assertEqual(fixture.verify(self.output),record)
        c=(self.output/"rc_public_link_only.c").read_text()
        self.assertEqual(c.count("void f_"),12)
        self.assertNotIn("hw_lookup",c)
        self.assertIn("hw_func_count=0",(self.output/"functable.c").read_text())
        self.assertIn("hw_module_count=0",(self.output/"module_info.c").read_text())
        self.assertIn("hw_symbol_count=0",(self.output/"functable.c").read_text())
        self.assertIn("#define f_Camera_BuildProj_orig f_100057cee0_orig",(self.output/"symbols.h").read_text())
        self.assertEqual(record["guest_bindings"]["f_100044660c_orig"],["runtime/src/game/stage_props.cpp"])
        self.assertFalse((self.output/"complete.json").exists())
        with self.assertRaises(FileExistsError):fixture.create(self.output)

    def test_added_or_modified_stub_refused(self):
        fixture.create(self.output)
        (self.output/"host-stub.c").write_text("void host_fake(void) {}")
        with self.assertRaisesRegex(ValueError,"Extra fixture"):
            fixture.verify(self.output)
        (self.output/"host-stub.c").unlink()
        (self.output/"rc_public_link_only.c").write_text("void host_fake(void) {}")
        with self.assertRaisesRegex(ValueError,"public guest schema"):
            fixture.verify(self.output)
        # Recomputing an untrusted sidecar cannot authorize host stubs either.
        marker=self.output/"public-link-fixture.json"
        record=json.loads(marker.read_text());record["files"]["rc_public_link_only.c"]=fixture.sha(self.output/"rc_public_link_only.c")
        marker.write_text(json.dumps(record))
        with self.assertRaisesRegex(ValueError,"public guest schema"):
            fixture.verify(self.output)

    def test_no_outside_output_and_no_new_guest_dependency_without_review(self):
        with self.assertRaises(ValueError):fixture.directory(fixture.ROOT/"generated")
        source=Path(self.temp.name)/"source/runtime/src"
        source.mkdir(parents=True)
        (source/"test.cpp").write_text('extern "C" void f_1000000000(Ctx* c);')
        with self.assertRaisesRegex(ValueError,"allowlist changed"):
            fixture.public_bindings(source.parents[1])
        # A named guest (game/hook.h) that is not reviewed is refused too.
        (source/"test.cpp").write_text("HW_GUEST(Scene_Draw);")
        (source.parents[1]/"tools/recomp").mkdir(parents=True)
        (source.parents[1]/"tools/recomp/symbols.txt").write_text("0x1000000004  Scene_Draw  func\n")
        with self.assertRaisesRegex(ValueError,"allowlist changed"):
            fixture.public_bindings(source.parents[1])


if __name__=="__main__":unittest.main()
