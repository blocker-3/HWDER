"""Command/certificate contracts only; no compiler or child process executes."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import build
import managed_tools

ROOT=Path(__file__).resolve().parents[2]


class ManagedBuild(unittest.TestCase):
    def args(self, **changes):
        values=dict(jobs=2,only_file=None,target=None,build_dir=None,generated_dir=None,
                    setup_ui=False,synthetic_tests=False,host_tests=False,game_only=False,
                    managed_toolchain=ROOT/"build/fake-private-tools",managed_probe=True)
        values.update(changes)
        return SimpleNamespace(**values)

    def test_uncertified_private_toolchain_refused_before_system_discovery(self):
        with patch.object(managed_tools,"complete",return_value=False), \
             patch.object(build,"vs_tool",side_effect=AssertionError("system tool lookup")):
            with self.assertRaisesRegex(RuntimeError,"certificate"):
                build.toolchain(ROOT/"build/not-certified")

    def test_managed_probe_private_python_and_separate_build_identity(self):
        calls=[]
        selected=managed_tools.paths(ROOT/"build/fake-private-tools")
        def run(argv,**kwargs):
            calls.append((argv,kwargs))
            return subprocess.CompletedProcess(argv,0)
        with patch.object(build,"toolchain",return_value=({"PATH":"private-only"},"private-cmake","private-ninja","private-cc","private-cxx")) as toolchain, \
             patch.object(build.subprocess,"run",side_effect=run):
            self.assertEqual(build.build(self.args(managed_temp_dir=ROOT/"build/private-data/probe/tmp")),0)
        toolchain.assert_called_once_with(ROOT/"build/fake-private-tools",ROOT/"build/private-data/probe/tmp")
        command=calls[0][0]
        self.assertIn(str(ROOT/"build/managed-probe"),command)
        self.assertIn("-DHWDER_MANAGED_PROBE_ONLY=ON",command)
        self.assertIn("-DPython3_FIND_REGISTRY=NEVER",command)
        self.assertIn("-DPython3_EXECUTABLE="+str(selected["python"]),command)
        self.assertEqual(calls[1][0][-1],"managed_compatibility")
        self.assertEqual(calls[0][1]["env"],{"PATH":"private-only"})

    @unittest.skipUnless(os.name == "nt", "the managed probe is the Windows LLVM-MinGW toolchain")
    def test_actual_production_probe_features_and_windows_defines(self):
        calls=[]
        def run(argv,**kwargs):
            calls.append(argv)
            return subprocess.CompletedProcess(argv,0)
        with patch.object(build,"toolchain",return_value=({},"cmake","ninja","cc","cxx")), \
             patch.object(build.subprocess,"run",side_effect=run):
            self.assertEqual(build.build(self.args(only_file="runtime/src/threads.cpp",managed_probe=False)),0)
        compile=next(c for c in calls if any(str(a).endswith("threads.cpp") for a in c))  # after the version.cmake stamp
        for flag in ("-mavx2","-mfma","-mf16c","-maes","-mpclmul","-msse4.2","-mcx16","-fno-strict-aliasing",
                     "-DNOMINMAX","-DWIN32_LEAN_AND_MEAN","-std=c++20",
                     "-D_WIN32_WINNT=0x0A00","-DWINVER=0x0A00","-DNTDDI_VERSION=0x0A000000"):
            self.assertIn(flag,compile)
        self.assertNotIn("/EHsc",compile)


if __name__=="__main__":
    unittest.main()
