# HWDER - Hyrule Warriors: Definitive Edition Recomp

A static recompilation of *Hyrule Warriors: Definitive Edition* (Nintendo Switch) to a native
Windows x64 executable. The game's ARM64 code is translated ahead of time to C and compiled with
clang; everything the game expects underneath it (the Horizon kernel, system services, the GPU,
audio, video decoding, the file system) is implemented by our own runtime, written for this
purpose. There is no CPU emulation, no JIT and no emulator core. The result is one `hwder.exe`
that reads code and assets straight out of the player's own game dump at startup.

**This repository contains no game code and no game assets.** The recompiled C in `generated/`
is produced on your machine from your own dump. First-run setup extracts the five supported
ExeFS modules into your private local cache, generates C and builds the complete native program.
The runtime reads ExeFS and RomFS from your own `.nsp` with your own `prod.keys` while you play.

---

Native Linux x86-64 is an experimental port for Bazzite/ROG Ally on the `linux-native` branch; it is not part of a
release. See [the Linux build and validation instructions](docs/linux-native.md).

## Playing

You do not need to clone this repository or install any developer tools. Windows 10/11 x64 only.

1. **Download** the latest `HWDER-setup-<version>.zip` from this repository's **Releases** page (not the
   green "Code > Download ZIP" button, which is source only).
2. **Extract** the whole zip into a folder you will keep (for example `C:\Games\HWDER-setup`) and open
   **HWDER Setup.exe**. The exe is not code-signed yet, so Windows may show "Windows protected your PC":
   choose **More info > Run anyway**.
3. **Next**: pick your game file (`.nsp`) and your `prod.keys`. Both must come from your own Switch and
   your own copy of *Hyrule Warriors: Definitive Edition*: the game dumped from your cartridge or eShop
   purchase (the base game, without an update merged in), and the keys dumped from the same console
   with homebrew tools. HWDER ships neither and
   cannot help you obtain them. The files are used where they are; keys are never copied.
4. **Install**: after you confirm, Setup downloads pinned build tools (Python, LLVM-MinGW, CMake, Ninja
   and four Python packages, each checked against a fixed SHA-256) from python.org, GitHub and PyPI into
   its Data folder (default `%LOCALAPPDATA%\HWDER`). No administrator rights, system installs or PATH
   changes.
5. **Prepare**: Setup checks that your dump is the supported version, extracts the game code, translates
   it to C and compiles `hwder.exe` on your PC. It shows the step (k of 6) and the time remaining; expect
   several minutes and several GB of disk space.
6. **Play** starts the game. The first launch shows a startup screen that checks your game and keys.
   Once it is ready, **Create desktop shortcut** and **Add to Steam** (a non-Steam game; close Steam
   first) let you start the game from the shortcut, from Steam (or `%LOCALAPPDATA%\HWDER\play\hwder.exe`) without opening Setup. Keep your `.nsp` and
   `prod.keys` where they are: the game reads them every time it starts.

**The first minutes on a fresh install can hitch.** Each graphics pipeline is compiled the first time
the game needs it, so the opening minutes (and each new stage the first time) can show short hitches or
an object appearing a moment late. Graphics > Shaders > "Compile shaders in the background" (on by
default) picks late objects over hitches. Compiled pipelines are saved beside `hwder.exe`
(`pipeline_cache.bin`, `pipeline_warmup.bin`) and prebuilt before the startup screen on later
launches, so this fades as you play. A new HWDER version, GPU or graphics driver starts the warmup list
over (the driver cache may still help). Deleting both files is safe.

**Advanced** also holds the Data folder choice, Check requirements, Open game folder and the Details log.
Next time, Setup opens straight on **Play**. More detail: [Windows setup](docs/windows-setup.md).
Developers who want to build from a checkout: [local-build.md](docs/local-build.md).

**No game code or assets are in this repository or the release zip.** The game-linked `hwder.exe`,
PDB and generated C are made on your machine from your own dump and are never packaged. `tools/make_dist.py`
and `tools/make_setup_dist.py` create the source and setup kits, with no game dump, keys, generated
program, saves or private shader cache.

Requirements: Windows 10/11 x64, a Vulkan 1.2 driver (NVIDIA, AMD, Intel), an AVX2 CPU, and
Media Foundation (built into Windows; "N" editions need the Media Feature Pack) for the movies.
Without an AVX2 CPU or a Vulkan 1.2 driver the game shows a message saying which is missing and
closes; Setup's Check and Prepare also refuse a CPU without AVX2.

In game: **F1** opens the options overlay (internal resolution scale, unlocked resolution for
ultrawide, vsync, borderless, anisotropic filtering, FPS counter, opt-in 60 Hz monitor switch, and
a save-file cheats tab). Settings > Display > Frame rate > "120 fps" draws an extra in-between frame per
battle tick (off by default, switchable while playing; menus stay at 60; see `docs/high-framerate.md`). **F11** toggles borderless fullscreen. SDL3 gamepads and XInput controllers work; two-player local co-op (the game's 2P mode): controllers take player slots in the order they connect, and the keyboard joins the first free slot by default (Settings > Input > Players chooses a fixed player or turns it off). Pressing the left stick on the warrior select answers the game's controller screen with the connected players;
keyboard: WASD move, arrows camera, K/Enter = A, J/Backspace = B, I = X, L = Y, Q/E = L/R,
U/O = ZL/ZR, Space = +, Tab = -, 1-4 = D-pad, Z/C = stick clicks.

The wizard keeps its playable result in the selected Data folder, initially
`%LOCALAPPDATA%\HWDER\play\hwder.exe`. The legacy CLI defaults to
`%LOCALAPPDATA%\hwder\local-build-v1`; select that existing folder in the wizard to reuse it.
There is no automatic data migration. Files created beside the exe: `hwder_settings.ini`, `hwder.log` (attach to bug reports),
`save\` (console save layout), `cache\` and `pipeline_cache.bin` (safe to delete). To use another save, choose
its `zmha.bin` on the startup screen or in the menu's Gameplay tab (Choose save file). A save in the console
layout is used where it is; any other (an emulator export, a backup) is imported into the current save folder
after asking, with the save it replaces backed up first. Nothing is moved or deleted.

## Reporting a bug

`hwder.log` is written beside `hwder.exe` (or wherever `HWDER_LOG` points) and is rewritten on
every launch. After a crash, a minidump `hwder-crash-YYYYMMDD-HHMMSS.dmp` is written next to it
(the newest 5 are kept; `HWDER_NO_DUMP=1` turns them off; they never leave your machine). Open an
issue with the [bug report form](https://github.com/sonsegajp/HWDER/issues/new?template=bug_report.yml),
attach the log, and zip the .dmp before dragging it in. The version string it asks for is at the
bottom of the F1 menu and on the first line of the log. Never attach game files or keys.

---

## Building from source

The automated workflow above is also the recommended source build. To prepare without launching:

```
python tools/first_run.py --nsp "<game>.nsp" --keys "<prod.keys>"
```

The manual developer flow remains available. It requires installed Python x64 with cryptography
and lz4, plus VS2022 Build Tools with MSVC x64, Clang, CMake/Ninja and a Windows10/11 SDK.
The managed wizard does not require those global developer installations.

```
git clone --recursive https://github.com/sonsegajp/HWDER
cd HWDER

# 1. extract your dump once for the recompiler (exefs/ is all it needs; romfs/ is optional,
#    the runtime can read it from the NSP directly)
python tools/nx_extract.py "<game>.nsp" "<prod.keys>" data --no-romfs

# 2. ARM64 -> C (all five ExeFS modules, ~5 minutes, writes generated/)
cd tools && python -m recomp.gen_main && cd ..

# 3. build (clang-cl + Ninja inside the VS developer environment)
python tools/build.py

# 4. run using your own NSP and keys (never copy keys into a release)
set HWDER_NSP=<game>.nsp
set HWDER_KEYS=<prod.keys>
build\release\hwder.exe

# package: source/setup kit only; no linked exe/PDB/generated C
python tools/make_dist.py
```

If the game ever jumps to an address the recompiler did not know was code, the runtime logs it,
appends it to `extra_entries.txt` and exits; `tools/iterate.sh` automates the
regenerate-rebuild-rerun loop (it collects them in `data/extra_entries.txt`, which the generator
merges with the tracked list in `tools/recomp/extra_entries.txt`). The tracked list already covers
normal play.

---

## How it works

### 1. Static recompilation (`tools/recomp`, `tools/nso.py`)

`nso.py` loads the five NSO modules of the ExeFS (`rtld`, `main`, `subsdk0`, `subsdk1`, `sdk`),
decompresses their LZ4 segments and applies the dynamic relocations, giving a fully linked
in-memory image of the process exactly as the loader on the console would have produced it.

`recomp/gen.py` then discovers functions in each module from several independent sources:
`.eh_frame` unwind entries, the entry point and init/fini arrays, exported dynamic symbols,
direct `BL` targets, tail-call targets, absolute code pointers found in relocated data, and
`ADRP+ADD` address materialisations. Inside each function it finds the intra-function branch
targets and recovers jump tables (the compiler's `ADR` + `LDRB/LDRH` + `ADD` + `BR` idiom), so
`switch` statements turn into real C `switch`es rather than indirect dispatch.

`recomp/a64.py` and `a64_simd.py` translate every AArch64 instruction (base integer, FP, the
full Advanced SIMD set, the AES/SHA/CRC extensions, saturating and narrowing arithmetic) into
C that operates on a guest register file `Ctx` (`runtime/include/hwder/cpu.h`). The translator
reaches 100% of the instructions in all five modules; anything it has no pattern for would
emit a call to a runtime hook that logs and halts, which never fires in practice.

Each guest function becomes one C function `f_<address>(Ctx*)`. Direct calls become direct C
calls. Indirect branches (`BLR`, `BR`, returns through unusual paths) go through a dense
address-to-function table built for every module's text segment, and the dispatcher uses
clang's `[[clang::musttail]]` so tail calls and returns cost a jump, not a growing host stack.
Functions that contain an unresolvable indirect branch are emitted as *resumable*: they carry a
label at every instruction and can be entered mid-function, which is how the game's hand-written
assembly and longjmp-style control flow keep working.

The generated program is split into `rc_NNNN.c` files of about 12k lines each so the build
parallelises. `module_info.c` records each module's load address and segment layout; at run
time `hwder.exe` maps the real NSO segments from the player's dump to those same addresses, so
the recompiled code and the original data (rodata, vtables, string tables, relocated pointers)
line up byte for byte.

### 2. The kernel (`runtime/src/kernel`)

Guest threads are real Windows threads. `svcCreateThread` creates a host thread whose stack
and TLS live in the guest address space, and the thread simply calls the recompiled entry
function. An `svc` instruction in the original code is a direct C call into `svc.cpp`, which
implements the Horizon system calls the game and its SDK use: memory management (`SetHeapSize`,
`MapMemory`, `QueryMemory` with the exact page attribute model), threads and their priorities,
core masks and the `GetCurrentProcessorNumber` rule the SDK's job system relies on, all the
synchronisation primitives (`WaitSynchronization`, `ArbitrateLock/Unlock`, condition variables,
`WaitForAddress`/`SignalToAddress`), events, transfer memory, sessions and IPC, and `GetInfo`.

Guest memory is a flat reservation at the addresses the console would use (code at `0xF_FFE0_0000`
and up, a heap region, stack and TLS regions), so a guest pointer is a host pointer. There is no
page table emulation and no fault-based fastmem: the game runs on the host MMU directly.

### 3. Services and IPC (`runtime/src/service`, `fs`, `hid`, `audio`, `video`, `gpu`)

The SDK talks to system services over HIPC. `ipc.cpp` implements the real message format
(CMIF headers, descriptors for buffers, handles and domains), so the unmodified SDK code in the
`sdk` module marshals requests exactly as on hardware and our services unmarshal them. Services
implemented: `sm`, `fsp-srv` (RomFS storage, save data, SD card), `hid` (shared memory input
state fed from XInput/keyboard), `appletOE`/`applet` (focus, operation mode, performance),
`vi` + `nvnflinger` (display layers and the buffer queue), `nvdrv` (the GPU driver interface),
`audren` (the audio renderer), `set`, `time`, `acc`, `pctl`, `psm`, `nifm`, `ns`, `mm`, `lm`
and friends.

**File system.** `fs/nsp.cpp` reads the game straight out of the `.nsp`: it parses the PFS0
container, decrypts the program NCA header (AES-XTS, `header_key`), derives the content key
from the key area (or from the title's ticket when rights-ID crypto is used) and serves the
ExeFS partition and the RomFS section (IVFC level 5) through on-demand AES-CTR decryption with
Windows CNG. The SDK's own RomFS driver then parses that image through the `IStorage`
interface, as it would on the console. An extracted `exefs/` + `romfs/` pair works too: `romfs.cpp`
synthesises a RomFS image (header, hash tables, directory and file tables) over the host files
so the same driver path is used. Save data maps to `save\<title>\<save id>\` on the host
(or the save folder picked in the settings, `fs/save_dir.cpp`).

**Audio.** `audio/audio.cpp` implements the `audren` renderer: it parses the per-frame update
(voices, mixes, splitters, effects, sinks, wave buffers), decodes PCM16 and ADPCM voices with
pitch resampling, routes them through the mix graph into the sink and plays the result with
WASAPI in shared mode at 48 kHz.

**Video.** The game's cutscenes are H.264 streams fed to the console's NVDEC through `nvdrv`.
`video/host1x.cpp` and `decoder.cpp` reconstruct the bitstream (SPS/PPS synthesised from the
decoder registers, slices re-wrapped as Annex-B), decode it with Media Foundation in low-latency
mode, and `vic` composition converts the result into the surface format the game expects.

### 4. The GPU (`runtime/src/gpu`, `shader`)

The game renders through NVN, which on hardware is a user-space driver that writes Maxwell
(GM20B) command buffers and submits them through `nvhost-gpu`. Because the real NVN driver in
the `sdk` module is recompiled along with everything else, we never had to reverse NVN itself.
Instead `gpu.cpp` is a Maxwell command processor: it executes GPFIFO entries, parses pushbuffer
methods for the 3D, compute, 2D, DMA copy and inline-to-memory engines, runs the macro
interpreter for the driver's uploaded macros, keeps the full register state, and turns draws,
clears, copies and dispatches into work for the renderer.

`shader/` is our SASS to SPIR-V translator. Maxwell shaders are decoded from the game's
precompiled shader binaries (`decode.cpp`, `opcodes.inc`), a control-flow graph is built
(`translate.cpp`), and `emit_alu.cpp` / `emit_mem.cpp` lower the ISA to SPIR-V through a small
builder (`spv_builder.h`) with the attribute, constant buffer, texture and image bindings
mapped from the pipeline state. `program.cpp` assembles a complete pipeline program and adds
the epilogue that applies our viewport scaling.

`gpu/vk/vk_renderer.cpp` is the Vulkan backend: pipeline state is derived from the Maxwell
registers (vertex attribute formats, blending, depth/stencil, culling with the Maxwell window
origin rule, render target formats), pipelines are created asynchronously and cached on disk,
and the draw path uses several caches (program state, texture descriptors, index ranges, vertex
buffer slices, redundant state elimination) to stay around 25 microseconds per draw.
`texture_cache.cpp` manages guest textures and render targets: block-linear deswizzling, format
conversion (including ASTC decoding with a disk cache), aliasing between differently sized
views of the same memory, CPU-write tracking and GPU-write ordering so that post-processing
chains read what the game last wrote. The same cache implements the **internal resolution
scaler**: render targets are allocated at a scaled physical size while the guest keeps seeing
its native dimensions, with viewports, scissors, blits and copies rescaled per axis. That is
also what makes **ultrawide** work: the targets are scaled non-uniformly to the window and the
projection is corrected in the shader epilogue, so 3D fills the screen while the 2D UI stays
correctly proportioned.

`display.cpp` owns the window and the swapchain, presents at a fixed 60 Hz with a precise
limiter (the game's logic is frame-locked, so presentation never changes its speed), and hosts
the Dear ImGui overlay (`overlay.cpp`, `settings.cpp`).

### 5. Why native, not emulated

* No instruction decoding at run time and no JIT warm-up: the whole game is C compiled with
  `-O2 /arch:AVX2`.
* Guest threads are host threads and SVCs are function calls, so synchronisation is cheap and
  the scheduler is the OS scheduler.
* Guest memory is host memory: no page-table lookups, no fault handling on the hot path.
* The shader translator runs ahead of time on the game's own shader packs and its output is
  cached, so there is no in-game stutter from shader discovery after the first run.

---

## Repository layout

```
tools/recomp/      ARM64 -> C recompiler (a64.py, a64_simd.py, gen.py, gen_main.py)
tools/nso.py       NSO loader / relocator
tools/nx_extract.py  NSP -> exefs/ romfs/ extraction (for the recompiler's input)
tools/build.py     clang-cl + Ninja build driver;  make_dist.py: release zip;  iterate.sh: entry-point loop
runtime/include/   Ctx (guest register file), dispatch and module interfaces used by generated code
runtime/src/       core (loader, dispatch, crash reporting), kernel/, service/, fs/, hid/, audio/, video/, gpu/, shader/
third_party/       imgui, Vulkan and SPIR-V headers (submodules)
docs/              design decisions
```

## Environment variables (debugging)

`HWDER_NSP`, `HWDER_KEYS`, `HWDER_EXEFS` and `HWDER_ROMFS` select the game data and `HWDER_NO_DUMP=1`
turns crash minidumps off (the loader reads these before the runtime starts). Everything else:

<!-- debug-flags:begin -->
Every `HWDER_*` switch the runtime reads, generated from `runtime/src/debug_flags.cpp` by
`tools/gen_debug_flags_md.py` (do not edit by hand; `python tools/gen_debug_flags_md.py --write`
refreshes it). Each is read once, at startup; the F1 overlay's Tools tab lists them with their
current values. Kinds:

- **presence**: set to anything (even empty) turns it on.
- **on unless 0**: only the exact value `0` turns it off.
- **nonzero**: on when set to a value that does not start with `0`.
- **integer**: the number is used when set; the site's own default otherwise.
- **number**: the number is used when set; the site's own default otherwise.
- **string**: the text is used when set; see the meaning for its format.

| Variable | Kind | Default | Meaning |
|---|---|---|---|
| `HWDER_ALL_CORES` | presence | off | Hybrid CPUs: let the game run on efficiency cores too (by default it prefers the performance cores). |
| `HWDER_ASYNC_RT_PIPELINES` | presence | off | Renderer: also compile pipelines asynchronously for draws that sample a render target (comparison; they default to synchronous). |
| `HWDER_AS_TRACE` | presence | off | nvdrv: log GPU address-space reservations, mappings and InitializeEx (as-gpu:). Read on every call. |
| `HWDER_AUTOPLAY` | integer | unset | Unattended runs: 1 taps A through the menus; 2 drives into a battle and keeps fighting. |
| `HWDER_AUTOPLAY_CAM` | presence | off | Autoplay: also turn the camera with the right stick (2 s right, 2 s left, 2 s still). |
| `HWDER_AUTOPLAY_CIRCLE` | presence | off | Autoplay: run in a slow circle (one turn every 8 s) so the view covers every direction. |
| `HWDER_AUTOPLAY_IDLE` | presence | off | Autoplay: no input once in battle (experiments). |
| `HWDER_AUTOPLAY_PLUS` | presence | off | Test: tap + five times a second for the first 30 s (players who press + through the startup and title screens). |
| `HWDER_AUTOPLAY_SKIP` | presence | off | Autoplay: the 6 s cutscene-skip cycle for a save that starts with the story cutscenes. |
| `HWDER_CACHE_PROFILE` | presence | off | Pipeline cache / warmup save: log retrieval, snapshot, validation, serialization and disk timings (cache-save:). Read on every save. |
| `HWDER_CACHE_SAVE_SYNC` | string | unset | Exactly "1": save the warmup records synchronously instead of on the background task. |
| `HWDER_DRAWLOG` | presence | off | With HWDER_FILMSTRIP: one line per draw into <filmstrip dir>/drawlog.txt while the capture window is open. |
| `HWDER_DUMP_AFTER` | number | unset | HWDER_DUMP_FRAMES: no dumps (and no dump stalls) before that many seconds. |
| `HWDER_DUMP_EVERY` | integer | unset | HWDER_DUMP_FRAMES: dump every n-th presented frame (each dump waits for the GPU). |
| `HWDER_DUMP_FRAMES` | string | unset | Directory for frame_<n>.bmp dumps of presented frames. |
| `HWDER_DUMP_SHADERS` | presence | off | Write every translated shader's SPIR-V and Maxwell listing beside the executable. |
| `HWDER_FD_RAW` | presence | off | Frame dump: also write raw vertex streams, index buffers and vertex-stage cbufs into fd/. |
| `HWDER_FILMSTRIP` | string | unset | Directory for a 96x54 thumbnail of every presented frame (f_<n>.raw + index.txt). |
| `HWDER_FILMSTRIP_AFTER` | number | unset | Filmstrip: start capturing at that many seconds since boot. |
| `HWDER_FILMSTRIP_BATTLE` | string | unset | Filmstrip: start that many seconds (atof) after the first battle-sized frame (> 450 draws). |
| `HWDER_FILMSTRIP_FOR` | number | unset | Filmstrip: stop capturing after that many seconds. |
| `HWDER_FILMSTRIP_W` | integer | unset | Filmstrip: thumbnail width (16:9, at least 16). |
| `HWDER_FORCE_INTERVAL1` | presence | off | Presenter: release every frame on the 60 Hz grid even when the game asks for swap interval 2 (experiment). |
| `HWDER_FPS` | number | unset | Presenter release rate for experiments; ignored by the 120 fps mode's per-frame rates. |
| `HWDER_FS_SELFTEST` | nonzero | off | RomFS: run the image self test after mounting. |
| `HWDER_FS_TRACE` | nonzero | off | File system service trace. |
| `HWDER_GAME_INTERP` | integer | unset | 0/1: override the 120 fps (game-drawn in-between frame) setting for tests. |
| `HWDER_GI_LOG` | presence | off | 120 fps mode: log its timing and skip reasons periodically. |
| `HWDER_GI_LOG_MS` | integer | unset | Period of the HWDER_GI_LOG reports in ms (strtoull, base 10). |
| `HWDER_GI_NO_OBJECTS` | presence | off | 120 fps mode: interpolate the camera only, no object/bone blending (comparison). |
| `HWDER_GI_OBJ_SYNC` | presence | off | 120 fps mode: blend objects on the GPU command thread instead of their own thread (comparison). |
| `HWDER_GI_OBJ_TRACE` | presence | off | 120 fps mode: once a second, trace the c3 blocks nearest the player and the other vertex cbufs with transforms. |
| `HWDER_GPU_TRACE` | string | unset | GPU command trace; "2": raw 3D method stream (first 200000); "3": host methods, class binds and submissions. |
| `HWDER_GUEST_LIBC` | presence | off | Run the game's own (translated) memcpy/memmove/memset/bzero/strlen instead of the host C library's. |
| `HWDER_HOLD` | presence | off | On a fatal error keep the window up showing the reason instead of exiting. |
| `HWDER_IPC_BT` | string | unset | Log a backtrace for the IPC command named "<Service>#<command>" (e.g. IFile#4). |
| `HWDER_IPC_DUMP` | string | unset | Dump the raw request/response bytes of IPC calls whose target service name contains the value (empty: all). |
| `HWDER_IPC_TRACE` | presence | off | Service call (IPC) trace, including Control requests. |
| `HWDER_KEYS` | presence | off | prod.keys path (the loader reads it as a wide string; the startup screen only notes that it is set). |
| `HWDER_LOG` | presence | off | Log file path (set by test launchers); when set the window opens minimized unless HWDER_SHOW is set. |
| `HWDER_MUTE` | presence | off | No audio output; also: no message boxes, and with HWDER_OFFSCREEN marks an automated run. |
| `HWDER_NO_PRELAUNCH` | presence | off | Skip the startup screen. |
| `HWDER_NSP` | presence | off | Game NSP path (read as a wide string by the loader; the startup screen only notes that it is set). |
| `HWDER_NV_TRACE` | presence | off | nvdrv ioctl trace. |
| `HWDER_OCCLUSION_ONE` | presence | off | Report the occlusion probe query as 1 sample instead of fully visible. |
| `HWDER_OFFSCREEN` | presence | off | Open the window off-screen and never activate it (automated runs). |
| `HWDER_OVERLAY` | presence | off | Show the F1 overlay at startup. |
| `HWDER_PACE_TRACE` | presence | off | Frame pacing trace: one line per present (display) and per queued buffer (vi). |
| `HWDER_PATCH_DT` | number | unset | Loader experiment: patch the game's frame-delta constants to 1/<Hz> (>= 30). |
| `HWDER_PIPELINE_CATALOG` | on unless 0 | on | Use pipeline_catalog.bin at startup ("0" disables). |
| `HWDER_PIPELINE_CATALOG_RECORD` | string | unset | Record a metadata pipeline catalog for the named scope (at most 128 chars). |
| `HWDER_PIPELINE_PREWARM` | on unless 0 | on | Early pipeline prebuilding from pipeline_warmup.bin ("0" disables, recording stays on). |
| `HWDER_PRELAUNCH_FRAMES` | integer | unset | Leave the startup screen after that many frames (tests). |
| `HWDER_PRESENT_TRACE` | presence | off | One line per present: address switches, stale re-presents, movie cadence (ptrace:). |
| `HWDER_PROFILE_MS` | number | unset | Log the draw-path profile when a flush accumulated more than that many ms. |
| `HWDER_RES_SCALE` | number | unset | Internal resolution scale override (0.5 to 4). |
| `HWDER_RES_SCALE_X` | number | unset | Per-axis resolution scale override, X (test aid: emulate another window aspect). |
| `HWDER_RES_SCALE_Y` | number | unset | Per-axis resolution scale override, Y. |
| `HWDER_SAMPLE_US` | integer | unset | Linux: sample the GPU command thread and, in battle, MainThread every that many microseconds of their CPU time; counts per function, per MainThread tick phase and per address go to <log>.samples.txt and <log>.samples-main.txt every 10 s. |
| `HWDER_SHADER_FALLBACK` | string | unset | "all": replace every stage, "frag": only fragment shaders, with the solid-magenta fallback. |
| `HWDER_SHADER_NO_KIL` | presence | off | Shader translation: turn every (conditional) KIL into a no-op (alpha-test diagnosis). |
| `HWDER_SHOW` | presence | off | Show the window normally even in an automated (HWDER_LOG) run. |
| `HWDER_SYNC_PIPELINES` | presence | off | Compile every pipeline synchronously (the F1 async-shaders option off sets this too). |
| `HWDER_SYNC_TRACE` | presence | off | nvdrv syncpoint wait / signal trace. |
| `HWDER_THREAD_DUMP` | integer | unset | Dump all guest threads to the log every n seconds. |
| `HWDER_THREAD_STATS` | integer | unset | Log per-thread CPU, waits and efficiency-core share every n ms, guest sleeps and the per-frame GPU thread profile (turns the draw profiler on). |
| `HWDER_TIMELINE` | number | unset | Linux/Windows: from that many seconds after start, record 0.5 s of frame-loop events (MainThread phases, GPU thread busy/idle, GPFIFO submissions, syncpoints, vi queue/dequeue, compositor) to <log>.timeline.txt. |
| `HWDER_TOTAL_MEM_MB` | integer | unset | The TotalMemorySize the game is told, in MiB (a Switch: 3285); overrides the Gameplay menu's Game memory option. Above 4096 the game takes its dev-kit memory layout; capped at the 6 GiB heap ceiling plus UsedMemorySize. |
| `HWDER_UPDATE` | presence | off | Folder of the game update unpacked by tools/game_update.py (read as a wide string by the loader; the startup screen notes it). |
| `HWDER_USED_MEM_MB` | integer | unset | Kernel: the fixed part of UsedMemorySize in MiB (default 48, the mapped code and main stack); 256 is the old figure that left the game a 208 MiB smaller heap. |
| `HWDER_VALIDATION` | presence | off | Enable the Vulkan validation layer (needs the Vulkan SDK). |
| `HWDER_VI_TRACE` | presence | off | Display service (vi) parcel trace. |
| `HWDER_VK_DUMP_CBUF` | string | unset | Frame dump: full cbuf contents of the fragment shader whose key starts with this hex prefix. |
| `HWDER_VK_DUMP_TEX` | presence | off | Frame dump: write the guest bytes of every sampled texture into fd/. |
| `HWDER_VK_FACE_RULE` | string | unset | Front-face experiment: atoi == 1 inverts on flip_y only, any other value never inverts. |
| `HWDER_VK_FRAME_DUMP` | string | unset | Log every draw/clear/blit of one frame: <present index>, "auto[<s>]" (first 3D frame, not before <s>), "t<s>" (time), "p<hex>" (program key prefix). |
| `HWDER_VK_FRAME_DUMP_AFTER` | number | unset | Frame dump "p" trigger: ignore matches before that many seconds. |
| `HWDER_VK_FRAME_DUMP_COUNT` | integer | unset | Frame dump: n consecutive presents from the trigger (strtoull, base 0, at least 1). |
| `HWDER_VK_FRAME_DUMP_DELAY` | number | unset | Frame dump auto trigger: arm the time trigger that many seconds after the draw-count trigger. |
| `HWDER_VK_FRAME_DUMP_MIN_DRAWS` | string | unset | Frame dump auto trigger: "min[-max]" draw-count bounds of the previous frame (strtoull, base 0). |
| `HWDER_VK_FRAME_DUMP_SKIP` | integer | unset | Frame dump auto trigger: dump the n-th present after the trigger (strtoull, base 0). |
| `HWDER_VK_INIT_DEPTH` | number | unset | Initial clear value of newly created depth images. |
| `HWDER_VK_NO_CULL` | presence | off | Disable face culling. |
| `HWDER_VK_SYNC_SUBMIT` | presence | off | Submit Vulkan work on the GPU command thread instead of its own submit thread (comparison). |
| `HWDER_VK_VERBOSE` | presence | off | Verbose renderer logging: the first draws, texture handles and clears. |
| `HWDER_VK_WHITE_TEX` | presence | off | Bind the white dummy texture instead of every sampled texture. |
| `HWDER_WAIT_TRACE` | presence | off | Kernel: log slow WaitSynchronization calls. |
| `HWDER_WATCH_PER_SERIAL` | presence | off | Query texture write watch once per recorder serial (every queue submit) instead of once per guest submission (A/B tests). |
<!-- debug-flags:end -->

Successful graphics pipelines are recorded with their shader code and binding metadata in
`pipeline_warmup.bin`. A compatible file prebuilds those recipes on the single pipeline
worker before the startup screen and guest boot. Warmed results are reused only after
fresh live translation exactly matches the saved shader and metadata. Absent or incompatible
files leave startup lazy. `HWDER_PIPELINE_PREWARM=0` disables early prebuilding for comparison
while continuing to record successful live pipelines. The file is disposable; delete it to
start fresh. Logs report startup costs, completion before the first live draw, adoption,
mismatches, failures and recording. Performance and visual acceptance require matched runs.

## Public synthetic tests

`python tools/tests/run_synthetic.py --jobs 2` runs the cache/catalog codecs and
compiled A64 arithmetic regressions without a game dump, generated game code,
keys, saves, Vulkan device or private cache. See [test setup and coverage](docs/synthetic-tests.md).
Source-package exclusion tests run automatically when the separate first-run
candidate is integrated; their current absence is reported explicitly.

## Status

Boots, menus, movies, Adventure/Legend mode battles, saves and audio work; gameplay runs at the
game's native 60 Hz (30 Hz where the game itself drops to half rate) at native or scaled
resolution. Known issues are tracked in the issue tracker.

## License

MIT for everything in this repository (see `LICENSE`). Hyrule Warriors: Definitive Edition is
the property of Nintendo and Koei Tecmo; you need your own copy of the game.


### Metadata pipeline catalog prototype

Custom field-of-view adjustment is temporarily hidden because wider FOV can misplace officer nameplates. The game uses its default FOV (100%); older saved `fov=` overrides are ignored. This does not disable the separate ultrawide aspect-ratio option.

`pipeline_catalog.bin` is a separate metadata-only format for collecting the pipelines observed on a named stage/route. Set `HWDER_PIPELINE_CATALOG_RECORD=sealed-grounds` for a collection run. Successful requested graphics pipelines periodically export their shader resource locators, SHA256 identities, translation settings and complete static pipeline state. The scope name describes the run; it does not promise complete stage or game coverage. No game shader code, SPIR-V, native pipeline cache or key material is exported. Keep `pipeline_warmup.bin` private: it contains compiled shader code and is **not** this catalog.

With a compatible catalog beside the executable, startup first lets you select your game, then resolves the original programs from **your selected local NSP or extracted RomFS**, verifies their contents and translates/compiles locally. The graphics preparation screen reports progress and waits for known jobs before guest boot. Every warmed result still requires the existing fresh full live-program/static-state verification before use. Missing, incompatible, changed or unsupported sources retain normal private-cache/lazy compilation. `HWDER_PIPELINE_CATALOG=0` disables catalog import; `HWDER_PIPELINE_PREWARM=0` disables startup prebuilding while recording remains available.

This first prototype supports raw NVN programs in four actual shader archives: `data/action/shader/Normal.bin.gz`, `data/deferred/DeferredShader.bin.gz`, `data/shadow/ShadowShader.bin.gz`, and `data/stage/common/StageShader.bin.gz`. Their shader chunks are directly readable despite the `.gz` filenames. Other compressed/relocated resource variants, VertexA merging and translator callbacks requiring uncaptured constant-buffer/texture context are deliberately unresolved. Collection logs report these gaps; no previously generated shader cache substitutes for source resolution. Catalog compatibility includes the compiled build/ABI, HWDE title and game version; catalogs must be regenerated when these change.

Offline checks and a read-only source proof tool use the same required build lock:

```
python tools/build.py --target pipeline_catalog_test
build\release\pipeline_catalog_test.exe
python tools/build.py --target pipeline_catalog_tool
build\release\pipeline_catalog_tool.exe "your-game.nsp" "your-prod.keys" "build\release"
```

The tool reads the user's game and prints metadata counts without writing shader or game bytes. Its optional trailing arguments `catalog.bin build-tag game-version` verify every catalog program by fresh local resolution/translation without persisting compiled output. The synthetic tests generate their own tiny programs in memory and cover format integrity, compatibility, bounds, deterministic identity/roundtrip and safe unresolved cases. Runtime evidence and exact one-stage coverage are recorded separately in `../docs/graphics-coordination/catalog-agent.md`.
