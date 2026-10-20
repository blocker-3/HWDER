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
        self.assertEqual(len(record["guest_bindings"]),23)
        self.assertEqual(fixture.verify(self.output),record)
        c=(self.output/"rc_public_link_only.c").read_text()
        self.assertEqual(c.count("void f_"),23)
        self.assertNotIn("hw_lookup",c)
        self.assertIn("hw_func_count=0",(self.output/"functable.c").read_text())
        self.assertIn("hw_module_count=0",(self.output/"module_info.c").read_text())
        self.assertFalse((self.output/"complete.json").exists())
        with self.assertRaises(FileExistsError):fixture.create(self.output)

    def test_added_or_modified_stub_refused(self):
        fixture.create(self.output)
        (self.output/"host-stub.c").write_text("void host_fake(void) {}")
        with self.assertRaisesRegex(ValueError,"Extra fixture"):
            fixture.verify(self.output)
        (self.output/"host-stub.c").unlink()
        (self.output/"rc_public_link_only.c").write_text("void host_fake(void) {}")
        with self.assertRaisesRegex(ValueError,"identity changed"):
            fixture.verify(self.output)

    def test_no_outside_output_and_no_new_guest_dependency_without_review(self):
        with self.assertRaises(ValueError):fixture.directory(fixture.ROOT/"generated")
        source=Path(self.temp.name)/"source/runtime/src"
        source.mkdir(parents=True)
        (source/"test.cpp").write_text('extern "C" void f_1000000000(Ctx* c);')
        with self.assertRaisesRegex(ValueError,"allowlist changed"):
            fixture.public_bindings(source.parents[1])


if __name__=="__main__":unittest.main()
