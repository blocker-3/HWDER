"""Offline synthetic phase/count events only: no tools, network or real user Data."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import managed_tools as tools
import setup_bridge as bridge

ROOT=Path(__file__).resolve().parents[2]/"build/managed-progress-tests"
ROOT.mkdir(parents=True,exist_ok=True)


class Progress(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(dir=ROOT);self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name);self.source=self.root/"source";self.source.mkdir()
        self.data=self.root/"data"

    def fixture(self):
        archive=self.root/"fixture.zip"
        with zipfile.ZipFile(archive,"w") as z:
            z.writestr("python.exe",b"synthetic; never execute")
            z.writestr("LICENSE.txt",b"synthetic notice")
        item={"id":"python","sha256":hashlib.sha256(archive.read_bytes()).hexdigest(),
              "max_download_bytes":10000,"max_unpacked_bytes":10000,"max_entries":20,
              "archive_root":"","required":["python.exe"]}
        value={"abi":"fixture","artifacts":[item]}
        def fetch(spec,target,*args):Path(target).write_bytes(archive.read_bytes())
        return archive,item,value,fetch

    def test_monotonic_throttle_and_unconditional_final(self):
        now=[0.0];events=[]
        counter=tools.FileProgress(lambda n,done:events.append((n,done)),clock=lambda:now[0])
        counter.update(1);now[0]=0.1;counter.update(2)
        now[0]=0.49;counter.update(3);now[0]=0.5;counter.update(4)
        now[0]=0.6;counter.update(5);counter.update(5,True)
        self.assertEqual(events,[(1,False),(4,False),(5,True)])
        # Default-off callback cannot introduce clock work or alter old callers.
        tools.FileProgress(clock=lambda:self.fail("clock with callback disabled")).update(1)

    def test_hash_counts_match_real_files_and_no_completion_on_error(self):
        output=self.root/"output";output.mkdir()
        for name in ("one","two","three"):(output/name).write_text(name)
        events=[];result=tools.tool_hashes(output,lambda n,done:events.append((n,done)))
        self.assertEqual(len(result),3);self.assertEqual(events[-1],(3,True))
        events.clear()
        with patch.object(tools.first_run,"file_hash",side_effect=OSError("synthetic read failure")):
            with self.assertRaisesRegex(OSError,"synthetic read failure"):
                tools.tool_hashes(output,lambda n,done:events.append((n,done)))
        self.assertFalse(any(done for n,done in events))

    def test_extraction_counts_only_success_and_cancel_does_not_complete(self):
        archive,item,_,_=self.fixture();events=[]
        tools.unpack(archive,self.root/"unpacked",item,progress=lambda n,done:events.append((n,done)))
        self.assertEqual(events[-1],(2,True))
        events.clear();cancel=[False]
        def progress(n,done):events.append((n,done));cancel[0]=True
        with self.assertRaises(InterruptedError):
            tools.unpack(archive,self.root/"cancelled",item,cancelled=lambda:cancel[0],progress=progress)
        self.assertFalse(any(done for n,done in events))

    def test_install_phase_order_and_cached_reuse_checks_only_the_stamp(self):
        _,_,value,fetch=self.fixture();events=[]
        def phase(kind,**fields):events.append((kind,fields))
        def validate(path):events.append(("actual_validation",{}))
        installed=tools.install(self.data,self.source,value,fetch=fetch,validate=validate,phase=phase)
        names=[n for n,f in events]
        for earlier,later in zip(("archive_check","download_checksum","extract","packages","imports","actual_validation","imports_done","finalize"),
                                 ("download_checksum","extract","packages","imports","actual_validation","imports_done","finalize","installed_ready")):
            self.assertLess(names.index(earlier),names.index(later))
        self.assertNotIn("verify_new",names)  # archives are SHA-256 checked; the extracted files are not re-hashed
        events.clear()
        hashed=[]
        original=tools.tool_hashes
        with patch.object(tools,"tool_hashes",lambda *a,**k:hashed.append(1) or original(*a,**k)):
            self.assertEqual(tools.install(self.data,self.source,value,fetch=lambda *a:self.fail("cached fetch"),
                                           validate=lambda *a:self.fail("cached re-import"),phase=phase),installed)
        self.assertEqual(hashed,[])  # reuse: stamp only
        self.assertNotIn("extract",[n for n,f in events])
        self.assertIn("installed_ready",[n for n,f in events])

    def test_failed_import_has_no_finalize_or_success_event(self):
        _,_,value,fetch=self.fixture();events=[]
        def invalid(path):raise ValueError("synthetic import failure")
        with self.assertRaisesRegex(ValueError,"synthetic import failure"):
            tools.install(self.data,self.source,value,fetch=fetch,validate=invalid,
                          phase=lambda name,**f:events.append(name))
        self.assertIn("imports",events)
        for name in ("imports_done","verify_new","finalize","installed_ready"):self.assertNotIn(name,events)
        self.assertFalse((self.data/"managed-tools"/tools.identity(value)[:24]).exists())

    def test_main_nonce_bound_real_phase_fields_and_bridge_count_status(self):
        def install(*args,**kwargs):
            for kind,fields in (("imports",{}),("verify_new",{"files":42,"finished":False}),
                                ("verify_new",{"files":42,"finished":True}),("finalize",{})):
                kwargs["phase"](kind,**fields)
            return self.root/"synthetic-never-executed"
        output=io.StringIO();nonce="a"*32
        with patch.object(sys,"argv",["tools","--data",str(self.data),"--source",str(self.source),"--install","--nonce",nonce]), \
             patch.object(tools,"install",side_effect=install),contextlib.redirect_stdout(output):
            tools.main()
        prefix="HWDER_UI:"+nonce+":"
        events=[json.loads(line[len(prefix):]) for line in output.getvalue().splitlines() if line.startswith(prefix)]
        counted=[e for e in events if e.get("phase")=="verify_new"]
        self.assertEqual([e["files"] for e in counted],[42,42])
        self.assertTrue(all("%" not in e["message"] and "total" not in e for e in events))
        self.assertEqual(events[-1]["type"],"components_ready")
        output=io.StringIO()
        with patch.object(bridge,"NONCE",nonce),contextlib.redirect_stdout(output):
            bridge.stage_log("private-tools: Checking private tool files: 42 files hashed")
        event=json.loads(output.getvalue()[len(prefix):]);self.assertEqual(event["type"],"stage")
        self.assertIn("42 files hashed",event["message"])


if __name__=="__main__":unittest.main()
