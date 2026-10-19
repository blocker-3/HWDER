# The game's frame loop (stage a of the 120 fps plan)

Measured 2026-10-18 on `interp-120` with the frame probe (`runtime/src/interp/frame_probe.cpp`),
five autoplay battle runs (`HWDER_AUTOPLAY=2`, `HWDER_AUTOPLAY_SKIP=1`, fixed autoplay save),
steady battle 25–40 s after the first 3D frame. Addresses are guest addresses (`main` at
`0x1000000000`); names are ours. Background: `docs/camera.md`, `docs/high-framerate.md`.

## Threads

| Guest thread | Runs | Per tick |
|---|---|---|
| 80 `MainThread` (entry `0xfffe00000`) | Draw: scene A slot 5 `f_10001767e8` → camera constants `f_10003d0a74`; then GPU submit and present | 1.3–3 ms |
| 96 (job worker, entry `0x100248e8d8`) | scene A slot 2 `f_1000176490`, then slot 4 `f_10001763bc` | 1–3 ms |
| 97 (job worker, same entry) | scene A slot 3 `f_10001766a4` (publishes the render cameras, `f_10003d45cc`) | 0.3–0.6 ms |

Scene object A is the same instance on every thread (`S = 0x208e2a9570` in these runs). Scene
class B (vtable `0x1000bd36f8`, slots 3 and 5) and A's slot 18 were never called in battle.

## One tick, in order (steady battle, 60 Hz)

```
worker 96   slot2 (1.1-2 ms) -> slot4 (0.01 ms)
worker 97                                  slot3: publish render cameras (0.3 ms)
MainThread                                                  Draw = slot5 (1.3-2 ms)
                                                              cam-constants upload (inside)
                                                            SubmitGPFIFO x2 -> QueueBuffer (present) -> DequeueBuffer
... ~13 ms idle (limiter / vsync wait), next tick ...
```

Strictly sequential: the next tick's slot 2 starts only after MainThread has presented and dequeued
(no overlap seen in 400+ traced calls per run). Exactly **one Draw per camera publish**: 0 or 2
Draws between publishes occur only in heavy moments when a frame runs late (e.g. 19/283 in one 5 s
window), never in calm play.

Call chains (guest return addresses at entry, run 5):

* Draw: `f_10001767e8` ← `f_1000387f9c` ← `f_10002acb54` ← `f_10002ad35c` ← `f_10003b01fc` ←
  `f_10003b00fc` ← `f_10003af844` ← `f_10002ac728` ← `f_10002ad200` ← `f_100039cfc8` ← sdk main.
  `f_10002ad35c` is MainThread's per-frame function; it calls the Draw stage (`f_10002acb54`) and,
  after it returns, the submit and present (both reached by indirect calls, not yet pinned down).
* Update: slot 2/4 from job `f_10002ace60`, slot 3 from job `f_10002acfbc`; both run by the job
  system (`f_100045dcdc` ← `f_10005c4f90` ← worker entry `0x100248e8d8`).

The camera slot words at `S + 0x710dc` / `S + 0x710e0` (docs/camera.md) read `3`/`3` during
update and `3`/`0` during Draw: not a rotating triple-buffer index as assumed; `+0x710e0` behaves
like a "drawing" state. To be re-read in stage b before relying on it.

## What Draw writes

`HWDER_DRAW_WRITES=5`: on 5 ticks, every read-write page of heap, alias and the module images was
made read-only for the duration of Draw; the first write to each page is attributed to the
recompiled function that made it (draw_writes.cpp). Pages first written by another thread during the
window are counted separately (so this is a lower bound for Draw).

* 2400–3100 pages per Draw, by 74–93 functions; 64 functions write on every sample.
* The bulk are render-side buffers: `f_10005b08a8`, `f_100058e708`, `f_1000590ec8`,
  `f_100058e78c`, `f_10005a7f3c`, `f_10005af768`, `f_100052c85c` (the `0x10005xxxxx` graphics
  library around `Camera::BuildView`), and the NVN driver in `sdk` (`f_10026xxxxx` writing
  page-aligned command memory, `f_10027971c4` a memcpy/memset-style helper).
* **Module globals written by Draw (the candidates for state to guard):** `f_100021b030`
  (`0x1000c75d30`), `f_1000220e60` (`0x1000ba9518`), `f_1000547f60` (`0x1000bec470`),
  `f_100058e46c` (`0x1000cf3568`), `f_100248fc90`, `f_1002645cf8` (`sdk`, `0x1002dec6f0`). Not yet
  identified: frame counters, allocator heads or effect state are all possible.
* Heap writers in `0x10002xxxxx`–`0x10004xxxxx` (game code: `f_100021aa68`, `f_1000221eec`,
  `f_100040d520`, `f_100044d520`, …) are the ones most likely to be game state (effects,
  animation); each needs a look in stage b.

## Draw twice per tick (`HWDER_DRAW_TWICE=1`)

Experiment: after slot 5 returns, run it again with the same entry registers (same tick, same camera).

* No crash and no stall over ~40 s of battle; frame time unchanged (16.96 ms average), still one
  camera publish per Draw pair and one present per tick.
* Both Draws' commands land in **the same frame**: draws per frame roughly doubled (e.g. clears 12 →
  22, 300–400 → 450–550 draws), submitted together by the single SubmitGPFIFO/QueueBuffer after
  slot 5. So an in-between frame needs the submit + present step repeated too, not just slot 5.
* Not checked (needs eyes): whether effects or animations advanced twice as fast.

## Consequences for stage b

* Hook point: MainThread's frame function `f_10002ad35c` around its Draw stage + submit + present
  (or slot 5 plus a call into the same submit/present path), with the render camera replaced by the
  blended one in between; the update threads are idle then, so nothing races the extra pass.
* Budget: Draw is 1.3–3 ms of CPU per pass, well inside a 8.3 ms 120 Hz slot.
* Guards: the six module-global writers above first; then any game-code heap writer that shows a
  visible double-speed effect when Draw runs twice.

## Probe switches

| Variable | Effect |
|---|---|
| `HWDER_FRAME_PROBE=<n>` | after the battle has been up `HWDER_FRAME_PROBE_DELAY` s (default 40): log n hook entries/exits with thread, time and camera slot words, then 5 s stats (calls/s, mean time, Draws per publish); also the guest caller chain once per hook |
| `HWDER_DRAW_WRITES=<n>` | write census of Draw on n ticks, 2 s apart (diagnostic; slows those ticks to ~100 ms) |
| `HWDER_DRAW_TWICE=1` | run Draw twice per tick once the probe is armed (experiment) |

Markers `* gpfifo`, `* queue`, `* dequeue` in the trace come from `nvdrv.cpp` (SubmitGPFIFO) and
`vi.cpp` (QueueBuffer, DequeueBuffer).

## Correction and the present path (stage b probing)

The Draw caller chain above stops one level short. MainThread's frame loop `f_10002ad35c` calls
`f_10002acb54` once per tick in battle, and that function **is the whole tick**:

```
f_10002acb54(scene x19, frame state x20, graphics device x21)
  wait for / run the update jobs (scene slots 2, 4, 3 on the job workers)
  f_100051e9c4(dev, w, h, ..)   BeginFrame                         call at 0x10002acd08
  f_10001e2f7c                  profiler marker (when [x20+662] == 0)
  f_1000387f9c(scene)           Draw: scene slot 5, camera constants
  f_10001e301c, lock/copy/unlock, f_10001df06c x2, f_10001df624   end-of-frame bookkeeping
  vcall scene+192 -> f_100052f6bc(dev, 0, 0, window target, 0)    when the scene asks
  f_100051ea20(dev, 1)          EndFrame
  f_100051e67c(dev, 0)          Present: SubmitGPFIFO x3, QueueBuffer, DequeueBuffer (unless [[x20+544]+0x2d420])
  f_10003cf0a8, f_1000389044(scene), f_10004598dc, vcall scene+136, tail call scene+152
```

Menus and other non-battle screens draw through `f_10002ac87c` instead (everything on MainThread;
the present is again `f_100051e67c`, from `0x10002acb2c`).

The render camera buffer index `S + 0x710dc` does rotate at times in battle: consecutive ticks can
publish into different slots (up to half of the ticks in some stretches).

## Stage b: the game draws the in-between frame (`HWDER_GAME_INTERP=1`)

`runtime/src/interp/interp_draw.cpp` hooks BeginFrame. On the battle call (return address
`0x10002acd0c`), before the real frame of tick n, it:

1. takes ticks n-1 and n from the camera ring (camera.cpp records each published camera and now its
   guest address), skips on a cut (eye or target > 500 units), a projection change, non-consecutive
   ticks or > 250 ms between them, and checks tick n's render camera is still unchanged;
2. writes eye/at/up/roll halfway between the two into tick n's render camera and rebuilds its
   matrices with the game's own `Camera::Update` (`f_100057cb78(cam, 1, 1)`);
3. runs BeginFrame, Draw, the scene+192 / `f_100052f6bc` step, EndFrame and Present on a copy of the
   registers (profiler markers and the end-of-frame bookkeeping are not repeated);
4. restores the camera bytes and lets the real frame run unchanged.

The integrated experimental mode needs only `HWDER_GAME_INTERP=1`; it supplies its own pacing.
QueueBuffer captures a release rate on the calling guest thread: 120 Hz for an extra and its paired
real frame, 60 Hz for an unpaired tick or a menu/loading frame. This immutable metadata travels with
the queued buffer to the compositor, so a delayed present keeps its original classification.
The two battle presents share each native 60 Hz tick. `HWDER_FPS` and `HWDER_FORCE_INTERVAL1` do not
override this mode; 120 Hz uses one release slot and native 60 Hz preserves guest swap intervals.
With game interpolation off, the presenter's normal defaults and experimental overrides are unchanged.
The old `HWDER_INTERP` GPU-replay output is disabled while game-drawn interpolation is active.

Measured (autoplay battle, 5 s windows, `HWDER_GI_LOG=1`): 294–295 extra frames per 295 ticks, game
ticks at 59–60/s, presents every 8.5 ms on average (about 118 fps), no frame over 20 ms, extra frame
5.5–7 ms including the present wait. Not yet judged by eye: whether the in-between frames look
right (camera only; characters still move at 60), and whether effects, animations or sounds run
faster.

### After Max's first playtest (2026-10-18)

* Max: camera very smooth; texture flashes and artifacts in battle gone; Link blurry when running fast
  (expected until stage c: he moves in 60 Hz steps while the camera moves at 120); an occasional
  one-frame black flash in menus; dips to about 100 fps with the GPU at 30 %.
* Menus ran at double speed: outside battle there is no extra frame, but the display still released
  frames at 120 and the game is frame-locked. The original fix used a 100 ms freshness grace after
  the last extra draw. Integration replaces that heuristic with queued-frame classification:
  every non-extra BeginFrame clears the paired state, and only actually queueing an extra marks the
  following real frame as paired. Menus and rejected/skipped-extra ticks immediately use native pacing.
* The dips were ticks that did not fit 16.7 ms (update, extra frame, real frame and two present waits
  run in series on MainThread); they slowed the game itself to 53-55 ticks/s. Now a tick that ran
  late (> 17.5 ms) skips the next extra frame, so the game keeps 59-60 ticks/s and the display shows
  a single 60 Hz frame instead (autoplay: 107-118 fps, 0-64 skipped per 295 ticks).

## Steady 120 (2026-10-19)

Per-tick timing (`HWDER_GI_LOG=1`, "gi: per tick"): update 3 ms mean but up to 11-15 ms, extra draw
~2 ms, real draw ~2 ms; the rest was waiting. The game could only run one frame ahead: the
compositor freed a swap buffer only when the next frame arrived, so "update + extra draw" had to fit
one 8.3 ms slot, and every update spike missed it.

* **Triple buffering under stage b** (`vi.cpp`): a buffer goes back to the game as soon as its screen
  blit has run, so the game can queue two frames ahead; the 120 Hz release grid still paces it. The
  extra frame's present wait dropped from 4.4 ms to ~0.2 ms.
  Integration keeps rendering-fence and display-read ownership across timeouts: it retries waits and
  never returns a still-pending source buffer to the guest. A Vulkan presentation-wait error uses the
  existing fatal path; process shutdown can end a genuine GPU stall without reusing the buffer.
* **Skip rule**: a running "behind the 60 Hz grid" budget (decaying, capped) instead of single late
  ticks, never two skips in a row.
* Result (autoplay battle): 295 of 295 ticks with an extra frame, frames every 8.47 ms on average
  (the 120 grid), none over 20 ms; a heavy stretch still skips ~16 of 295.
* The autoplay script no longer leaves "battle" on a single light frame (it opened the pause menu).

## Stage d: experimental side-effect diagnostics

* The sampled battle's service calls during extra frames (`hw_gi_note_ipc`) were only
  `IHOSBinderDriver#3` (Queue/Dequeue buffer) and `nvdrv#1` (GPU submits), two per frame. No audio,
  input or file IPC was observed in that sample. The scope is thread-local and measures synchronous
  calls from the extra-draw thread; it does not cover work scheduled on other threads.
* This is tracing, not a general side-effect fence. It does not suppress calls, identify all guest
  memory changes, or prove that sounds, particles, animation, spawns or scene changes cannot advance
  during Draw. The unidentified game-code writers in the census above remain relevant.
* The draw-twice experiment and Max's earlier playtest showed no visibly doubled effects in the
  tested scenes. Effects, dense spawns, audio and transitions still need playtesting on the integrated
  candidate; historical timing measurements above are not validation of the newly combined build.
* Menus, loading and pause have no extra frame and use queued native pacing (see above).

## Stage c research notes

* Per-object constant blocks live in GPU-visible ring pages (e.g. `0x2005124000..`), 0x100 apart,
  copied by the sdk memcpy `f_10027971c4` during Draw. Each holds a 3x4 world matrix (`mL2W`, rows
  with the translation in the 4th column) followed by the camera's viewProj (`mW2P`); the shader
  parameter table at `0x1000bf1718` names the group: `mL2W, mW2P, mW2S, mP2S, mL2WPrev`, then the skin
  group `mL2P, mL2WT, bSkin, nBLoop`.
* The ring keeps older frames' blocks, so matching game-side sources by value is ambiguous. Plan:
  blend in the renderer: tag the extra frame's GPU submissions, keep the previous real frame's
  constant data per draw (key: program + vertex/index buffers + counts), and blend the 3x4 world
  matrices in the extra frame's copy. `HWDER_CBUF_DUMP=<present number>` dumps every draw's constant
  buffers (camera positions, 3x4 transforms) to learn the bone-matrix layout first.

## Stage c: characters and objects blended (2026-10-19)

From a per-draw constant buffer dump (`HWDER_CBUF_DUMP`): every draw's vertex-stage c3 holds its
transforms. Static/simple objects: 128 bytes, the camera's viewProj at +16 and the object's 3x4 world
matrix at +80. Skinned characters: 1792-2048 bytes, a camera-derived header then 29-35 bone matrices
(3x4, 48 bytes each, world-space translations).

Implementation (`vk_renderer.cpp`, `obj_transforms`), on with `HWDER_GAME_INTERP` (off with
`HWDER_GI_NO_OBJECTS=1`):

* GPU submissions carry a tag, `(tick << 1) | extra`, set by `interp_draw.cpp` (`hw_gi_submit_tag`)
  and copied into each nvdrv submission; the GPU thread exposes it (`gpu::exec_gi_tag()`).
* Real frames: each draw's c3 block that holds a world transform is kept (one arena per frame, no
  allocation per draw), keyed by program + vertex/index buffers + counts.
* In-between frame of tick n: each c3 block is paired with real frame n-1's block of the same key
  (same draw order first, else the nearest unused one by first transform); every 3x4 world transform
  is blended halfway (same slot first - bone palettes keep their order - else the nearest transform),
  only in the GPU's copy. Transforms that moved more than 80 units are left at tick n (teleports,
  respawns, other objects).
* The block is built in cached memory and written to the staging buffer once: reading the staging
  buffer back (write-combined memory) had made this cost 3-15 ms per tick; now 0.1 ms.
* Autoplay battle: ~180-250 c3 blocks per in-between frame, ~100 paired and blended (~2,200 matrices),
  ~80 transforms per frame left unblended for moving too far.

## Ultrawide side bands flashing (Max, 2026-10-19)

Since triple buffering, the side bands of "Fill ultrawide" were blacked out on frames whose full-width
pass had run before the previous present (the check compared frame numbers). Now each image remembers
whether a full-width pass wrote it since it was last presented; the log line "ultrawide side bands
blacked out on N of 600 presents" stays at 0 in battle.
