"""Build with clang-cl on Windows or Clang on Linux; serialize across worktrees."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_lock import build_lock

ROOT = str(Path(__file__).resolve().parent.parent)


def vs_tool(sub, exe, env):
    import msvc
    install = msvc.vs_install()
    if install:
        candidate = os.path.join(install, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", sub, exe)
        if os.path.exists(candidate):
            return candidate
    found = shutil.which(exe, path=env["PATH"])
    if not found:
        raise RuntimeError(f"{exe} not found: install the VS C++ CMake tools component")
    return found


def toolchain(managed=None, temporary=None):
    if managed:
        import managed_tools
        value = managed_tools.manifest()
        key = managed_tools.identity(value, ROOT)
        if not managed_tools.complete(managed, key):
            raise RuntimeError("Private toolchain certificate is missing or changed; run setup tools verification")
        selected = managed_tools.paths(managed, value)
        for path in selected.values():
            if not path.is_file():
                raise RuntimeError("Private toolchain is incomplete: " + str(path))
        return (managed_tools.environment(managed, value, temporary), str(selected["cmake"]), str(selected["ninja"]),
                str(selected["cc"]), str(selected["cxx"]))
    if os.name == "nt":
        import msvc
        env = msvc.vs_env()
        return env, vs_tool(os.path.join("CMake", "bin"), "cmake.exe", env), vs_tool("Ninja", "ninja.exe", env), "clang-cl", "clang-cl"
    if not sys.platform.startswith("linux"):
        raise RuntimeError("Only Windows x64 and experimental Linux x86-64 are supported")
    env = dict(os.environ)
    names = ["cmake", "ninja", env.get("CC", "clang"), env.get("CXX", "clang++")]
    found = [shutil.which(name) for name in names]
    missing = [name for name, path in zip(names, found) if not path]
    if missing:
        raise RuntimeError("Missing build prerequisites: " + ", ".join(missing))
    return env, *found


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--only-file")
    ap.add_argument("--target", default=None)
    ap.add_argument("--build-dir", help="isolated build directory")
    ap.add_argument("--generated-dir", help="user-local generated C directory")
    ap.add_argument("--managed-toolchain", type=Path, help="verified private LLVM-MinGW toolset directory")
    ap.add_argument("--managed-temp-dir", type=Path, help="owned private compiler/test temporary directory")
    modes = ap.add_mutually_exclusive_group()
    modes.add_argument("--synthetic-tests", action="store_true", help="configure public tests without game inputs")
    modes.add_argument("--host-tests", action="store_true", help="configure only portable host contract tests")
    modes.add_argument("--game-only", action="store_true", help="compile generated game library without host runtime")
    modes.add_argument("--setup-ui", action="store_true", help="build only the code-free native Windows setup window")
    modes.add_argument("--managed-probe", action="store_true", help="public Windows GNU-ABI compatibility contracts; no game inputs")
    modes.add_argument("--managed-runtime-link", action="store_true", help="link public runtime/SDL with reviewed synthetic guest fixture; NEVER run")
    a = ap.parse_args()
    if a.jobs < 1:
        ap.error("--jobs must be at least 1")
    if a.only_file and (a.synthetic_tests or a.host_tests or a.game_only or a.setup_ui or a.managed_probe or a.managed_runtime_link):
        ap.error("--only-file cannot be combined with a test/library mode")
    if a.host_tests and not (Path(ROOT) / "tools/tests/host.cmake").is_file():
        ap.error("--host-tests requires the separate, unmerged Linux host-test candidate; use --synthetic-tests on Windows")
    if a.managed_probe and not a.managed_toolchain:
        ap.error("--managed-probe requires a certified --managed-toolchain")
    if a.managed_runtime_link and (not a.managed_toolchain or not a.generated_dir):
        ap.error("--managed-runtime-link requires private tools and a public fixture directory")
    # Single-file probes and complete builds share the same host reservation.
    with build_lock():
        return build(a)


def build(a):
    mode = "runtime-link-NOT-A-GAME" if getattr(a,"managed_runtime_link",False) else "probe" if getattr(a, "managed_probe", False) else "setup-ui" if a.setup_ui else "synthetic" if a.synthetic_tests else "host" if a.host_tests else "game" if a.game_only else "release"
    managed = getattr(a, "managed_toolchain", None)
    bdir = os.path.abspath(a.build_dir or os.path.join(ROOT, "build", ("managed-" if managed else "") + mode))
    generated = os.path.abspath(a.generated_dir or os.path.join(ROOT, "generated"))
    if getattr(a,"managed_runtime_link",False):
        sys.path.insert(0,str(Path(ROOT)/"tools/tests"))
        import public_runtime_fixture
        public_runtime_fixture.verify(generated)
    os.makedirs(bdir, exist_ok=True)
    env, cmake, ninja, cc, cxx = toolchain(managed, getattr(a, "managed_temp_dir", None))
    if a.only_file:
        cpp = a.only_file.endswith((".cpp", ".cc", ".cxx"))
        includes = [os.path.join(ROOT, "runtime", "include"), generated, os.path.join(ROOT, "runtime", "src")]
        includes += [os.path.join(ROOT, "third_party", name, "include") for name in ("vulkan-headers", "spirv-headers")]
        if os.name == "nt" and not managed:
            args = [cc, "/nologo", "/c", "/O2", "/arch:AVX2", "-mfma", "-Wno-everything", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN"]
            args += ["/std:c++20", "/EHsc"] if cpp else []
            args += [f"/I{path}" for path in includes]
            args += [a.only_file, "/Fo" + os.path.join(bdir, "probe.obj")]
        else:
            args = [cxx if cpp else cc, "-c", "-O2", "-mavx2", "-mfma", "-std=c++20" if cpp else "-std=c11"]
            if managed:
                args += ["-mf16c", "-maes", "-mpclmul", "-msse4.2", "-mcx16",
                         "-fno-strict-aliasing",
                         "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN", "-D_WIN32_WINNT=0x0A00",
                         "-DWINVER=0x0A00", "-DNTDDI_VERSION=0x0A000000"]
            args += [f"-I{path}" for path in includes]
            args += [a.only_file, "-o", os.path.join(bdir, "probe.o")]
        return subprocess.run(args, cwd=ROOT, env=env).returncode
    # Always configure: mode and explicit generated-dir changes must reach CMake.
    args = [cmake, "-S", ROOT, "-B", bdir, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}",
            "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_CXX_COMPILER={cxx}",
            f"-DGEN_DIR={generated}", f"-DHWDER_SYNTHETIC_TESTS_ONLY={'ON' if a.synthetic_tests else 'OFF'}",
            f"-DHWDER_HOST_TESTS_ONLY={'ON' if a.host_tests else 'OFF'}", f"-DHWDER_GAME_ONLY={'ON' if a.game_only else 'OFF'}"]
    args += [f"-DHWDER_SETUP_UI_ONLY={'ON' if a.setup_ui else 'OFF'}"]
    args += [f"-DHWDER_MANAGED_PROBE_ONLY={'ON' if getattr(a, 'managed_probe', False) else 'OFF'}"]
    args += [f"-DHWDER_PUBLIC_RUNTIME_LINK_ONLY={'ON' if getattr(a,'managed_runtime_link',False) else 'OFF'}"]
    if managed:
        import managed_tools
        args += [f"-DPython3_EXECUTABLE={managed_tools.paths(managed)['python']}",
                 "-DPython3_FIND_REGISTRY=NEVER"]
    result = subprocess.run(args, env=env)
    if result.returncode:
        return result.returncode
    target = a.target or ("managed_compatibility" if getattr(a, "managed_probe", False) else "hwder_setup" if a.setup_ui else "synthetic_tests" if a.synthetic_tests else "host_platform_test" if a.host_tests else "hwder_game" if a.game_only else "hwder")
    return subprocess.run([ninja, "-C", bdir, "-j", str(a.jobs), target], env=env).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as error:
        print(f"build.py: {error}", file=sys.stderr)
        sys.exit(1)
