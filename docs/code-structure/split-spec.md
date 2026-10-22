# Splitting vk_renderer.cpp (step 3 of the code-structure milestone)

Pure move, no behaviour change. Line numbers refer to `vk_renderer.cpp` at main `7a4fee3` (4,602 lines); after the game-layer commits they shift by about +1 (one added include). Verify each commit with: release build, synthetic tests, `tools\compare_runs.py` against `runs\baseline-7a4fee3` (draw counts identical for every frame; thumbnails may legitimately differ only where the autoplay input timing differs, so the check is "draw-count series identical" plus "no new log lines of kind CRASH/fatal/slow").

## Target layout (`runtime/src/gpu/vk/`)

| File | Takes from `vk_renderer.cpp` | Notes |
|---|---|---|
| `vk_renderer.h` (new, private to vk/) | the `VkRenderer` class declaration, lines 693–1125, plus the structs it needs: `CbufBinding`, `StageLayout`, `ShaderEntry`, `PictureShader`, `GfxProgram`, `WarmProgram`, `WarmPipeline`, `Framebuffer`, `PipelineKey` (lines 360–690 region) | Everything that was in the anonymous namespace of `namespace gpu` and is referenced from more than one new file moves here into `namespace gpu::vk::detail` (or stays `namespace gpu` with `static inline` for small helpers). Enum/format mappers (`compare_op`, `stencil_op`, `blend_op`, `blend_factor`, `topology`, `logic_op`, `view_type`, `tic_swizzle`, `cached_vertex_format`) become `inline` functions in the header. |
| `vk_sched.cpp` (new) | lines 101–357: ring buffer constants, `Frame`, `Sched`, `g_s`, `g_rec`, `Recorder` members, waiter thread, `retire_resource`, graveyard | Already `namespace gpu::vk`; its globals stay file-local except `g_s` and `g_rec`, which other files use: declare them `extern` in `vk_renderer.h` (they are currently in an anonymous namespace, so give them external linkage in exactly one TU). Keep `rec()` as the accessor. |
| `vk_resources.cpp` (new) | `create_dummies`, `dummy_view` (1128–1159), `resolve_framebuffer`, `begin_rendering` (1160–1281), `alloc_set`, `read_tic` (2178–2263), `bind_resources` (2619–2935) | |
| `vk_pipelines.cpp` (new) | `stage_layout`, `get_shader`, `get_program`, `get_program_slow` (1282–1542), `ensure_warmup_records` … `create_pipeline` (1543–2177), `recipe_bytes`, the catalog/warmup file constants and helpers (642–673), and the entry points `start_private_pipeline_prewarm`, `start_pipeline_prewarm`, `prepare_pipeline_catalog` (4538–4601) | Reformat the single-line blocks at 4538–4601 while moving (whitespace only). |
| `vk_interp_objects.cpp` (new) | lines 2219–2618: `kObj*` constants, `is_world_3x4`, `obj_*` members | The Draw-distance thread's patch lands here later. |
| `vk_draw.cpp` (new) | `do_draw` (2936–3716), `do_clear` (3852–3921), `do_dispatch` (3922–4022), `do_blit` (4023–4113) | Second commit later: split `do_draw` at its `// ----` markers into `build_pipeline_key`, `stage_vertex_input`, `stage_index_buffer`, `bind_sets`, `record_draw`. |
| `vk_present.cpp` (new) | `post_picture` (3717–3851), `present` (4295–4529), `kPictureShaders` (556–563) | |
| `vk_debug.cpp` (new) | `write_image_bmp`, `dump_frame`, `filmstrip_capture`, `g_strip_tag` (4114–4294), the drawlog helpers (625–641), `prof_flush` (674–690) | |
| `vk_renderer.cpp` (kept) | the Renderer-interface wrappers (constructor and lines 727–823 if they are not already inline in the class), `create_renderer`, `find_memory_type`, `create_buffer`, `destroy_buffer` (38–99) | Ends up a few hundred lines. |

Add every new file to `runtime/src/gpu/vk/sources.cmake`.

## Rules for the move
- Move text, do not rewrite it. If a function needs a helper that is now in another file, move the helper to `vk_renderer.h` as `inline` rather than duplicating it.
- Globals that were file-static and are used by one new file only stay static in that file. Globals used by several files get one definition in the file that owns them and an `extern` declaration in `vk_renderer.h`. List every such global in the commit message.
- Keep `static const bool x = getenv(...)` initialisers exactly where they are; they move with their function.
- Do one file per commit, building after each: `Renderer: move <what> to vk_<file>.cpp`. The first commit creates `vk_renderer.h` and `vk_sched.cpp`.
- After the last move, `wc -l` every file and put the table in the commit message.

## Verification per commit
1. `python tools\build.py` succeeds.
2. `python tools\tests\run_synthetic.py --jobs 2` passes.
3. After the final commit (not every one): `run-baseline.ps1` with the output folder `runs\split-<short sha>` and `python tools\compare_runs.py runs\baseline-7a4fee3 runs\split-<sha>`: the per-frame draw-count series must match the baseline for the whole run, and the log must have no CRASH/fatal/TERMINATE lines.
