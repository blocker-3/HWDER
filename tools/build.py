"""Configure + build HWDER with clang-cl and Ninja inside the VS developer environment.

    python tools/build.py [--jobs N] [--target hwder] [--only-file generated/rc_000.c]
"""
import argparse
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msvc  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def vs_tool(sub, exe, env):
    """CMake/Ninja bundled with the located Visual Studio, else whichever the VS environment's PATH has."""
    if msvc.vs_install():
        cand = os.path.join(msvc.vs_install(), "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", sub, exe)
        if os.path.exists(cand):
            return cand
    found = shutil.which(exe, path=env["PATH"])
    if not found:
        sys.exit(f"{exe} not found: install the VS 'C++ CMake tools for Windows' component")
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--only-file")
    ap.add_argument("--target", default="hwder")
    ap.add_argument("--build-dir", help="isolated build directory (default: build/release)")
    ap.add_argument("--generated-dir", help="isolated user-local generated C directory")
    a = ap.parse_args()
    if a.jobs < 1:
        ap.error("--jobs must be at least 1")
    env = msvc.vs_env()
    if a.only_file:
        inc = os.path.join(ROOT, "runtime", "include")
        tp = os.path.join(ROOT, "third_party")
        cxx = a.only_file.endswith(".cpp")
        extra = (["/std:c++20", "/EHsc", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", f"/I{os.path.join(ROOT, 'runtime', 'src')}",
                  f"/I{os.path.join(tp, 'vulkan-headers', 'include')}", f"/I{os.path.join(tp, 'spirv-headers', 'include')}"]
                 if cxx else [])
        obj = os.path.join(ROOT, "build", "probe_" + os.path.basename(a.only_file) + ".obj")
        r = msvc.run(["clang-cl", "/nologo", "/c", "/O2", "/arch:AVX2", "-mfma", "-Wno-everything",
                      f"/I{inc}", f"/I{os.path.join(ROOT, 'generated')}"] + extra + [a.only_file,
                      f"/Fo{obj}"], cwd=ROOT)
        print(r.stdout[-8000:], r.stderr[-4000:])
        sys.exit(r.returncode)
    bdir = os.path.abspath(a.build_dir or os.path.join(ROOT, "build", "release"))
    os.makedirs(bdir, exist_ok=True)
    # Several agents may build at once, possibly from different worktrees: serialize on one
    # machine-wide lock file so their compilers don't compete for the same cores and memory.
    import msvcrt
    import time
    lockdir = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "hwder")
    os.makedirs(lockdir, exist_ok=True)
    lockf = open(os.path.join(lockdir, "build.lock"), "a+")
    lockf.seek(0)
    waited = False
    while True:
        try:
            msvcrt.locking(lockf.fileno(), msvcrt.LK_NBLCK, 1)
            break
        except OSError:
            if not waited:
                print(f"build.py: waiting for another build to finish ({lockf.name})", flush=True)
                waited = True
            time.sleep(1)
    cmake = vs_tool(os.path.join("CMake", "bin"), "cmake.exe", env)
    ninja = vs_tool("Ninja", "ninja.exe", env)
    # Reconfigure explicit first-run builds on every resume; CMake checks its existing cache.
    if a.generated_dir or not os.path.exists(os.path.join(bdir, "build.ninja")):
        gen_arg = [f"-DGEN_DIR={os.path.abspath(a.generated_dir)}"] if a.generated_dir else []
        r = subprocess.run([cmake, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DCMAKE_BUILD_TYPE=Release",
                            "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl"] + gen_arg + [ROOT],
                           cwd=bdir, env=env)
        if r.returncode:
            sys.exit(r.returncode)
    r = subprocess.run([ninja, "-j", str(a.jobs), a.target], cwd=bdir, env=env)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
