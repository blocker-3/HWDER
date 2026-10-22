# Splitting `do_draw` (code-structure milestone 2, step 2)

`VkRenderer::do_draw` in `runtime/src/gpu/vk/vk_draw.cpp` at main `b4ae99a` is 780 lines (15-794). Line numbers below are from that commit. Read before cutting, 2026-10-22.

## Outline of the phases

1. **Prologue** (16-35): draw counters, return on an empty draw, the one-time `before_first_live_draw` warmup log, `get_program` under `kProfProgram`; skip (counted, logged once) when there is no usable program.
2. **Framebuffer** (36-53, `kProfFramebuffer`): hash the render-target/zeta registers, reuse `fb_cache_` or `resolve_framebuffer`.
3. **Pipeline key** (55-151, `kProfKeyBuild`): formats and topology, cull/front face (with the `HWDER_VK_FACE_RULE` override and the once-per-func alpha-test log), polygon mode, depth bias class, depth/stencil, per-target blend and colour mask, logic op, primitive restart.
4. **Index source and vertex range** (153-231): inline or guest index buffer (early return, `skipped_++`, when it is not mapped), `vmin`/`vmax` from the cached scan (`idx_range_cache_`, `st_.idx_hit/miss`), quad and quad-strip conversion to a triangle list (early return when it is empty).
5. **Vertex attribute formats** (233-257): fills `key.attrib_*` from `VertexAttribFormat`, computes `streams_used` and `need_const`; pure.
6. **Vertex streams** (258-357, `kProfVertex`): per stream the guest byte range for [vmin, vmax] or the instance, the staging slice (`vb_cache_` hit/miss), the `fd:` vertex dumps, `key.binding_stride`, the constant-attribute binding.
7. **Index upload** (361-409, `kProfIndex`): quads to u32, 8-bit to u16 (restart remapped), 16/32-bit staged through `vb_cache_`; `vertex_offset` / `first_vertex`.
8. **Descriptor sets** (413-448, `kProfDescriptors`): `cur_draw_key_`, per stage `alloc_set` + `bind_resources` (may upload textures, collects `post` callbacks).
9. **Pipeline** (451-467, `kProfPipeline`): `get_pipeline`; a pending async pipeline skips the draw (counted, audited, logged once).
10. **Diagnostics** (468-589, `kProfRecord` starts here): `HWDER_VK_FRAME_DUMP` trigger, the drawlog line, the `fd:` draw/cbuf/attribute dump, the verbose draw line.
11. **Record** (591-793): `begin_rendering`, bind pipeline/sets/vertex buffers through `rs_`, viewport and scissor, dynamic state, push constants, `vkCmdDraw(Indexed)`, `mark_modified`, the `post` callbacks, `post_picture`.

## The cut

Four private members are extracted per the brief, plus one more because the index work sits in two places (the vertex range must be known before the vertex streams are staged, and the index upload comes after them; merging the two would reorder the staging allocations):

| Member | Phases | Early return |
|---|---|---|
| `build_pipeline_key` | 3 and 5 | none |
| `read_indices` | 4 | `false` when the index buffer is not mapped or the converted quad list is empty |
| `stage_vertex_input` | 6 | none |
| `stage_indices` | 7 | none |
| `record_draw` | 11 | none |

Phases 1, 2, 8, 9 and 10 and the `ProfScope` phase switches stay in `do_draw`.

State that flows between the phases lives in `DrawCtx` (`vk_renderer.h`): the key, `topo`/`quads`, the index source (`indexed`, `isize`, `idx_src`, `converted`), the vertex range and count, `attrs_used`/`streams_used`/`need_const`, the staged vertex and index buffers and `vertex_offset`/`first_vertex`. Each function (and the rest of `do_draw`) aliases the fields it uses with references at the top so the moved lines stay verbatim.

One block moves across another: phase 5 (the attribute-format loop) joins `build_pipeline_key`, so it now runs before `read_indices` instead of after it. It has no side effects (`cached_vertex_format` is a pure lookup cache, nothing is logged or counted) and both were under `kProfKeyBuild`; the only difference is that a draw whose index buffer is unmapped now computes attribute formats it does not use. Everything else keeps its order: log lines, counters, cache updates, staging allocations and early returns are where they were.

Verified per commit by a release build and `run_synthetic.py`; a line-multiset check (every non-blank line of the old `do_draw` appears once in the new file, modulo the alias and signature boilerplate) and, at the end of the milestone, the autoplay compare against `runs/baseline-b4ae99a`.
