"""Fetch third-party sources into third_party/ (see third_party/README.md).

In a git checkout prefer `git submodule update --init --recursive`; this script is for source
archives without submodule metadata.
"""
import os
import subprocess

TP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "third_party")

# (name, url, ref): ref pins a tag for the shallow clone, None = default branch.
DEPS = [
    ("SDL", "https://github.com/libsdl-org/SDL.git", "release-3.2.30"),
    ("imgui", "https://github.com/ocornut/imgui.git", None),
    ("spirv-headers", "https://github.com/KhronosGroup/SPIRV-Headers.git", None),
    ("vulkan-headers", "https://github.com/KhronosGroup/Vulkan-Headers.git", None),
]


def main():
    os.makedirs(TP, exist_ok=True)
    for name, url, ref in DEPS:
        dst = os.path.join(TP, name)
        if os.path.isdir(dst) and os.listdir(dst):
            continue
        branch = ["--branch", ref] if ref else []
        subprocess.run(["git", "clone", "--depth", "1"] + branch + [url, name], cwd=TP, check=True)


if __name__ == "__main__":
    main()
