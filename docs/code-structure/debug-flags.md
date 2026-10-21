# Debug flags (HWDER_* environment variables)

Inventory of every `getenv("HWDER_...")` in `runtime/src` at code-structure commit 9a1b790, before the table-driven conversion (plan.md finding 3). The table below is the source of `runtime/src/debug_flags.{h,cpp}` (`debug::table()`); the per-site list records the parse rule each site had and the accessor it was converted to. Behaviour is unchanged: the accessors call `getenv` when invoked, and every call site keeps its `static const` caching, so each variable is still read once, at the same point as before.

76 variables, 115 read sites (source lines; a line that reads one variable twice, as in `set ? atof(value) : d`, counts once).

## Variables

| Variable | Kind | Default | Meaning | Sites |
|---|---|---|---|---|
| `HWDER_ASYNC_RT_PIPELINES` | presence | - | Renderer: also compile pipelines asynchronously for draws that sample a render target (comparison; they default to synchronous). | 1 |
| `HWDER_AS_TRACE` | presence | - | nvdrv: log GPU address-space reservations, mappings and InitializeEx (as-gpu:). Read on every call. | 4 |
| `HWDER_AUTOPLAY` | int | unset | Unattended runs: 1 taps A through the menus; 2 drives into a battle and keeps fighting. | 2 |
| `HWDER_AUTOPLAY_CAM` | presence | - | Autoplay: also turn the camera with the right stick (2 s right, 2 s left, 2 s still). | 1 |
| `HWDER_AUTOPLAY_CIRCLE` | presence | - | Autoplay: run in a slow circle (one turn every 8 s) so the view covers every direction. | 1 |
| `HWDER_AUTOPLAY_IDLE` | presence | - | Autoplay: no input once in battle (experiments). | 1 |
| `HWDER_AUTOPLAY_SKIP` | presence | - | Autoplay: the 6 s cutscene-skip cycle for a save that starts with the story cutscenes. | 1 |
| `HWDER_CACHE_PROFILE` | presence | - | Pipeline cache / warmup save: log retrieval, snapshot, validation, serialization and disk timings (cache-save:). Read on every save. | 6 |
| `HWDER_CACHE_SAVE_SYNC` | string | unset | Exactly "1": save the warmup records synchronously instead of on the background task. | 1 |
| `HWDER_DRAWLOG` | presence | - | With HWDER_FILMSTRIP: one line per draw into <filmstrip dir>/drawlog.txt while the capture window is open. | 1 |
| `HWDER_DUMP_AFTER` | float | 0 | HWDER_DUMP_FRAMES: no dumps (and no dump stalls) before that many seconds. | 1 |
| `HWDER_DUMP_EVERY` | int | 600 | HWDER_DUMP_FRAMES: dump every n-th presented frame (each dump waits for the GPU). | 1 |
| `HWDER_DUMP_FRAMES` | string | unset | Directory for frame_<n>.bmp dumps of presented frames. | 1 |
| `HWDER_DUMP_SHADERS` | presence | - | Write every translated shader's SPIR-V and Maxwell listing beside the executable. | 1 |
| `HWDER_FD_RAW` | presence | - | Frame dump: also write raw vertex streams, index buffers and vertex-stage cbufs into fd/. | 2 |
| `HWDER_FILMSTRIP` | string | unset | Directory for a 96x54 thumbnail of every presented frame (f_<n>.raw + index.txt). | 3 |
| `HWDER_FILMSTRIP_AFTER` | float | 0 | Filmstrip: start capturing at that many seconds since boot. | 1 |
| `HWDER_FILMSTRIP_BATTLE` | string | unset | Filmstrip: start that many seconds (atof) after the first battle-sized frame (> 450 draws). | 1 |
| `HWDER_FILMSTRIP_FOR` | float | 1e9 | Filmstrip: stop capturing after that many seconds. | 1 |
| `HWDER_FILMSTRIP_W` | int | 96 | Filmstrip: thumbnail width (16:9, at least 16). | 1 |
| `HWDER_FORCE_INTERVAL1` | presence | - | Presenter: release every frame on the 60 Hz grid even when the game asks for swap interval 2 (experiment). | 1 |
| `HWDER_FPS` | float | 60 | Presenter release rate for experiments; ignored by the 120 fps mode's per-frame rates. | 1 |
| `HWDER_FS_SELFTEST` | nonzero | - | RomFS: run the image self test after mounting. | 1 |
| `HWDER_FS_TRACE` | nonzero | - | File system service trace. | 1 |
| `HWDER_GAME_INTERP` | int | the setting | 0/1: override the 120 fps (game-drawn in-between frame) setting for tests. | 1 |
| `HWDER_GI_LOG` | presence | - | 120 fps mode: log its timing and skip reasons periodically. | 1 |
| `HWDER_GI_LOG_MS` | int | 5000 | Period of the HWDER_GI_LOG reports in ms (strtoull, base 10). | 1 |
| `HWDER_GI_NO_OBJECTS` | presence | - | 120 fps mode: interpolate the camera only, no object/bone blending (comparison). | 1 |
| `HWDER_GI_OBJ_TRACE` | presence | - | 120 fps mode: once a second, trace the c3 blocks nearest the player and the other vertex cbufs with transforms. | 3 |
| `HWDER_GPU_TRACE` | string | unset | GPU command trace; "2": raw 3D method stream (first 200000); "3": host methods, class binds and submissions. | 4 |
| `HWDER_HOLD` | presence | - | On a fatal error keep the window up showing the reason instead of exiting. | 1 |
| `HWDER_IPC_BT` | string | unset | Log a backtrace for the IPC command named "<Service>#<command>" (e.g. IFile#4). | 1 |
| `HWDER_IPC_DUMP` | string | unset | Dump the raw request/response bytes of IPC calls whose target service name contains the value (empty: all). | 1 |
| `HWDER_IPC_TRACE` | presence | - | Service call (IPC) trace, including Control requests. | 2 |
| `HWDER_KEYS` | presence | - | prod.keys path (the loader reads it as a wide string; the startup screen only notes that it is set). | 1 |
| `HWDER_LOG` | presence | - | Log file path (set by test launchers); when set the window opens minimized unless HWDER_SHOW is set. | 1 |
| `HWDER_MUTE` | presence | - | No audio output; also: no message boxes, and with HWDER_OFFSCREEN marks an automated run. | 6 |
| `HWDER_NO_PRELAUNCH` | presence | - | Skip the startup screen. | 1 |
| `HWDER_NSP` | presence | - | Game NSP path (read as a wide string by the loader; the startup screen only notes that it is set). | 1 |
| `HWDER_NV_TRACE` | presence | - | nvdrv ioctl trace. | 1 |
| `HWDER_OCCLUSION_ONE` | presence | - | Report the occlusion probe query as 1 sample instead of fully visible. | 1 |
| `HWDER_OFFSCREEN` | presence | - | Open the window off-screen and never activate it (automated runs). | 2 |
| `HWDER_OVERLAY` | presence | - | Show the F1 overlay at startup. | 1 |
| `HWDER_PACE_TRACE` | presence | - | Frame pacing trace: one line per present (display) and per queued buffer (vi). | 2 |
| `HWDER_PATCH_DT` | float | unset | Loader experiment: patch the game's frame-delta constants to 1/<Hz> (>= 30). | 1 |
| `HWDER_PIPELINE_CATALOG` | on-unless-0 | on | Use pipeline_catalog.bin at startup ("0" disables). | 1 |
| `HWDER_PIPELINE_CATALOG_RECORD` | string | unset | Record a metadata pipeline catalog for the named scope (at most 128 chars). | 2 |
| `HWDER_PIPELINE_PREWARM` | on-unless-0 | on | Early pipeline prebuilding from pipeline_warmup.bin ("0" disables, recording stays on). | 1 |
| `HWDER_PRELAUNCH_FRAMES` | int | 0 | Leave the startup screen after that many frames (tests). | 1 |
| `HWDER_PRESENT_TRACE` | presence | - | One line per present: address switches, stale re-presents, movie cadence (ptrace:). | 1 |
| `HWDER_PROFILE_MS` | float | 200 | Log the draw-path profile when a flush accumulated more than that many ms. | 1 |
| `HWDER_RES_SCALE` | float | the setting | Internal resolution scale override (0.5 to 4). | 2 |
| `HWDER_RES_SCALE_X` | float | unset | Per-axis resolution scale override, X (test aid: emulate another window aspect). | 2 |
| `HWDER_RES_SCALE_Y` | float | unset | Per-axis resolution scale override, Y. | 2 |
| `HWDER_SHADER_FALLBACK` | string | unset | "all": replace every stage, "frag": only fragment shaders, with the solid-magenta fallback. | 2 |
| `HWDER_SHADER_NO_KIL` | presence | - | Shader translation: turn every (conditional) KIL into a no-op (alpha-test diagnosis). | 2 |
| `HWDER_SHOW` | presence | - | Show the window normally even in an automated (HWDER_LOG) run. | 1 |
| `HWDER_SYNC_PIPELINES` | presence | - | Compile every pipeline synchronously (the F1 async-shaders option off sets this too). | 2 |
| `HWDER_SYNC_TRACE` | presence | - | nvdrv syncpoint wait / signal trace. | 3 |
| `HWDER_THREAD_DUMP` | int | 0 (off) | Dump all guest threads to the log every n seconds. | 1 |
| `HWDER_VALIDATION` | presence | - | Enable the Vulkan validation layer (needs the Vulkan SDK). | 1 |
| `HWDER_VI_TRACE` | presence | - | Display service (vi) parcel trace. | 1 |
| `HWDER_VK_DUMP_CBUF` | string | unset | Frame dump: full cbuf contents of the fragment shader whose key starts with this hex prefix. | 1 |
| `HWDER_VK_DUMP_TEX` | presence | - | Frame dump: write the guest bytes of every sampled texture into fd/. | 1 |
| `HWDER_VK_FACE_RULE` | string | unset | Front-face experiment: atoi == 1 inverts on flip_y only, any other value never inverts. | 1 |
| `HWDER_VK_FRAME_DUMP` | string | unset | Log every draw/clear/blit of one frame: <present index>, "auto[<s>]" (first 3D frame, not before <s>), "t<s>" (time), "p<hex>" (program key prefix). | 5 |
| `HWDER_VK_FRAME_DUMP_AFTER` | float | 0 | Frame dump "p" trigger: ignore matches before that many seconds. | 1 |
| `HWDER_VK_FRAME_DUMP_COUNT` | int | 1 | Frame dump: n consecutive presents from the trigger (strtoull, base 0, at least 1). | 1 |
| `HWDER_VK_FRAME_DUMP_DELAY` | float | 0 | Frame dump auto trigger: arm the time trigger that many seconds after the draw-count trigger. | 1 |
| `HWDER_VK_FRAME_DUMP_MIN_DRAWS` | string | 600 | Frame dump auto trigger: "min[-max]" draw-count bounds of the previous frame (strtoull, base 0). | 1 |
| `HWDER_VK_FRAME_DUMP_SKIP` | int | 0 | Frame dump auto trigger: dump the n-th present after the trigger (strtoull, base 0). | 1 |
| `HWDER_VK_INIT_DEPTH` | float | 1.0 | Initial clear value of newly created depth images. | 1 |
| `HWDER_VK_NO_CULL` | presence | - | Disable face culling. | 1 |
| `HWDER_VK_VERBOSE` | presence | - | Verbose renderer logging: the first draws, texture handles and clears. | 1 |
| `HWDER_VK_WHITE_TEX` | presence | - | Bind the white dummy texture instead of every sampled texture. | 2 |
| `HWDER_WAIT_TRACE` | presence | - | Kernel: log slow WaitSynchronization calls. | 1 |

Kinds: `presence` = set at all (`getenv() != nullptr`); `on-unless-0` = on unless the value is exactly "0"; `nonzero` = set, non-empty and not starting with '0'; `int` = atoi (or strtoull where noted); `float` = atof; `string` = the site parses the text itself.

## Read sites

| Variable | Site | Parse rule and accessor |
|---|---|---|
| `HWDER_MUTE` | `runtime/src/audio/audio.cpp:76` | presence -> flag(id) |
| `HWDER_MUTE` | `runtime/src/audio/audio.cpp:128` | presence -> flag(id) |
| `HWDER_HOLD` | `runtime/src/core.cpp:224` | presence -> flag(id) |
| `HWDER_THREAD_DUMP` | `runtime/src/core.cpp:425` | site keeps its own parse of the string -> str(id) |
| `HWDER_PATCH_DT` | `runtime/src/core.cpp:519` | site keeps its own parse of the string -> str(id) |
| `HWDER_MUTE` | `runtime/src/core.cpp:630` | presence -> flag(id) |
| `HWDER_OFFSCREEN` | `runtime/src/core.cpp:630` | presence -> flag(id) |
| `HWDER_NO_PRELAUNCH` | `runtime/src/core.cpp:631` | presence -> flag(id) |
| `HWDER_MUTE` | `runtime/src/core.cpp:638` | presence -> flag(id) |
| `HWDER_FS_TRACE` | `runtime/src/fs/fs.cpp:46` | site keeps its own parse of the string -> str(id) |
| `HWDER_FS_SELFTEST` | `runtime/src/fs/romfs.cpp:415` | site keeps its own parse of the string -> str(id) |
| `HWDER_MUTE` | `runtime/src/fs/save_dir.cpp:172` | presence -> flag(id) |
| `HWDER_OFFSCREEN` | `runtime/src/gpu/display.cpp:146` | presence -> flag(id) |
| `HWDER_LOG` | `runtime/src/gpu/display.cpp:157` | presence -> flag(id) |
| `HWDER_SHOW` | `runtime/src/gpu/display.cpp:157` | presence -> flag(id) |
| `HWDER_VALIDATION` | `runtime/src/gpu/display.cpp:260` | presence -> flag(id) |
| `HWDER_FPS` | `runtime/src/gpu/display.cpp:412` | site keeps its own parse of the string -> str(id) |
| `HWDER_FORCE_INTERVAL1` | `runtime/src/gpu/display.cpp:428` | presence -> flag(id) |
| `HWDER_PACE_TRACE` | `runtime/src/gpu/display.cpp:699` | presence -> flag(id) |
| `HWDER_GPU_TRACE` | `runtime/src/gpu/gpu.cpp:28` | presence -> flag(id) |
| `HWDER_OCCLUSION_ONE` | `runtime/src/gpu/gpu.cpp:354` | presence -> flag(id) |
| `HWDER_GPU_TRACE` | `runtime/src/gpu/gpu.cpp:417` | site keeps its own parse of the string -> str(id) |
| `HWDER_GPU_TRACE` | `runtime/src/gpu/gpu.cpp:730` | site keeps its own parse of the string -> str(id) |
| `HWDER_GPU_TRACE` | `runtime/src/gpu/gpu.cpp:888` | site keeps its own parse of the string -> str(id) |
| `HWDER_AS_TRACE` | `runtime/src/gpu/nvdrv.cpp:361` | presence -> flag(id) |
| `HWDER_AS_TRACE` | `runtime/src/gpu/nvdrv.cpp:419` | presence -> flag(id) |
| `HWDER_AS_TRACE` | `runtime/src/gpu/nvdrv.cpp:441` | presence -> flag(id) |
| `HWDER_AS_TRACE` | `runtime/src/gpu/nvdrv.cpp:452` | presence -> flag(id) |
| `HWDER_SYNC_TRACE` | `runtime/src/gpu/nvdrv.cpp:491` | presence -> flag(id) |
| `HWDER_SYNC_TRACE` | `runtime/src/gpu/nvdrv.cpp:549` | presence -> flag(id) |
| `HWDER_SYNC_TRACE` | `runtime/src/gpu/nvdrv.cpp:739` | presence -> flag(id) |
| `HWDER_NV_TRACE` | `runtime/src/gpu/nvdrv.cpp:868` | presence -> flag(id) |
| `HWDER_OVERLAY` | `runtime/src/gpu/overlay.cpp:211` | presence -> flag(id) |
| `HWDER_SYNC_PIPELINES` | `runtime/src/gpu/overlay.cpp:563` | presence -> flag(id) |
| `HWDER_MUTE` | `runtime/src/gpu/overlay.cpp:570` | presence -> flag(id) |
| `HWDER_NSP` | `runtime/src/gpu/prelaunch.cpp:121` | presence -> flag(id) |
| `HWDER_KEYS` | `runtime/src/gpu/prelaunch.cpp:141` | presence -> flag(id) |
| `HWDER_PRELAUNCH_FRAMES` | `runtime/src/gpu/prelaunch.cpp:294` | site keeps its own parse of the string -> str(id) |
| `HWDER_PACE_TRACE` | `runtime/src/gpu/vi.cpp:314` | presence -> flag(id) |
| `HWDER_VI_TRACE` | `runtime/src/gpu/vi.cpp:507` | presence -> flag(id) |
| `HWDER_VK_INIT_DEPTH` | `runtime/src/gpu/vk/texture_cache.cpp:622` | float (set ? (float)atof : 1.0f) -> (float)value(id, 1.0) |
| `HWDER_RES_SCALE` | `runtime/src/gpu/vk/texture_cache.cpp:670` | site keeps its own parse of the string -> str(id) |
| `HWDER_RES_SCALE_X` | `runtime/src/gpu/vk/texture_cache.cpp:671` | site keeps its own parse of the string -> str(id) |
| `HWDER_RES_SCALE_Y` | `runtime/src/gpu/vk/texture_cache.cpp:672` | site keeps its own parse of the string -> str(id) |
| `HWDER_DRAWLOG` | `runtime/src/gpu/vk/vk_debug.cpp:14` | presence -> flag(id) |
| `HWDER_FILMSTRIP` | `runtime/src/gpu/vk/vk_debug.cpp:14` | presence -> flag(id) |
| `HWDER_FILMSTRIP` | `runtime/src/gpu/vk/vk_debug.cpp:17` | site keeps its own parse of the string -> str(id) |
| `HWDER_PROFILE_MS` | `runtime/src/gpu/vk/vk_debug.cpp:30` | float (set ? atof : default) -> value(id, default) |
| `HWDER_DUMP_FRAMES` | `runtime/src/gpu/vk/vk_debug.cpp:43` | site keeps its own parse of the string -> str(id) |
| `HWDER_DUMP_EVERY` | `runtime/src/gpu/vk/vk_debug.cpp:47` | int (set ? atoi : default) -> (u32)value(id, default) |
| `HWDER_DUMP_AFTER` | `runtime/src/gpu/vk/vk_debug.cpp:50` | float (set ? atof : default) -> value(id, default) |
| `HWDER_FILMSTRIP_W` | `runtime/src/gpu/vk/vk_debug.cpp:123` | presence -> flag(id) |
| `HWDER_FILMSTRIP_BATTLE` | `runtime/src/gpu/vk/vk_debug.cpp:127` | site keeps its own parse of the string -> str(id) |
| `HWDER_FILMSTRIP_FOR` | `runtime/src/gpu/vk/vk_debug.cpp:128` | float (set ? atof : default) -> value(id, default) |
| `HWDER_FILMSTRIP_AFTER` | `runtime/src/gpu/vk/vk_debug.cpp:129` | float (set ? atof : default) -> value(id, default) |
| `HWDER_VK_NO_CULL` | `runtime/src/gpu/vk/vk_draw.cpp:66` | presence -> flag(id) |
| `HWDER_VK_FACE_RULE` | `runtime/src/gpu/vk/vk_draw.cpp:87` | site keeps its own parse of the string -> str(id) |
| `HWDER_FD_RAW` | `runtime/src/gpu/vk/vk_draw.cpp:314` | presence -> flag(id) |
| `HWDER_VK_FRAME_DUMP_AFTER` | `runtime/src/gpu/vk/vk_draw.cpp:471` | float (set ? atof : default) -> value(id, default) |
| `HWDER_VK_DUMP_CBUF` | `runtime/src/gpu/vk/vk_draw.cpp:524` | site keeps its own parse of the string -> str(id) |
| `HWDER_FD_RAW` | `runtime/src/gpu/vk/vk_draw.cpp:558` | presence -> flag(id) |
| `HWDER_GI_OBJ_TRACE` | `runtime/src/gpu/vk/vk_interp_objects.cpp:135` | presence -> flag(id) |
| `HWDER_GI_NO_OBJECTS` | `runtime/src/gpu/vk/vk_interp_objects.cpp:174` | presence -> flag(id) |
| `HWDER_GI_OBJ_TRACE` | `runtime/src/gpu/vk/vk_interp_objects.cpp:194` | presence -> flag(id) |
| `HWDER_PIPELINE_CATALOG` | `runtime/src/gpu/vk/vk_pipelines.cpp:19` | site keeps its own parse of the string -> str(id) |
| `HWDER_SHADER_FALLBACK` | `runtime/src/gpu/vk/vk_pipelines.cpp:28` | presence -> flag(id) |
| `HWDER_SHADER_NO_KIL` | `runtime/src/gpu/vk/vk_pipelines.cpp:28` | presence -> flag(id) |
| `HWDER_PIPELINE_PREWARM` | `runtime/src/gpu/vk/vk_pipelines.cpp:31` | site keeps its own parse of the string -> str(id) |
| `HWDER_DUMP_SHADERS` | `runtime/src/gpu/vk/vk_pipelines.cpp:123` | presence -> flag(id) |
| `HWDER_PIPELINE_CATALOG_RECORD` | `runtime/src/gpu/vk/vk_pipelines.cpp:569` | site keeps its own parse of the string -> str(id) |
| `HWDER_CACHE_PROFILE` | `runtime/src/gpu/vk/vk_pipelines.cpp:600` | presence -> flag(id) |
| `HWDER_CACHE_SAVE_SYNC` | `runtime/src/gpu/vk/vk_pipelines.cpp:621` | presence -> flag(id) |
| `HWDER_CACHE_PROFILE` | `runtime/src/gpu/vk/vk_pipelines.cpp:623` | presence -> flag(id) |
| `HWDER_CACHE_PROFILE` | `runtime/src/gpu/vk/vk_pipelines.cpp:625` | presence -> flag(id) |
| `HWDER_CACHE_PROFILE` | `runtime/src/gpu/vk/vk_pipelines.cpp:632` | presence -> flag(id) |
| `HWDER_CACHE_PROFILE` | `runtime/src/gpu/vk/vk_pipelines.cpp:636` | presence -> flag(id) |
| `HWDER_SYNC_PIPELINES` | `runtime/src/gpu/vk/vk_pipelines.cpp:674` | presence -> flag(id) |
| `HWDER_ASYNC_RT_PIPELINES` | `runtime/src/gpu/vk/vk_pipelines.cpp:682` | presence -> flag(id) |
| `HWDER_PIPELINE_CATALOG_RECORD` | `runtime/src/gpu/vk/vk_pipelines.cpp:970` | site keeps its own parse of the string -> str(id) |
| `HWDER_PRESENT_TRACE` | `runtime/src/gpu/vk/vk_present.cpp:188` | presence -> flag(id) |
| `HWDER_FILMSTRIP` | `runtime/src/gpu/vk/vk_present.cpp:241` | site keeps its own parse of the string -> str(id) |
| `HWDER_RES_SCALE` | `runtime/src/gpu/vk/vk_present.cpp:297` | presence -> flag(id) |
| `HWDER_RES_SCALE_X` | `runtime/src/gpu/vk/vk_present.cpp:297` | presence -> flag(id) |
| `HWDER_RES_SCALE_Y` | `runtime/src/gpu/vk/vk_present.cpp:297` | presence -> flag(id) |
| `HWDER_VK_FRAME_DUMP_MIN_DRAWS` | `runtime/src/gpu/vk/vk_present.cpp:352` | site keeps its own parse of the string -> str(id) |
| `HWDER_VK_FRAME_DUMP_SKIP` | `runtime/src/gpu/vk/vk_present.cpp:360` | u64 (set ? strtoull : default) -> value(id, default, base) |
| `HWDER_VK_FRAME_DUMP_DELAY` | `runtime/src/gpu/vk/vk_present.cpp:363` | float (set ? atof : default) -> value(id, default) |
| `HWDER_CACHE_PROFILE` | `runtime/src/gpu/vk/vk_renderer.h:729` | presence -> flag(id) |
| `HWDER_VK_VERBOSE` | `runtime/src/gpu/vk/vk_renderer.h:767` | presence -> flag(id) |
| `HWDER_VK_FRAME_DUMP` | `runtime/src/gpu/vk/vk_renderer.h:770` | presence -> flag(id) |
| `HWDER_VK_FRAME_DUMP` | `runtime/src/gpu/vk/vk_renderer.h:771` | site keeps its own parse of the string -> str(id) |
| `HWDER_VK_FRAME_DUMP` | `runtime/src/gpu/vk/vk_renderer.h:773` | presence -> flag(id) |
| `HWDER_VK_FRAME_DUMP` | `runtime/src/gpu/vk/vk_renderer.h:776` | presence -> flag(id) |
| `HWDER_VK_FRAME_DUMP` | `runtime/src/gpu/vk/vk_renderer.h:777` | presence -> flag(id) |
| `HWDER_VK_FRAME_DUMP_COUNT` | `runtime/src/gpu/vk/vk_renderer.h:780` | presence -> flag(id) |
| `HWDER_VK_WHITE_TEX` | `runtime/src/gpu/vk/vk_resources.cpp:11` | presence -> flag(id) |
| `HWDER_GI_OBJ_TRACE` | `runtime/src/gpu/vk/vk_resources.cpp:254` | presence -> flag(id) |
| `HWDER_VK_WHITE_TEX` | `runtime/src/gpu/vk/vk_resources.cpp:357` | presence -> flag(id) |
| `HWDER_VK_DUMP_TEX` | `runtime/src/gpu/vk/vk_resources.cpp:462` | presence -> flag(id) |
| `HWDER_AUTOPLAY` | `runtime/src/hid/hid.cpp:285` | presence -> flag(id) |
| `HWDER_AUTOPLAY` | `runtime/src/hid/hid.cpp:289` | site keeps its own parse of the string -> str(id) |
| `HWDER_AUTOPLAY_SKIP` | `runtime/src/hid/hid.cpp:315` | presence -> flag(id) |
| `HWDER_AUTOPLAY_IDLE` | `runtime/src/hid/hid.cpp:349` | presence -> flag(id) |
| `HWDER_AUTOPLAY_CIRCLE` | `runtime/src/hid/hid.cpp:356` | presence -> flag(id) |
| `HWDER_AUTOPLAY_CAM` | `runtime/src/hid/hid.cpp:364` | presence -> flag(id) |
| `HWDER_GAME_INTERP` | `runtime/src/interp/interp_draw.cpp:56` | presence -> flag(id) |
| `HWDER_GI_LOG` | `runtime/src/interp/interp_draw.cpp:100` | presence -> flag(id) |
| `HWDER_GI_LOG_MS` | `runtime/src/interp/interp_draw.cpp:103` | u64 (set ? strtoull : default) -> value(id, default, base) |
| `HWDER_WAIT_TRACE` | `runtime/src/kernel/kernel.cpp:429` | presence -> flag(id) |
| `HWDER_IPC_DUMP` | `runtime/src/service/ipc.cpp:225` | site keeps its own parse of the string -> str(id) |
| `HWDER_IPC_TRACE` | `runtime/src/service/ipc.cpp:243` | presence -> flag(id) |
| `HWDER_IPC_TRACE` | `runtime/src/service/ipc.cpp:307` | presence -> flag(id) |
| `HWDER_IPC_BT` | `runtime/src/service/ipc.cpp:314` | site keeps its own parse of the string -> str(id) |
| `HWDER_SHADER_NO_KIL` | `runtime/src/shader/translate.cpp:293` | presence -> flag(id) |
| `HWDER_SHADER_FALLBACK` | `runtime/src/shader/translate.cpp:371` | site keeps its own parse of the string -> str(id) |

Sites that read one variable in several ways keep their own parsing (`debug::str`): `HWDER_VK_FRAME_DUMP` (vk_renderer.h: "auto", "t<s>", "p<hex>" and a present index from one string), `HWDER_VK_FRAME_DUMP_MIN_DRAWS` ("min-max"), `HWDER_VK_FRAME_DUMP_COUNT` (max(1, strtoull)), `HWDER_FILMSTRIP_W` (max(16, atoi)), `HWDER_GPU_TRACE` (first character), `HWDER_GAME_INTERP` (tri-state), `HWDER_AUTOPLAY` (presence plus atoi == 2), `HWDER_CACHE_SAVE_SYNC` (exactly "1"), `HWDER_THREAD_DUMP` / `HWDER_PATCH_DT` / `HWDER_FPS` / `HWDER_PRELAUNCH_FRAMES` (string kept, parsed by the site).

## Not converted (read another way)

- `HWDER_NSP`, `HWDER_KEYS`, `HWDER_EXEFS`, `HWDER_ROMFS`, `HWDER_PRELAUNCH`, `HWDER_NO_DUMP`: paths and flags read as wide strings through `env_u8` / `_wgetenv` in `core.cpp`, `fs/nsp.cpp`, `fs/romfs.cpp` and `config.cpp` (Unicode paths). Only the `getenv` presence checks of `HWDER_NSP`/`HWDER_KEYS` on the startup screen are in the table.
- `gpu/overlay.cpp`: the Debug switches tab compares each listed switch with `getenv(sw[0])` to show the restart marker (dynamic names).
- `gpu/settings.cpp` `apply_env`: sets `HWDER_SYNC_PIPELINES` and the saved debug switches with `_putenv_s` before anything reads them; the table does not change that order.

## Next

- Generate the README's debug-switch section and the F1 Debug switches list from `debug::table()` instead of the hand-written lists (follow-up; not part of the conversion).
- `HWDER_VK_FRAME_DUMP` could get one parsed accessor returning a small struct once its five rules are worth consolidating.
