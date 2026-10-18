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
