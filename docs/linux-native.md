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
- Crash handling: no minidumps. Linux writes a crash report into hwder.log (signal, faulting address, host thread, backtrace with the recompiled guest function names) and leaves core dumps to the system.
- Hybrid-CPU P-core preference is Windows-only (the Z1 Extreme has no efficiency cores).
- Error dialogs: the no-game-data, wrong-game-version, save-copy and missing-Vulkan dialogs use SDL message boxes on Linux.
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

Add `--update "/private/path/own-update.nsp"` (or `.nsz`, which needs the `zstandard` module) to build game update 1.0.1: Setup unpacks it into the cache, builds from its main module and writes `update_path` into the play folder's `hwder_settings.ini`. A run without `--update` builds the base game and clears `update_path`.

These are source-candidate commands, not evidence of a successful Linux build. Setup validates the supported game version, extracts local ExeFS, generates C and compiles the complete runtime. Interrupted-build recovery, input quarantine, cache hashing and last-success rollback apply. Default private cache: $XDG_CACHE_HOME/hwder/local-build-v1, falling back to $HOME/.cache/hwder/local-build-v1. Its stable play directory contains the native hwder ELF executable, settings and private caches. Saves default to $XDG_DATA_HOME/HWDER/save, falling back to ~/.local/share/HWDER/save (not the cache, which cleaners may empty); save_dir in hwder_settings.ini overrides it. New cache roots are private. Generated outputs are never distributable artifacts. Use --cache only for a private user-owned directory. Identity includes target platform, source, compiler and dependency versions. ELF64/little-endian/x86-64 identity and execute mode are checked; publication preserves execute permissions.

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

Round 3 (2026-10-25): the periodic `vk: gpu thread busy` log line now also gives vkQueueSubmits per frame and
their cost. The busy spot makes ~24 submits per frame at ~21 us each (0.5 ms), and each starts a new round of
texture write-watch queries (~74 PAGEMAP_SCAN calls per frame, ~3.8 us each). Almost all submits come from the
game's wait-for-idle methods. Tried and dropped: batching consecutive GPFIFO submissions and deferring
semaphore/syncpoint signals (no change, the guest submits one batch at a time), and turning wait-for-idle into a
barrier (1.6 submits per frame and a 6.3 ms GPU thread, but the game waits on those fences: 120 fps mode fell to
60). Submit timing is part of the game's frame pacing; the remaining gains are in per-draw CPU cost.
A flat open-addressing map (`gpu/vk/flat_map.h`) for the per-draw vertex/index slice and index-range caches:
GPU thread 8.9-9.0 -> 8.7-9.0 ms per frame, busy spot 103 -> 104-106 fps (small, within run-to-run noise).

Round 4 (2026-10-25): flat maps for the texture header (TIC) cache (57f18b9) and the geometry cache (cfdc012).
Play build rebuilt from cfdc012: busy spot at 120, GPU thread 8.36-8.51 ms per frame, 106-109 fps (five runs);
autoplay at 120 105-120 fps; 60 fps runs 58-60; no errors, thumbnails normal, blending outcomes unchanged.
Neutral and reverted (interleaved A/B, three runs each): a lock-free pipeline-result check, flat program caches
and a vertex-format table (63e8180); a cheaper per-draw timer and cached device limits. Run-to-run noise at the
busy spot is about +-0.2 ms of GPU-thread time per frame, so single changes below that are not measurable.
Note for measurements: a dev build's pipeline warmup cache is "incompatible" with the play build's, so its
pipelines compile on demand; compare builds only after the first ~40 s (the harness's battle windows).

Round 5 (2026-10-25): object blending for the in-between frames runs on its own thread ("HWDER objects"),
in the order the draws were bound, with each queue submit waiting until it has written its staging slices
(`HWDER_GI_OBJ_SYNC=1` keeps it on the GPU thread). Same binary, interleaved, three runs each at the busy spot:
GPU thread 8.35-8.45 -> 8.11-8.21 ms per frame, 108-109 -> 111 fps. The GPU thread waits ~0.01 ms per tick for
it. Blending outcomes, 60 fps runs and thumbnails unchanged. The gain is smaller than the ~1 ms per frame the
blending costs, presumably because a second busy core shares the handheld's power budget (inferred).

Round 6 (2026-10-25): vkQueueSubmit runs on its own thread ("HWDER vk submit"), in order; the GPU thread hands
the frame over (it is already in flight, its serial counts as submitted; timeline waits may precede the
signalling submit). `HWDER_VK_SYNC_SUBMIT=1` submits on the GPU thread. Same binary, interleaved, three runs each
at the busy spot: GPU thread 8.26-8.38 -> 7.86-8.01 ms per frame, 109-110 -> 111-113 fps. 60 and 120 runs on both
saves: no errors, no fence or GPU timeouts, thumbnails normal. Retested properly and still neutral: cached device
limits and a cheaper per-draw timer.

Round 7 (2026-10-25, not kept): a helper thread that pre-scans the texture write-watch blocks after each submit
cut the GPU thread's kernel time (ioctl 7.7% -> 5.9%) but only 7.7-7.85 -> 7.6-7.75 ms per frame and no fps
change (112 both, same-binary A/B). At the busy spot the tick is now bound by the game itself as much as by the
GPU thread: per tick the guest's real frame takes ~8.8 ms, its in-between draw ~4.8 ms and waiting to present it
~3.4 ms (16.9 ms against 16.7), while the GPU thread needs 2 x ~7.8 ms. Closing the last ~3% needs faster
recompiled guest code (the game's own draw submission on MainThread) as well as translator work.

Round 8 (2026-10-25, branch `draw-path-perf`): the game's own draw submission on MainThread.
`HWDER_SAMPLE_US` now samples MainThread too (from the first battle tick), with each sample tagged by tick phase
(update, extra draw, extra present, real frame, split again at QueueBuffer) into `<log>.samples-main.txt`.
`HWDER_TIMELINE=<s>` writes 0.5 s of frame-loop events (MainThread phases, GPU thread busy/idle, GPFIFO
submissions, syncpoints, vi queue/dequeue, compositor) to `<log>.timeline.txt`.

What they showed at the busy spot: MainThread's ~95% "busy" is mostly a `sched_yield` spin in the SDK's fence
wait (f_10026c8364): 81% of its extra-present samples and 28% of its real-frame samples. Its real CPU per tick
is about 12 of 16.7 ms. The game submits each frame as two GPFIFO entries at its end and, before recording the
next frame, waits until the previous frame's syncpoints are reached (signalled when the GPU finished it). The GPU
thread takes ~8.4 ms (wall) per frame and idles ~1.7 ms in every real frame, because the real frame's recording
(~6.4 ms) plus the previous frame's GPU completion (2-4 ms after translation) is longer than one translation.
The GPU is 84-86% busy at ~1.5 GHz sclk (package ~30 W, cores ~4.0 GHz). Hottest guest code on MainThread: the
SDK memcpy (f_10027971c4, 7.6% of all samples, 11% of the extra draw), then hw_dispatch/hw_call (4.4%), then a
flat list of draw-path functions (vtable-call loops at ~1-2% each).

Kept: the SDK's memcpy/memmove/memset/bzero/strlen run as host C library calls (gen_main.py
`LIBC_INTRINSICS`; `HWDER_GUEST_LIBC=1` runs the translated bodies), and the recompiler keeps guest registers in
C locals that are written back only at calls and exits (gen.py `regcache_emit`; `--no-regcache` restores the
old output). Same binary, interleaved, three runs each: host libc cut the extra draw 5.1-5.2 -> 4.7-4.8 ms
(frame 8.90-8.95 -> 8.70-8.87 ms); register caching then 4.7-4.9 -> 4.5 ms (frame within noise, 8.78 -> 8.73).
The time saved on MainThread mostly turns into more waiting in present: MainThread is no longer critical.
Measured neutral and not kept: early syncpoint signalling (the wait moves to vi buffer slots and the 120 Hz
release grid), blocking waits instead of the guest yield spin and the object worker's spin, an inline per-site
lookup for indirect calls, batched ConstBufferData writes.

Final dev build against the play build (a83c4a8), interleaved, three runs each at the busy spot at 120: frame
8.80-8.88 -> 8.71-8.75 ms (113 -> 114-115 fps), extra draw 4.94-5.02 -> 4.35-4.59 ms, GPU thread unchanged
(7.6-7.8 ms per frame). Autoplay fixture at 120 (two runs each), 60 fps runs on both saves: same ranges as the
play build, no errors, thumbnails normal. Still short of 120 at the busy spot.

GPU thread, same round: the per-phase profile puts descriptor-set allocation and update at only ~0.3 ms per frame
(desc-alloc 0.17, desc-update 0.13 of the GPU thread's ~8.4 ms with the profiler on), so set caching was not
attempted. Texture write-watch queries are now keyed by guest GPFIFO submission (~2 per frame) instead of the
recorder serial (~24 per frame); `HWDER_WATCH_PER_SERIAL=1` restores the old keying. Same binary, three runs
each: GPU thread 7.68-7.76 -> 7.59-7.61 ms per frame, frame unchanged (8.65-8.72 ms either way).
At `HWDER_RES_SCALE=0.5` the GPU drops to 59% busy, the cores clock up from ~4.0 to ~4.6 GHz, the GPU thread to
5.6 ms per frame and MainThread's extra draw to 3.1 ms, and late in the run the busy spot reaches ~120: at the
busy spot the Ally is limited by the shared ~30 W package budget (CPU clocks against GPU load) more than by any
one thread. Remaining candidates: less GPU work per frame, and burning less power in MainThread's fence-wait
spin and the object thread's spin (single runs of blocking variants showed no change; worth a power-aware
retest with more runs).

Round 9 (2026-10-25): `HWDER_GPU_TIMING=<s>` logs GPU busy/idle per frame and the costliest render passes from
GPU timestamps, and the 120-frame `gpu thread busy` line is followed by the GPU thread's waits for the GPU
(submission fences, pushbuffer syncpoints, semaphore acquires). The GPU thread never waits on the GPU at the busy
spot: it spends 7.7-8.3 ms of real CPU per frame translating. Removing the end-of-pass barriers saved only
~0.2 ms of GPU time per frame and was dropped, as was a no-clear probe (both were measurement-only switches).

Round 10 (2026-10-25): GPU-thread CPU cost per draw. Sampled at the busy spot the thread spent its time in
bind_resources (11%), RADV (13%), write-watch ioctls (7%), command decode (~10%: pushbuffer words, method
dispatch, macros), per-draw hashing of register blocks and pipeline keys (~5%) and texture checks.
Kept, each measured interleaved, three runs each (GPU thread per frame):
- per-draw lookups: the program, framebuffer and pipeline caches compare the previous draw's registers/key
  word for word before hashing; the pipeline-result lock is taken only when a worker has published results;
  unbound texture slots use cached dummy views; texture headers and samplers go through small direct-mapped
  caches by TIC/TSC index in front of the hashed ones; bind_resources takes the stage's constant-buffer bindings
  instead of std::function callbacks; the slow-op timer is cheaper. Against 61c2571: 8.14-8.24 -> 7.52-7.59 ms.
- command decode: plain 3D register writes (every method outside method_3d's switch) are stored inline by the
  pushbuffer and macro loops (HWDER_GPU_TRACE=2/3 keeps the full path), the pushbuffer is prefetched, and macro
  parameter buffers keep their capacity: 7.41-7.75 -> 7.25-7.37 ms.
- no slow-op timer per draw (a slow draw still shows as a slow pushbuffer entry); vertex/index staging computes
  its sample hash and per-submission cache lookup only when the persistent geometry cache misses, and the index
  range scan trusts an unchanged persistent range first: 7.28-7.38 -> 7.24-7.27 ms (small, pure removals).
- descriptor-set reuse: a graphics stage whose constant-buffer contents (hashed), textures and samplers equal
  the last set filled for its layout in the same queue submission binds that set again, without staging copies,
  allocation or update; `vkCmdBindDescriptorSets` is skipped for a set already bound with the same layout. About
  40% of stage sets are reused (705-730 of ~1800 per frame): 7.17-7.27 -> 6.99-7.00 ms.
Measured and dropped: one write-watch query over runs of adjacent 2 MiB blocks (280k -> 119k ioctls per run but
1.88 -> 1.62 s of kernel time: the cost is the page walk, not the call), and a host-cached staging ring (GPU
thread 7.38-7.43 -> 7.43-7.66 ms).

Total at the busy spot at 120: GPU thread 8.14-8.24 -> 6.99-7.00 ms per frame (-15%), frame 8.72-8.83 ->
8.51-8.57 ms (113-115 -> 117-118 fps). The frame gains less because the cycle is now a chain: the GPU thread
translates the in-between frame (~6.8 ms), the GPU finishes it ~3.7 ms later, and only then does MainThread
record the next in-between frame (~7 ms with its waits); the GPU thread idles ~3-4 ms per two frames.
Regression: autoplay at 120 8.36-8.80 ms (before 8.35-9.09), 60 fps on both saves 58-60 fps, no errors,
thumbnails as before, host tests pass. The 60 fps runs' one long frame entering the battle (~44 s) was
170-183 ms against 154-161 ms before.

Round 11 (2026-10-26): the in-between frame chain and more GPU-thread cuts. With `HWDER_GPU_TIMING` the GPU is
busy 6.45 ms per frame with 2.1 ms idle between submissions; its biggest pass is the 1920x1080 G-buffer pass
(~830 draws, 2.8 ms), one rendering instance, so the GPU starts it only after the GPU thread has translated all
of it. Measured and dropped (same binary, interleaved, three runs each):
- submitting at render-pass boundaries once 100/300 draws were recorded: no change (the game's wait-for-idle
  methods already split each frame into ~24 short command buffers; the kick hardly fired);
- also splitting inside a pass every 150/300 draws (ending and resuming rendering): frame 8.52-8.59 ->
  8.46-8.47 ms, but one of the six runs hit a GPU hang ("GPU timeout waiting for serial", then 61 fps). The only
  GPU timeout in this work; not worth ~0.08 ms.
Kept:
- FlatMap marks empty slots with key 0 instead of a separate occupancy array (one cache line per probe; the
  geometry cache holds up to 200k entries), GeomEntry without padding (80-byte slots), macro writes inlined into
  the interpreter loop: GPU thread 7.05-7.08 -> 6.96-7.00 ms against ddfed10.
  A second run of the FlatMap change alone: 7.00-7.06 -> 6.94-7.02 ms. Small, but both A/Bs point the same way.
Also dropped: skipping TextureCache::validate on a texture-header cache hit within the same guest submission
(6.96-7.01 -> 6.89-6.99 ms against the same build without it: noise). A first measurement had looked better
because the build also contained the write-watch run queries dropped in round 10 by mistake; ddfed10 itself
only carried those two unused members in a header, its write-watch code is the measured one.
Frame time is unchanged by these (8.48-8.57 ms either way): at the busy spot the GPU thread no longer
sets the pace; the chain of game recording, translation and the GPU's own work does, near the 120 Hz grid
(~117-118 fps, most frames on time).
The battle-load frame of the 60 fps runs is the first battle submission compiling pipelines synchronously
(empty shader cache in every test run). Side by side the same day: autoplay save 61c2571 168-169 ms, round-11
builds 167-179 ms; busy-spot save 61c2571 172-178 ms, this build 180-187 ms (the submission 125-130 -> 129-137 ms).
So round 10's 154-161 ms was a different session; what remains is a few ms (~5%) on a one-time hitch, within the
spread of the compiles but in one direction in both pairs, not investigated further.
Regression on the final build: busy spot at 120 8.40-8.74 ms windows (GPU thread 6.91 ms), autoplay at 120
8.38-9.05 ms, 60 fps on both saves 58-60 fps, no errors or GPU timeouts, thumbnails normal, host tests pass.

Round 12 (2026-10-26): GPU work. Probes with `HWDER_GPU_TIMING` (same binary, two or three runs each):
translating Maxwell's FMZ multiplies (0 times anything is 0) as plain multiplies changed nothing (GPU busy
6.43-6.52 against 6.47-6.48 ms per frame), so shader ALU is not the limit; `RADV_DEBUG=nodcc,nohiz` raised GPU
time 6.46 -> 7.2 ms, so colour/depth compression is in use. The full-screen clears were not fast clears: images
live in GENERAL layout, where RADV never fast-clears, and the game's colour clears cost ~0.6 ms of GPU time per
frame at the busy spot (four G-buffer targets 0.36 ms, three effect targets 0.25 ms), depth clears 0.34 ms.
Kept: a colour clear of a whole single-level, single-layer image is a load-op clear in COLOR_ATTACHMENT_OPTIMAL
layout, between two layout barriers (vk_draw.cpp `fast_clear_color`). Busy spot at 120, interleaved, three
runs each against f3496cd: GPU busy 80-81% -> 73%, GPU thread 7.01-7.10 -> 6.66-6.78 ms, frame 8.50-8.56 ->
8.47-8.50 ms. Depth clears done the same way stayed at 0.34 ms (these depth images cannot fast-clear) and were
left as they are. Regression: autoplay 120 8.39-9.14 ms windows, 60 fps on both saves, no errors or GPU
timeouts, thumbnails as before.
The remaining gap to 120 was not average speed but a hitch: one frame every 300 was 20-39 ms late. Logging every
frame over 20 ms put each one at the geometry cache's sweep, which rebuilt the whole table every 300 frames to
drop ranges not drawn for 300 frames: 17-22 ms on the GPU thread for ~40k entries (also present before this
work). The sweep now deletes stale entries in place (backward-shift deletion, `FlatMap::sweep`), a 32nd of the
table per frame; the eviction rule is unchanged. FlatMap passes a randomized test against std::unordered_map
under ASan/UBSan (`runs/drawperf/flatmap_test.cpp`, 40 rounds of 200k operations). Busy spot at 120,
interleaved against 61f2661, three runs each: frame 8.47-8.50 -> 8.36-8.38 ms (118 -> 119-120 fps), worst frame
per 300-frame window 18-39 -> 12-18 ms. Autoplay at 120 8.32-8.40 ms windows (119-120 fps, worst 18 ms); 60 fps
on both saves 58-60 fps with per-window worst frames 19-22 ms (32-47 ms in round 8's build). No errors or GPU
timeouts, thumbnails normal, host tests pass. Also seen: `save_pipeline_cache` (every 600 presents) takes 5-6 ms
on the present path when there are new pipelines or warmup records (early in a session), mostly the warmup
snapshot copy; left as it is.

Round 13 (2026-10-26): Max's playtest of f207fc9 (no bugs; "still some dips to 60 fps"). Its log: menus and loading
at 60 Hz by design, the first battle frame compiling shaders (184 ms), then dips in stretches where the pipeline
count grew (103 -> 183 pipelines during the session, one synchronous compile of 46 ms) and in heavy scenes where
the GPU thread needed 8.4-9.6 ms per frame (1400-1750 draws, ~2500 descriptor sets, 18% reused, against ~1300
draws and 40% at the busy spot). The session started with "prewarm early load: incompatible, 0 recipes": the
warmup cache's compatibility tag included the build commit, so every rebuild started cold and also replaced the
accumulated recipes with that session's. The tag no longer includes the commit (epoch "ABI 2"): a stored program
is adopted only when its whole record equals this build's translation, a pipeline only for an equal key, and
prewarm compiles it with this build's code, so the commit added no protection. `tools/linux_autoplay.py
--caches-from <run>` seeds a test run's pipeline caches from another run. Busy spot, a cold run then a warm run
from its caches, twice: 112-115 recipes loaded and adopted, 0 mismatched, draws skipped for a missing pipeline
174-186 -> 0-4, battle-load frame 161-170 -> 147-160 ms; steady frame rate the same (8.33-8.37 ms windows).
Autoplay warm run: 134 of 136 adopted, 119-120 fps, thumbnails normal. The next build after this one still
starts cold once (the tag format changed); later rebuilds keep their recipes.

Round 14 (2026-10-26): a heavy test case and the real play setup. Max's mid-battle save from 2026-10-26 is the
`heavy-fight` fixture (`linux_autoplay.py --fixture heavy-fight`; `HWDER_AUTOPLAY_X=1` presses X once 1 s into the
battle, which starts the special attack whose frames dropped). Max plays in Game Mode (TDP setting 25 W, Steam frame
limit 120, VRR on, tearing off). Headless runs are not slowed by Game Mode running in the background (only a run
during the switch itself was). A run launched outside Steam into Game Mode's gamescope got a small window
(rendered at 0.667x) and is not comparable; `Test HWDER heavy.sh` (in the project folder, added to Steam as a
non-Steam game and started by Max) runs an on-screen A/B the way Steam launches the game: 1920x1080, VRR, the
120 limit, 21-26 W. On screen, e7bb3ff and 3f32766 alike: the special attack 8.5-9.2 ms per 300-frame window
(109-118 fps) for ~10 s, then 8.32-8.40 ms (120); headless gives nearly the same (8.4-8.95 ms). An earlier visible
run on the GNOME desktop (Xwayland) ran the attack at 10.3-12.0 ms. The dips to 60 in Max's sessions were mostly
the first session of each new build compiling the attack's pipelines (cold warmup cache, fixed in e7bb3ff).
Sweeping a 256th of the geometry cache per frame (a 32nd cost ~4% of the GPU thread in this battle): GPU thread
6.75-6.82 -> 6.57-6.74 ms per frame headless, no visible difference on screen.
The special attack's dip to 60-70 (Max saw 69 on Steam's counter in an on-screen run, and in his own play):
the extra frame was skipped as a camera cut whenever the projection changed, and the attack animates the field
of view every tick (a zoom; up to 0.13 rad per tick at fov 0.88, ~15%), so for ~10 s every in-between frame
was dropped and the attack ran at 60 Hz. It shows when the attack starts after fighting (`linux_autoplay.py
--fixture heavy-fight --fight`), rarely when it starts standing still (1-4 skips), which is why the idle tests
missed it. The gi log now splits cuts into camera jumps, field of view (largest change) and other projection
state. A field-of-view change of up to 20% per tick with nothing else in the projection changing is blended
halfway for the extra frame (Camera_Update rebuilds proj from it); larger changes and other projection changes
are still cuts. Heavy save fighting, two runs each: in-between frames drawn during the attack 106-112 ->
283-285 per 5 s, attack windows 10.87-13.37 -> 8.51-8.88 ms; busy spot unchanged (120 fps); thumbnails of the
zoom normal.
Max's session on 1e4ad64 ("better, some dips to 89"): the attack's zoom frames were blended (110-210 per 5 s),
no field-of-view cuts left; the remaining dips were in his heaviest moments (2,100-3,700 draws per frame), where
the GPU thread needed 8.2-9.4 ms per frame and extra frames were skipped "behind the 60 Hz grid" (30-67 per 5 s).
Vulkan command recording now runs on a worker thread ("HWDER vk record", vk_sched.cpp): the GPU thread queues each
command, or a draw's whole changed state and the draw itself (vk_draw.cpp `DrawRec`), as a closure in 64 KiB
chunks; the worker records them in order and ends and submits the command buffers (it replaces the submit
thread). Descriptor updates and staging writes still happen on the GPU thread before the commands that use them
are queued. `HWDER_VK_SYNC_SUBMIT=1` records and submits on the GPU thread (fallback, A/B). Interleaved against
1e4ad64, three runs each: GPU thread 6.18-6.31 -> 5.76-5.91 ms per frame on the heavy-fight save (fighting,
special attack), 6.78-6.85 -> 6.01-6.21 ms at the busy spot; frame times unchanged there (both builds at or near
120). Autoplay at 120 8.33-8.43 ms windows, 60 fps on both saves, no errors or GPU timeouts, thumbnails normal,
host tests pass; the synchronous fallback runs (GPU thread 7.36 ms at the busy spot).
The first Play install of c216e45 froze ~45 s into a headless heavy-fight check ("GPU timeout waiting for
serial"): the present, which waits on the render timeline for the last flushed serial on the same queue, could
be queued before the worker had submitted that serial, so the queue waited for a signal queued behind it. The
present now waits until the worker has submitted the serial (`wait_submitted`). Heavy-fight save fighting,
interleaved: 1 hang in 8 runs without the fix, 0 in 16 with it. The previous submit thread had the same race
with a much smaller window (likely the one hang in the round-11 mid-pass split experiment). The Play build was
rolled back to 1e4ad64 at once.
That fix still read the recorder serial three times in present() while the GPU thread could advance it; a check
in present() (an error is logged when the serial waited for is not yet submitted) fired 5 times in each of 5
heavy-fight runs. The serial is now read once (and atomic): 10 heavy-fight runs with 0 check firings and 0
hangs; regression runs and host tests pass.

## No extra frames while the game runs at 30 fps (2026-10-26)

Max saw a flashing text box during cutscenes. His 34deaf1 play log shows the video cutscene (nvdec/vic active,
226-288 s) ticking at 30 fps: only every other tick drew an extra frame (75 drawn, 75 "behind the 60 Hz grid"
per 5 s), so the screen got 2 real + 1 extra image per two ticks at uneven gaps (22 ms average, 45 ms worst).
Extra frames are now off while the smoothed tick interval is above 26 ms and back on below 21 ms (logged as
"gi: game frame rate ..."); `HWDER_GI_KEEP_30=1` keeps them for comparison. Busy-spot and heavy-fight runs
against 34deaf1: same extra-frame counts and frame time (8.36 / 8.40 ms), the rule never switched on in battle.
Not yet confirmed in the cutscene itself (autoplay cannot reach it): needs a playtest.
