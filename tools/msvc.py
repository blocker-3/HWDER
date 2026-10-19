"""Run commands inside the Visual Studio x64 developer environment.

Paths are passed through environment variables (HW_*) so that non-ASCII characters
in the project path survive cmd.exe.
"""
import os
import subprocess
import tempfile

VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                       "Microsoft Visual Studio", "Installer", "vswhere.exe")
_install_cache = None
_env_cache = None


def vs_install():
    """Visual Studio installation directory with the x64 C++ tools (via vswhere), or None."""
    global _install_cache
    if _install_cache is None:
        path = ""
        if os.path.exists(VSWHERE):
            path = subprocess.run([VSWHERE, "-latest", "-products", "*", "-requires",
                                   "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                   "-property", "installationPath"], capture_output=True, text=True).stdout.strip()
        _install_cache = path.splitlines()[0] if path else ""
    return _install_cache or None


def vcvars():
    """vcvars64.bat: HW_VCVARS if set, otherwise from the vswhere-located install."""
    path = os.environ.get("HW_VCVARS")
    if not path and vs_install():
        path = os.path.join(vs_install(), "VC", "Auxiliary", "Build", "vcvars64.bat")
    if not path or not os.path.exists(path):
        raise RuntimeError("Visual Studio with the x64 C++ tools not found: install the 'Desktop development "
                           "with C++' workload (with clang-cl) or set HW_VCVARS to vcvars64.bat"
                           + (f" (tried {path})" if path else ""))
    return path


def vs_env():
    global _env_cache
    if _env_cache is None:
        out = subprocess.run(f'cmd /c ""{vcvars()}" >nul && set"', capture_output=True, text=True,
                             shell=False).stdout
        env = {}
        for line in out.splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                env[k] = v
        # Windows environment names are case-insensitive; cmd may emit "Path".
        # Consumers use PATH consistently, including subprocess tool discovery.
        for key in list(env):
            if key.upper() == "PATH" and key != "PATH":
                env["PATH"] = env.pop(key)
        if "INCLUDE" not in env:
            raise RuntimeError("failed to load Visual Studio environment")
        _env_cache = env
    return dict(_env_cache)


def run(args, cwd=None, **kw):
    """Run an argv list (no shell) with the VS environment."""
    env = vs_env()
    exe = args[0]
    for d in env["PATH"].split(";"):
        cand = os.path.join(d, exe if exe.endswith(".exe") else exe + ".exe")
        if os.path.exists(cand):
            args = [cand] + list(args[1:])
            break
    return subprocess.run(args, cwd=cwd, env=env, capture_output=True, text=True, **kw)
