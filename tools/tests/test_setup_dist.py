"""Synthetic code-free package boundary tests, never a real executable/game."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import make_setup_dist as dist
import make_dist

ROOT=Path(__file__).resolve().parents[2]/"build/ui-package-tests"
ROOT.mkdir(parents=True,exist_ok=True)


class Package(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.files={"README.md":b"public source"}
        for sub in make_dist.SUBMODULES:
            self.files["third_party/"+sub+"/LICENSE"]=b"retained upstream license fixture"
        self.kit=self.root/"base.zip"
        self.write_kit()
        self.sources=["CMakeLists.txt","tools/build.py","tools/setup_ui/bootstrap.cpp"]
        for n in self.sources:
            p=self.root/n;p.parent.mkdir(parents=True,exist_ok=True);p.write_text("public bootstrap source")
        self.exe=self.root/"bootstrap.exe";self.exe.write_bytes(b"MZsynthetic code-free fixture; never execute")
        self.certificate=self.root/"certificate.json"
        self.cert={"kind":"HWDER-codefree-setup-v1","exe_sha256":dist.digest(self.exe.read_bytes()),
            "source_sha256":{n:dist.digest((self.root/n).read_bytes()) for n in self.sources},
            "no_game_linkage":True,"system_imports_only":True}
        self.certificate.write_text(json.dumps(self.cert))
        self.baseline="a"*40
        (self.root/dist.BASELINE).write_text(self.baseline+chr(10))
        self.contains=lambda root,commit:commit==self.baseline

    def write_kit(self):
        with zipfile.ZipFile(self.kit,"w") as z:
            for name,data in self.files.items():z.writestr("kit/"+name,data)
            z.writestr("kit/source-manifest.json",json.dumps({n:dist.digest(b) for n,b in self.files.items()}))

    def package(self):
        return dist.assemble(self.kit,self.exe,self.certificate,self.root,self.root/"release.zip",lambda p:self.sources,self.contains)

    def test_stale_source_without_main_baseline_refused(self):
        self.contains=lambda root,commit:False
        with self.assertRaisesRegex(ValueError,"Stale kit source"):self.package()
        self.assertFalse((self.root/"release.zip").exists())

    def test_baseline_file_must_name_one_full_commit(self):
        (self.root/dist.BASELINE).write_text("f9e061a"+chr(10))
        with self.assertRaisesRegex(ValueError,"kit_baseline"):self.package()
        (self.root/dist.BASELINE).unlink()
        with self.assertRaises(FileNotFoundError):self.package()

    def test_baseline_recorded_and_untracked_project_files_dropped(self):
        self.files["runtime/src/interp/frame_probe.cpp"]=b"removed on main";self.files["docs/old.md"]=b"removed on main"
        self.files["third_party/SDL/src/keep.c"]=b"pinned";self.write_kit()
        with zipfile.ZipFile(self.package()) as z:
            names=z.namelist()
            self.assertNotIn("source/runtime/src/interp/frame_probe.cpp",names)
            self.assertNotIn("source/docs/old.md",names)
            self.assertIn("source/third_party/SDL/src/keep.c",names)
            self.assertIn("source/CMakeLists.txt",names)
            self.assertNotIn("source/README.md",names)  # in the base kit, not tracked by this fixture
            self.assertEqual(json.loads(z.read("setup-manifest.json"))["baseline"],self.baseline)
            self.assertNotIn("source/runtime/src/interp/frame_probe.cpp",json.loads(z.read("source/source-manifest.json")))

    def test_exact_one_binary_and_notices_hashes_reopened(self):
        p=self.package()
        with zipfile.ZipFile(p) as z:
            self.assertEqual([n for n in z.namelist() if n.endswith(".exe")],["HWDER Setup.exe"])
            manifest=json.loads(z.read("setup-manifest.json"))
            for name,sha in manifest["files"].items():self.assertEqual(dist.digest(z.read(name)),sha)
        with self.assertRaises(FileExistsError):self.package()

    def test_only_exact_public_bootstrap_script_is_overlaid(self):
        script="tools/setup_ui/install_private_python.ps1"
        for name in (script,"tools/arbitrary.ps1"):
            path=self.root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text("public fixture")
            self.sources.append(name)
        with zipfile.ZipFile(self.package()) as z:
            self.assertIn("source/"+script,z.namelist())
            self.assertNotIn("source/tools/arbitrary.ps1",z.namelist())

    def test_gnu_bootstrap_requires_exact_notice_and_link_map_certificates(self):
        self.cert["compiler_abi"]="llvm-mingw-ucrt-static-cxx-v1"
        self.certificate.write_text(json.dumps(self.cert))
        with self.assertRaisesRegex(ValueError,"runtime notices"):self.package()
        notices={}
        for name in dist.GNU_NOTICES:
            p=self.root/name;p.parent.mkdir(exist_ok=True);p.write_text("synthetic licensed runtime notice")
            notices[name]=dist.digest(p.read_bytes())
        self.cert["runtime_notices"]=notices
        (self.root/"setup-ui-link.map").write_text("synthetic linker provenance")
        self.cert["link_map_sha256"]=dist.digest((self.root/"setup-ui-link.map").read_bytes())
        self.certificate.write_text(json.dumps(self.cert))
        with zipfile.ZipFile(self.package()) as z:
            self.assertTrue(dist.GNU_NOTICES.issubset(z.namelist()))

    def test_game_executable_cannot_replace_certified_bootstrap(self):
        self.exe.write_bytes(b"MZsynthetic game-linked binary")
        with self.assertRaisesRegex(ValueError,"code-free"):self.package()

    def test_private_source_member_refused_even_with_matching_hash(self):
        self.files["generated/game.c"]=b"must remain private";self.write_kit()
        with self.assertRaisesRegex(ValueError,"Private"):self.package()

    def test_missing_third_party_notice_refused(self):
        del self.files["third_party/SDL/LICENSE"];self.write_kit()
        with self.assertRaisesRegex(ValueError,"notice"):self.package()

    def test_source_change_after_compile_refused(self):
        (self.root/self.sources[0]).write_text("changed source")
        with self.assertRaisesRegex(ValueError,"source changed"):self.package()


if __name__=="__main__":unittest.main()
