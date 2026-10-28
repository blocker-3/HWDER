# Environment variables (debugging)

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
| `HWDER_AUTOPLAY_X` | presence | off | Autoplay: press X once, 1 s into the battle (the heavy-fight save: starts the special mode). |
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
| `HWDER_GI_KEEP_30` | presence | off | 120 fps mode: keep drawing extra frames while the game runs at 30 fps (comparison; they are off there by default). |
| `HWDER_GI_LOG` | presence | off | 120 fps mode: log its timing and skip reasons periodically. |
| `HWDER_GI_LOG_MS` | integer | unset | Period of the HWDER_GI_LOG reports in ms (strtoull, base 10). |
| `HWDER_GI_NO_OBJECTS` | presence | off | 120 fps mode: interpolate the camera only, no object/bone blending (comparison). |
| `HWDER_GI_OBJ_SYNC` | presence | off | 120 fps mode: blend objects on the GPU command thread instead of their own thread (comparison). |
| `HWDER_GI_OBJ_TRACE` | presence | off | 120 fps mode: once a second, trace the c3 blocks nearest the player and the other vertex cbufs with transforms. |
| `HWDER_GPU_TIMING` | number | unset | Renderer: GPU timestamps around every rendering instance; every that many seconds log the GPU time per frame, the idle time between submissions and the costliest passes (gpu-time:). Adds a timestamp per pass. |
| `HWDER_GPU_TRACE` | string | unset | GPU command trace; "2": raw 3D method stream (first 200000); "3": host methods, class binds and submissions. |
| `HWDER_GUEST_LIBC` | presence | off | Run the game's own (translated) memcpy/memmove/memset/bzero/strlen instead of the host C library's. |
| `HWDER_HOLD` | presence | off | On a fatal error keep the window up showing the reason instead of exiting. |
| `HWDER_IPC_BT` | string | unset | Log a backtrace for the IPC command named "<Service>#<command>" (e.g. IFile#4). |
| `HWDER_IPC_DUMP` | string | unset | Dump the raw request/response bytes of IPC calls whose target service name contains the value (empty: all). |
| `HWDER_IPC_TRACE` | presence | off | Service call (IPC) trace, including Control requests. |
| `HWDER_KEYS` | presence | off | prod.keys path (the loader reads it as a wide string; the startup screen only notes that it is set). |
| `HWDER_LOG` | presence | off | Log file path (set by test launchers); when set the window opens minimized unless HWDER_SHOW is set. |
| `HWDER_MODS` | string | unset | Mods folder to use instead of the one in the settings or the default (read as a path; see fs/mods.h). |
| `HWDER_MUTE` | presence | off | No audio output; also: no message boxes, and with HWDER_OFFSCREEN marks an automated run. |
| `HWDER_NO_MODS` | nonzero | off | Load no mods from the mods folder (the game RomFS is passed through untouched). |
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
