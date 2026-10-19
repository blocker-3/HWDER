# Experimental native Linux x86-64 source port

Status: implemented source candidate; native compilation and gameplay validation pending. Target: Bazzite on a ROG Ally, AVX2 x86-64 CPU and Vulkan1.3 graphics. A Windows regression build does not validate Linux. Do not describe this candidate as playable until the native checks and on-device tests below succeed.

## Source and dependencies

Use this candidate branch locally; initialize its pinned public dependencies with git submodule update --init --recursive. No linked game executable, generated C, game data, key file or private shader cache belongs in a source package. SDL/ImGui/header licenses remain with the pinned sources. See [dependency notices](../third_party/README.md).

In an existing native Linux development environment, provide Python3.10+ x64 (3.11+ for public synthetic tests), Clang/Clang++, CMake3.24+, Ninja, pkg-config, OpenSSL3 development files, shared FFmpeg libavcodec/libavutil development files with its native H.264 decoder, and SDL's Linux window/audio dependencies. SDL is built from pinned sources; video, audio, gamepads and dialogs are enabled. A usable Wayland or X11 backend and Vulkan loader/driver are required. Dependencies are not downloaded or installed by setup. This work does not install WSL or configure SSH/remote build services.

Optional Bazzite Desktop Mode example: use a Distrobox development container, then keep both build and run inside that same container so shared-library versions match. This creates a local container only when you run the commands; no environment was installed by this task. Bazzite supports Distrobox for development ([Bazzite docs](https://docs.bazzite.gg/Installing_and_Managing_Software/Distrobox/), [Distrobox create](https://distrobox.it/usage/distrobox-create/)).

~~~sh
distrobox create --name hwder-dev --image docker.io/library/ubuntu:24.04
distrobox enter hwder-dev
sudo apt-get update
sudo apt-get install build-essential git clang cmake ninja-build pkg-config python3-venv \
  libssl-dev libavcodec-dev libavutil-dev libvulkan1 mesa-vulkan-drivers vulkan-tools \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev liburing-dev
~~~

The SDL packages follow its [Linux dependency guidance](https://wiki.libsdl.org/SDL3/README-linux). Distribution FFmpeg builds may enable GPL components; record the actual library build/license. These commands are for private local testing and do not establish an LGPL-only redistribution configuration. The source kit includes no FFmpeg binaries.

From the candidate source directory, create/activate a Python environment and install the setup dependencies before any Python tests:

~~~sh
python3 -m venv "$HOME/.local/share/hwder/venv"
. "$HOME/.local/share/hwder/venv/bin/activate"
python3 -m pip install -r tools/requirements-setup.txt
~~~

Record the actual environment before a device test:

~~~sh
uname -m
cat /etc/os-release
printf '%s\n' "$XDG_SESSION_TYPE"
clang --version
cmake --version
pkg-config --modversion openssl libavcodec libavutil
~~~

Keep personal game/key paths private. Logs, shader recipes, generated source and compiled programs stay local.

## Public native checks (no game data or keys)

~~~sh
python3 tools/build.py --host-tests --jobs 4
ctest --test-dir build/host --output-on-failure
python3 tools/build.py --synthetic-tests --jobs 4
ctest --test-dir build/synthetic --output-on-failure
python3 -m unittest discover -s tools/tests -p 'test_*.py'
~~~

Host tests cover fixed guest-memory collision/commit/decommit/zero-recommit, events, dormant threads, guest TLS and kernel-lock release/restoration. Synthetic tests cover cache/catalog IO and recompiled A64 execution. POSIX fixtures cover private permissions, atomic replacement and partial-write preservation. Directory fsync failure after rename reports uncertain durability; replacement may already be visible. Windows execution does not exercise POSIX branches.

## Private local first run

After the public checks succeed, keep that same environment active, then:

~~~sh
python3 tools/first_run.py --check
bash play-local.sh --nsp "/private/path/own-game.nsp" --keys "/private/path/prod.keys" --jobs 4
~~~

These are source-candidate commands, not evidence of a successful Linux build. Setup validates the supported game version, extracts local ExeFS, generates C and compiles the complete runtime. Interrupted-build recovery, input quarantine, cache hashing and last-success rollback apply. Default private cache: $XDG_CACHE_HOME/hwder/local-build-v1, falling back to $HOME/.cache/hwder/local-build-v1. Its stable play directory contains the native hwder ELF executable, settings, saves and private caches. New cache roots are private. Generated outputs are never distributable artifacts. Use --cache only for a private user-owned directory. Identity includes target platform, source, compiler and dependency versions. ELF64/little-endian/x86-64 identity and execute mode are checked; publication preserves execute permissions.

## On-device acceptance

Begin at native60/default settings. Check startup/controller/menu navigation, game entry, audio/music, movies, save/reload, focus and fullscreen/window resizing. Linux refresh switching uses an exclusive fullscreen mode; windowed desktop refresh is unchanged. Capture Vulkan device/driver/session details and logs locally. Confirm saves survive a failed build and source update before a longer battle. Then test optional game-drawn full-object120, camera/object behavior, live toggling and frame pacing; handheld performance is an observation, not a promised target. Keep one live test at a time.

Controlled normal exits restore exclusive fullscreen modes before terminating. Fatal exits may remain abrupt. Linux crash signals leave an async-safe stderr message and defer to the system's local core-dump policy. Windows minidumps/register suspension, draw-write page census and suspended-thread matrix diagnostics are unavailable. Regular gameplay/full-object interpolation remains implemented. Textures use the existing hash fallback because Linux lacks Windows GetWriteWatch.

The exact Linux decoder class passed a public Windows FFmpeg probe (51 I/P/B pictures, 23,040 NV12 pixel checks), including prediction, reset, resolution changes and reused tags. This does not validate the Linux library/platform. No native Linux compiler, controller/GPU/media playback or real-save validation was available on the Windows development host. Successful native compilation, synthetic checks and Max's Ally playtest remain required before this milestone is complete.
