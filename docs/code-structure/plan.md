# Making the HWDER code easier to change

Audit of local `HWDER` main `7a4fee3`, read from the sources on Max's PC on 2026-10-21 (no build, no launch, no git write). Goal, in Max's words: make the C/C++ code easier to work with so substantial improvements to the game can be made later. Line numbers are from `7a4fee3`.

## What the code base is

| Part | Where | Size | Shape today |
|---|---|---|---|
| Recompiled game (generated C) | `generated/rc_<module>_<bucket>.c`, 340 files, one per 64 KiB of guest code | ~330 MB of C | Every function is `void f_<guest address>(Ctx* c)`. Names exist only in comments and in `docs/camera.md` / `docs/frame-loop.md`. |
| Recompiler | `tools/recomp/{gen.py, gen_main.py, a64*.py, nso.py}` | 733 + 168 + ~1900 lines | Clean pipeline. Hooks come from `hooks.txt` (address per line, name only in the `#` comment). |
| Game-facing layer (hooks) | `runtime/src/game/widescreen.cpp`, `runtime/src/interp/{camera,interp_draw,passthrough_hooks}.cpp` | ~500 lines | Each hook is an `extern "C" void f_<addr>(Ctx*)` that calls `f_<addr>_orig`. Guest structures are reached by `memcpy(cam + 0x14c, ...)` with the offsets repeated in each file. Cross-file state is passed through ad hoc `extern "C"` globals (`hw_gi_submit_tag`, `hw_last_frame_draws`, `hw_widescreen_tick`, `hw_game_interp_release_hz`) redeclared in every consumer. |
| Runtime: kernel, IPC services, fs, hid, audio, video | `runtime/src/{kernel,service,fs,hid,audio,video}` | ~7,500 lines | Reasonably modular: one class per service, a registry (`service/common.h`), one file per concern. Fine as is. |
| GPU core and Vulkan renderer | `runtime/src/gpu` (3,900 lines) and `runtime/src/gpu/vk` (6,900 lines) | `vk_renderer.cpp` alone is 4,602 lines / 230 KB | One class `VkRenderer` holds draw recording, pipeline building, the warmup cache and catalog, the 120 fps object blending, frame dumps, the filmstrip, post-processing and present. `do_draw` is 780 lines, `bind_resources` 317, `present` 235. |
| Shader translator | `runtime/src/shader` | ~3,300 lines | Self-contained, has an offline harness (`tools/shadertest`) that needs dumped programs. |
| Tests | `tools/tests`, `runtime/tests` | 5 suites | Build lock, first-run setup, 16 A64 instructions, warmup cache codec, catalog codec, save-folder picker. Nothing covers the kernel, IPC parsing, GPU method decoding, texture layout, pipeline keys or the game hooks. |
| Debug switches | everywhere | 115 `getenv("HWDER_...")` sites, 51 in `vk_renderer.cpp` | Each is a `static const bool` at its point of use. No single list; the README names about 25 of them. |

## Ranked findings: what makes changes hard, and the fix

Ranked by payoff for "substantial improvements to the game" divided by effort. Effort: S = hours, M = a day, L = several days. Every fix below is behaviour-preserving unless marked.

### 1. There is no game layer: hooks, names and structure offsets are scattered (payoff high, effort M)
- Adding a game hook today means: find the address by reading generated C, add it to `tools/recomp/hooks.txt`, regenerate, and write `extern "C" void f_<addr>(Ctx* c) { ...; f_<addr>_orig(c); }` somewhere. Nothing ties the name "Camera::BuildProj" to `0x100057cee0` except a comment (`widescreen.cpp:10`, `hooks.txt:3`).
- Guest structures are raw offsets: the camera's aspect is `cam + 0x14c` in `widescreen.cpp:66`, `widescreen.cpp:84`, `camera.cpp:396` and `docs/camera.md`. A mistake in one place is silent.
- Guest globals are magic numbers in code: `0x1000C21DE0` (`interp_draw.cpp:208`), `0x1000c21880` and `0x1000c2b5e8` (`camera.md`).
- Fix: a game layer in `runtime/src/game/`:
  - `game/symbols.txt` (checked in): `address  Name  [hook]  # note`, one line per known guest function and global. `gen_main.py` reads it and emits `generated/symbols.h` with `#define Camera_BuildProj f_100057cee0` style aliases plus a `// Camera::BuildProj` comment above each named function in the generated C. `hooks.txt` is derived from the `hook` column, so there is one source.
  - `game/structs.h`: `struct Camera { ...; float fov; float aspect; ...; }` with `static_assert(offsetof(Camera, aspect) == 0x14c)`, built from `docs/camera.md`.
  - `game/hook.h`: `HW_HOOK(Camera_BuildProj)` declares `f_<addr>` and `f_<addr>_orig` from the symbol table, and `game/guest.h` typed accessors replacing `GUESTP(u8, ...)` + `memcpy`.
  - `interp/interp.h` declaring the four cross-file globals once.
- Why first: this is the thing every future game-side feature (culling fixes, 120 fps work, gameplay patches) touches, and it is cheap. Verified by: generated C differs only in comment lines (diff), the runtime builds, the synthetic tests pass, an autoplay battle logs the same `widescreen:` and `gi:` lines as before.

### 2. `vk_renderer.cpp` is one 4,600-line class (payoff high, effort M, risk low if done as a pure move)
- Any renderer change rebuilds a 230 KB translation unit, and reading it means scrolling through six unrelated concerns. The draw-distance thread and this thread are both editing it, which is the collision the coordinator warned about.
- Fix: split by concern into files that share one private header (`vk/vk_renderer.h` with the class, `vk/vk_internal.h` with the helper tables at lines 360–690):
  | New file | Moves from `vk_renderer.cpp` lines | Content |
  |---|---|---|
  | `vk_draw.cpp` | 2936–3717 | `do_draw` (keep as is first, split into `build_pipeline_key`, `stage_vertex_input`, `stage_indices`, `record_draw` as a second commit) |
  | `vk_resources.cpp` | 1128–1316, 2178–2263, 2619–2936 | framebuffers, descriptor sets, TIC reads, `bind_resources` |
  | `vk_pipelines.cpp` | 1316–2178 | shaders, programs, pipeline keys, workers, warmup and catalog records |
  | `vk_interp_objects.cpp` | 2264–2619 | the 120 fps c3 object blending (stage c) and its trace |
  | `vk_present.cpp` | 3717–3852, 4295–4530 | `post_picture`, `present`, triple-buffer bookkeeping |
  | `vk_debug.cpp` | 4117–4295 and the frame-dump members | `HWDER_DUMP_FRAMES`, filmstrip, draw log |
  | `vk_renderer.cpp` | the rest | constructor, Renderer interface, clear, dispatch, blit, prewarm entry points |
- Verified by: identical symbol list of the linked exe (`dumpbin /symbols` diff on the objects, modulo file names), synthetic tests, an autoplay battle with the same draws-per-frame line and the same filmstrip hashes as a baseline run of `7a4fee3`.
- Coordination: tell the Draw-distance thread before moving lines it may be editing; rebase its patch across the move if it has one.

### 3. 115 scattered `getenv` debug switches (payoff medium, effort S–M)
- Each switch is discovered only by grepping. Several are read with different spellings of the same intent (`HWDER_VK_FRAME_DUMP` is parsed five ways at `vk_renderer.cpp:1108–1120`). The F1 menu's "startup switches" field (`settings.h` `Debug::env`) cannot list them because no list exists.
- Fix: `runtime/src/debug_flags.h` with one table `{"HWDER_SYNC_PIPELINES", "build pipelines on the GPU thread (no skipped draws, stutter instead)"}, ...` and `debug::flag(id)` / `debug::value(id)` accessors. Each `getenv` site becomes one call. The README section and the F1 menu are generated from the table. Behaviour-preserving: the same variables are read at the same times.

### 4. Tests do not cover what refactors break (payoff high for every later step, effort M)
- The synthetic suite is good at what it covers but cannot tell whether a renderer or kernel refactor changed behaviour. The only such check is a playtest or an autoplay run.
- Fix, in two layers:
  - Pure-function unit tests, no Vulkan device: `TextureCache::compute_layout` (block-linear swizzle sizes), `tic_format`/`rt_format`, `vertex_format`, the pipeline-key helpers (`compare_op`, `blend_factor`, `topology`), IPC `parse` + `write_response` round trip, `kern` handle table. One `runtime/tests/runtime_unit_test.cpp` target added to `synthetic_tests`.
  - An autoplay baseline script: run the fixed save for 120 s with `HWDER_FILMSTRIP` and the draw log on, keep the per-frame draw counts and thumbnail hashes as `runs/baseline-<commit>.txt`, and a compare script that reports first divergence. This is the before/after check for steps 2, 5 and 6, and what any renderer PR should attach. Uses the fixed autoplay save only, per the project rules.

### 5. Generated code and crash logs show only `f_<addr>` (payoff medium, effort S, falls out of step 1)
- Backtraces (`core.cpp:505`, `core.cpp:526`) and `hw_log` lines show `f_10002acb54`; the reader needs `frame-loop.md` to know that is the battle tick.
- Fix: `generated/symbols.h` also emits a name table `{addr, "Camera::BuildProj"}`; `hw_log_backtrace` and the crash handler print the name after the address. Pure addition.

### 6. Incremental build cost (measured 2026-10-22: not a problem)
- Measured in `HWDER-structure` at `7a4fee3`: full release build 367 s (610 steps); one comment edit inside `do_draw` to a new exe 9 s; a no-op build 2 s. The link is about 2 s, the rest is compiling the 230 KB `vk_renderer.cpp`.
- One cheap win: `vk_renderer.cpp` includes `hwder_version.h`, which `tools/version.cmake` restamps on every build and rewrites whenever the tree's dirty/clean state changes, so an edit-then-revert in any file recompiles the renderer. Take the version include out of the renderer units during the split (only `version.cpp` and the one place that hashes the commit need it).
- The static-library and incremental-link ideas are dropped; nothing to gain.

### 7. Smaller items (effort S each, do when nearby)
- `settings::video()` is read from the guest thread, the GPU thread and the UI thread; only `fps120` is atomic. Document the rule (UI writes, others read) in `settings.h`.
- `gpu/graphics_audit.h` counters, `g_prof`, `SlowTimer` and `ProfScope` are three profiling mechanisms; fold the counters into one `gpu/diag.h`.
- `vk_renderer.cpp:4538–4600` (catalog/prewarm entry points) is formatted on single lines without spaces; reformat when it moves in step 2.
- Comment banners that say "stage a/b/c" refer to a plan that is finished; rename to what the code does (`docs/high-framerate.md` already does).

## Not recommended now
- Rewriting the generated-code ABI (register struct, calling convention) or the recompiler's output format: high risk, no payoff for game improvements.
- Reorganising the kernel and services: they are already one class per concern and have not been where changes stalled.
- Splitting `overlay.cpp` (1,029 lines) or `texture_cache.cpp` (1,175 lines): large but single-purpose.

## First milestone (proposal)

Branch `code-structure` in worktree `HWDER-structure`, off local main `7a4fee3`. Small commits, each with a clean release build and the synthetic tests; renderer commits also get the autoplay baseline compare. Order:

1. **Baseline.** Build `7a4fee3` once, record the build time for a one-line `do_draw` change, run the 120 s autoplay baseline (step 4b) and keep its hashes. (S)
2. **Game layer** (finding 1 + 5): `symbols.txt`, `generated/symbols.h`, `game/structs.h`, `game/hook.h`, `interp/interp.h`; convert `widescreen.cpp`, `camera.cpp`, `interp_draw.cpp`, `passthrough_hooks.cpp` to use them; named backtraces. (M)
3. **Renderer split** (finding 2), after telling the Draw-distance thread: six pure-move commits, one file each, each verified against the baseline. (M)
4. **Debug flag table** (finding 3) in the renderer files as they move, then the rest of the tree. (S–M)
5. **Unit tests** (finding 4a) for the pure functions the split exposed. (M)

Nothing in this milestone changes what is on screen, so there is nothing for Max to playtest until it is done; one native-60 and one 120 fps playtest of the branch at the end confirms the baseline compare was enough. After it, the items left are the small items (7); build time turned out fine (6).

## Results

(Filled in as commits land. All on branch code-structure in HWDER-structure; nothing merged, nothing pushed. Autoplay runs under HWDER-structure/runs/.)

| Step | Status | Commit(s) | Check |
|---|---|---|---|
| 1 baseline | done | be6aa7c (run-baseline.ps1, compare_runs.py) on main 7a4fee3 | full release build 367 s (610 steps, exe 88.9 MB); synthetic tests 43 Python + 4 native, all pass; runs/baseline-7a4fee3 (150 s autoplay) |
| 2 game layer | done | 77c4c7b | symbols.txt (30 names), generated symbols.h + hw_symbols[], HW_HOOK/HW_ORIG/HW_GUEST, game/structs.h, interp/interp.h; one generator fix (gen_main.py emitted `0x...ULLULL` in symbols.h); build 5 min 47 s; 43 + 4 tests; runs/gamelayer 90 s: first 300 thumbnails identical to the baseline, gi:/widescreen: lines match |
| 3 renderer split | done | 5d0c718 sched, 7558a6d resources, e54f2c8 pipelines, de3b202 interp objects, 00638e0 draw, 271e791 present, 9a1b790 debug | vk_renderer.cpp 4603 lines -> 9 files, 4701 lines (30 lines edited for linkage/inline/namespace, 128 lines of boilerplate, every other line verbatim once, checked by line multiset); builds 5-8 s each; 43 + 4 tests after each; runs/split 90 s matches baseline and gamelayer |
| 4 debug flags | done | a25a06e table + inventory, then per directory af7ab30 gpu/vk, dbb4593 gpu, cf739d2 kernel, 686cd36 service, 8c91a61 fs, 013d976 hid, f18f09c audio, 795ecef interp, d4cca5e core, bae46ec shader | 76 variables, 115 reads on 109 lines -> 0 `getenv("HWDER_` outside debug_flags.cpp (wide-string path reads and the overlay's dynamic restart check left as they were); same parse rules and static caching; builds 5-6 s each, 43 + 4 tests after each; runs/flags 90 s matches runs/split (first 300 thumbnails identical) |
| 5 unit tests | done | cc3f8c7 | runtime/tests/runtime_unit_test.cpp: 245 checks over the format and pipeline-key mappers, texture layout, HIPC framing and the handle table; ctest runtime_unit (5 native tests now); pure moves into texture_layout.cpp, hipc.cpp, kernel/handles.cpp so they link without Vulkan; one pinned quirk (X-descriptor counter bits from address bits) |

## Results, milestone 2 (branch code-structure-2 in HWDER-structure, off main b4ae99a, 2026-10-22)

Finding 2's second commit (the `do_draw` sub-split, outline in [draw-split.md](draw-split.md)), finding 7 and the one behaviour change (the HIPC X-descriptor counter). Nothing merged, nothing pushed. Autoplay runs under `HWDER-structure/runs/`.

| Step | Status | Commit(s) | Check |
|---|---|---|---|
| 1 baseline | done | (none; main b4ae99a built in the worktree) | release build 7 s (incremental on the warm build dir; only the renderer units differed from the previous branch); 47 Python + 5 native synthetic tests; runs/baseline-b4ae99a (90 s autoplay, 7440 filmstrip captures, no CRASH/fatal/TERMINATE) |
| 2 do_draw sub-split | done | 6662037 build_pipeline_key, 795fb33 read_indices, 3a3ba6a stage_vertex_input, 93a7ef5 stage_indices, 0037e7f record_draw | do_draw 780 -> 240 lines; the five members are 131 + 92 + 108 + 63 + 218 lines; `DrawCtx` in vk_renderer.h carries the state between them. Line multiset of vk_draw.cpp before/after: every moved line appears once, only the signatures, reference aliases, copy-outs and the two `return false` lines are new, and three declaration lines changed to aliases (`PipelineKey key;`, `VkBuffer vbufs[32];`, `VkDeviceSize voffs[32];`). One block reordered (the attribute-format loop now runs before read_indices; no side effects, see draw-split.md). Builds 5-6 s each, 47 + 5 tests after each. |
| 3 one diagnostics header | done | 0dba254 | gpu/diag.h + diag.cpp replace gpu/graphics_audit.h and the profile/SlowTimer block of vk_common.h; the storage and prof_flush moved from texture_cache.cpp / vk_debug.cpp; vk_common.h re-exports the names into gpu::vk, so the renderer's call sites did not change; gpu.cpp, emit_mem.cpp, vk_draw.cpp, vk_present.cpp swap one include. Build 6 s, 47 + 5 tests. |
| 4 small items | done | 6ec0328 settings threading comment, 12f537f stage a/b/c banners (13 comment lines in 9 files; log strings kept), 7c55a96 reformat of the catalog/prewarm blocks | 4c is whitespace only: vk_pipelines.cpp with all whitespace removed hashes identically before and after (sha256 59279e3e...); `git diff -w --stat` still shows the split lines (62+/21-) because it cannot ignore line breaks. Builds 5-6 s, 47 + 5 tests after each. |
| 5 IPC fix | done | da63d06 | hipc.cpp parse(): X-descriptor counter is `(d0 & 0x3F) \| (d0 & 0xE00)`; the pinned unit test now sets counter bits 9-11 and address bits 32-34 and asserts 0x602 (old formula: 0xA02); 245 checks pass. `x_index` is pushed but never read anywhere in the runtime, so the fix changes no observable behaviour today. |
| 6 final check | done | (docs commit) | release build at the tip; 47 Python + 5 native synthetic tests; runs/milestone2 (90 s autoplay) vs runs/baseline-b4ae99a: 300 of the first 300 thumbnails SHA-identical, the one `widescreen:` line identical, the same five `gi:` lines (1 enabled + 4 stage-c summaries), no CRASH/fatal/TERMINATE in either log; draw-count series first differs at position 458 (9 vs 10 draws, autoplay timing, as in milestone 1); 7440 vs 7548 captures. |
