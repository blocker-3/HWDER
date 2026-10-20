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

    def write_kit(self):
        with zipfile.ZipFile(self.kit,"w") as z:
            for name,data in self.files.items():z.writestr("kit/"+name,data)
            z.writestr("kit/source-manifest.json",json.dumps({n:dist.digest(b) for n,b in self.files.items()}))

    def package(self):
        return dist.assemble(self.kit,self.exe,self.certificate,self.root,self.root/"release.zip",lambda p:self.sources)

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
