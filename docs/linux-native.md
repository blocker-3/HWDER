# Experimental native Linux x86-64 source port

Status (2026-10-24): the `linux-native` branch is main v0.2.2 (e7df07f) plus the 18 Linux port commits of 2026-10-19 and one follow-up that ports main's newer code (renderer split, debug-flag table, perf stats, multi-player input, save picker) to the host layer. On the Ally (Bazzite, Ubuntu 24.04 `hwder-dev` distrobox, Clang 18, RADV Phoenix / Mesa 25.2): host tests 7/7, synthetic tests 6/6, Python tests pass apart from the two public-runtime-fixture tests that also fail on main (its guest allowlist predates the game layer), and the complete runtime compiles and links against the link-check fixture below. The same branch still builds on Windows and its synthetic tests pass. A first-run build from the dump played through the 60 fps checks and 120 on the Ally (2026-10-24, Max). Target: Bazzite on a ROG Ally, AVX2 x86-64 CPU and Vulkan 1.3 graphics. Do not describe it as playable until the on-device tests below succeed.

### Link check without a game dump

`tools/tests/link_check_fixture.py` writes a stand-in generated directory (symbols.h from `tools/recomp/symbols.txt`, empty guest stubs, empty function and module tables). It proves the whole runtime compiles and links; the executable has no game modules and must never be run:

~~~sh
python3 tools/tests/link_check_fixture.py --output build/link-check-generated
python3 tools/build.py --generated-dir build/link-check-generated --build-dir build/link-check --jobs 4
~~~

### Known gaps (Windows features without a Linux equivalent yet)

- Texture/geometry write watch: done (8187ec6). Write-watched guest memory registers with userfaultfd async write-protect and `PAGEMAP_SCAN` reports written pages (kernel 6.7+; older kernels log "no write watch" and keep hashing). Max's Ally playtest 2026-10-24: all 60 fps checks good; at 120 busy areas dipped to ~75 before and ~100 after.
- `HWDER_THREAD_STATS`: done (cba01eb); the per-thread report reads `/proc/self/task` on Linux.
- Crash handling: no minidumps; Linux prints a signal message and leaves core dumps to the system.
- Hybrid-CPU P-core preference is Windows-only (the Z1 Extreme has no efficiency cores).
- Error dialogs: the "wrong game version" and save-copy failure dialogs are log-only on Linux; the missing-Vulkan dialog uses an SDL message box.
- The native Setup window, setup kits and Steam shortcut tooling are Windows-only; Linux uses `tools/first_run.py` / `play-local.sh`.
- XInput fallback is Windows-only; SDL handles every pad on Linux.
- The save picker's "choose zmha.bin" file mode and the 60 Hz monitor switch need checking on the device.

## Source and dependencies

For a Git checkout, initialize pinned public dependencies with git submodule update --init --recursive. The source/setup ZIP already contains those dependency sources and needs no Git/submodule operation. No linked game executable, generated C, game data, key file or private shader cache belongs in a source package. SDL/ImGui/header licenses remain with the pinned sources. See [dependency notices](../third_party/README.md).

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
python3 tools/build.py --host-tests --jobs 4 --build-dir build/host-linux
ctest --test-dir build/host-linux --output-on-failure
python3 tools/build.py --synthetic-tests --jobs 4 --build-dir build/synthetic-linux
ctest --test-dir build/synthetic-linux --output-on-failure
python3 -m unittest discover -s tools/tests -p 'test_*.py'
python3 tools/tests/ffmpeg_decode_probe.py \
  --avcodec "$(pkg-config --variable=libdir libavcodec)/libavcodec.so" \
  --avutil "$(pkg-config --variable=libdir libavutil)/libavutil.so"
~~~

Use the explicit Linux directories above when an existing folder contains Windows builds. Preserve copied caches and the `.git` file; a source ZIP needs no Git repair. If `build/host-linux` or `build/synthetic-linux` itself belongs to another source location or platform, choose another new directory and use that same path for its CTest command. Do not remove old caches to work around CMake path mismatches. The full `host_tests` aggregate must succeed before CTest; passing executable tests alone does not establish compile-only coverage.

Host tests also compile the actual applet/time service, settings loading, actual nvdrv diagnostics and interpolation/SVC integration files. Option/calendar tests cover every camera case variant, UTC leap/nonleap/post-2038 fixtures, three host time zones and calendar bounds. The settings test loads a disposable INI file and checks that configured startup switches enable absent variables while preserving external values, including explicitly empty POSIX values. CTest now has seven host tests.

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

Submitted Bazzite/Ubuntu 24.04.5 native evidence passed the FFmpeg 6.1.1 probe (51 I/P/B pictures, 23,040 NV12 pixel checks), all three synthetic tests and 45 Python tests (4 Windows skips). Corrected native host aggregate coverage subsequently compiled and passed six tests; direct SSH validation with the settings correction now compiles the aggregate and passes all seven tests. Native runtime compilation originally exposed applet/time-service calls and then the settings `_putenv_s` call; these observed portability blockers are corrected, together with later native-only diagnostics/filesystem calls in nvdrv and the Vulkan texture/renderer code. Direct SSH validation on the Ally completed the full native game build and normal first-run ELF verification/publication; all dynamic runtime libraries resolved. No gameplay, controller/GPU/media playback or real-save acceptance is established. Max's Ally playtest remains required.

## Unattended autoplay runs on the Ally

`tools/linux_autoplay.py` runs on the Bazzite host (outside the container). Each run gets its own folder under
`HWDER-linux-native/runs/autoplay/` with a hard link of the play build, copies of its settings and pipeline caches
and a copy of a fixed save (`runs/autoplay/fixtures/autoplay` = test-saves/autoplay 9038..., `busy-spot` = 38e7...).
The play folder and its save are never written. The game runs in `hwder-dev` inside a headless gamescope
(1920x1080, 120 Hz, nothing on screen; Max approved this 2026-10-24) and is stopped after `--seconds`. Keep the SSH
session open for the whole run: Tailscale SSH ends host processes started from a session when it closes.

~~~sh
cd /var/home/bazzite/Documents/HWDER-linux-native
python3 hwder/tools/linux_autoplay.py --fixture busy-spot --mode 120 --seconds 90 --name busy120 [--stats 2000] [--filmstrip]
~~~

Results 2026-10-25 (build cba01eb, Ally Z1 Extreme, RADV), 300-frame window averages in battle:

| Run | fps | Notes |
|---|---|---|
| autoplay, 120 | 8.33-8.39 ms (~120) | 0 extra frames skipped; filmstrip normal |
| autoplay, 60 | 16.67-17.1 ms (58-60) | |
| busy-spot, 120 | 10.7-11.8 ms (85-93) | ~1280 draws; real frame 12.5 ms mean |
| busy-spot, 60 | 16.67-17.2 ms (58-60) | |

Every run has one 100-170 ms frame at ~40 s, the battle load (`pushbuffer entry ... took 1xx ms`). With
`--stats 2000` at the busy spot the `HWDER GPU` thread is 95-97% busy (12.4 ms per frame at ~1270 draws; the
profiler adds ~10%): object blending for in-between frames 2.7 ms, texture binding 1.8 ms, vertex 0.8 ms, key
building 0.7 ms, recording 0.7 ms. It is the same profile as on Windows, scaled by the slower CPU; MainThread's
high share is its present-wait poll. The busy-spot dips are therefore the single translator thread, not a Linux gap.

## Busy-spot speed-ups (2026-10-25, 7eae304 and 4564e01)

`HWDER_SAMPLE_US=500` samples the GPU command thread's CPU time (Linux) and writes counts per function and
address to `<log>.samples.txt`; `addr2line -f -i -C -e hwder <offset>` in the container gives the line. It showed:
crowd bone-palette pairing for in-between frames 13% (scalar compare of whole palettes against dozens of
candidates), clock reads 3% (clock_gettime behind host::counter), the written-page map walk in every texture
check 5%, per-draw heap-allocated profile phases ~2%.

Changes: AVX palette compare with nearest candidates first; host::counter reads the invariant TSC on Linux
(calibrated once, as QueryPerformanceCounter does on Windows); texture checks skip the page map when their
blocks had no write report since the last check; profile phases are std::optional. Pairing outcomes are
unchanged (matched ~75%, blended ~48% of c3 blocks, ambiguous under 0.1%). Tried and dropped: a guest spin-wait
backoff (no change), 8 MiB write-watch blocks (more calls), -O3 (no change).

Busy spot, 120 fps mode, autoplay harness (`tools/linux_autoplay_compare.py`):

| Build | GPU thread per frame | fps |
|---|---|---|
| af2ea14 (before) | 10.4-10.5 ms | 89-93 |
| 4564e01 | 9.0-9.1 ms | 102-105 |

Still short of 120 there: each tick needs two translated frames (~18 ms of GPU-thread work per 16.7 ms tick).
Next candidates from the profile: driver calls (RADV ~10%, submit/write-watch ioctls ~12%), std::unordered_map
lookups in the vertex/index/geometry caches (~6%), command decode (~1.6 ms per frame). The changes are portable
(the TSC part is Linux-only); the branch builds on Windows, where they are not yet measured.
