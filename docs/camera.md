# The game's camera (main module)

> The renderer replay interpolation described below (`HWDER_INTERP`, `HWDER_INTERP_MODE=camera`, `HWDER_CAM_LOG`, `HWDER_CAM_INV`) was removed on 2026-10-21. The camera hook and its per-tick snapshots remain and feed the game-drawn 120 fps mode (`docs/high-framerate.md`).

Found statically from `data/exefs/main` (no symbols; names below are ours). Addresses are guest
addresses with `main` at `0x1000000000`.

## Camera object (0x170 bytes)

| Offset | Field | Evidence |
|---|---|---|
| +0x10 | eye (vec3, w pad) | `Camera::BuildView` 0x100057cc3c reads +0x10 and +0x20, normalises eye − at |
| +0x20 | target / at (vec3) | same |
| +0x30 | up (vec3) | cross product with the forward axis, same function |
| +0x40 | view basis, 4x4 (before roll) | written by BuildView |
| +0x80 | view matrix 4x4 (roll applied) | BuildView; inverted to `matV2W_*` at 0x10003d0c14 |
| +0xc0 | projection 4x4 | `Camera::BuildProj` 0x100057cee0 writes `x19 + 0xc0` |
| +0x100 | view × proj 4x4 | `Camera::Update` 0x100057cb78 |
| +0x140 / +0x144 | near / far | uploaded as `fNear` / `fFar` at 0x10003d1010 |
| +0x148 | vertical fov (radians) | uploaded as `fFov`; `tanf(fov * 0.5)` in BuildProj |
| +0x14c | aspect | BuildProj, `vViewInfo` |
| +0x150 | roll | BuildView applies it when non-zero |
| +0x154, +0x158 | extra projection parameter, flags (bit 0: size from eye–target distance, likely orthographic; bit 1: near/far swapped) | BuildProj |
| +0x15c | aperture | uploaded as `fAperture` |
| +0x160 / +0x161 | view / projection dirty flags | `Camera::Update` |

## Where cameras live and move

* **Game cameras:** camera manager `*(*(u64*)0x1000c21880)`, per screen at `+0x350 + screen * 0x5a0`.
* **Render cameras:** triple-buffered in the scene object S (`*(*(u64*)0x1000c2b5e8)`) at
  `S + 0x71360 + (slot % 3) * 0x2e0 + screen * 0x170`. The write slot index is `S + 0x710dc` (observed mostly 0, so not a tick counter).
  The render side reads its slot with `S + 0x710e0` (0x10003d6634, `GetRenderCamera(S, screen)`).
* **Per-tick sync:** `0x10003d45cc(S, slot)`, reached through `0x10003d61a4(S)` from 0x1000173270 and
  0x10002d96e0. For each active screen it copies the game camera into the render slot
  (`Camera::CopyFrom` 0x10003e6ee4, 0x150 bytes from +0x10) and calls `Camera::Update(cam, 1, 1)`.
  This is the hook point: one finished camera per game tick, after the game is done with it.

Other `Camera::Update` callers (0x10003d4ed4, 0x10003df448 and about a dozen more) are other
copies and cameras, not identified (inferred: shadow, reflection or menu views) and not hooked yet.

## Hook

`tools/recomp/hooks.txt` lists hooked guest functions. `gen_main.py` emits each one's body as
`f_<addr>_orig`; the runtime defines `f_<addr>` (here `runtime/src/interp/camera.cpp`), so direct
calls, tail calls and the dispatch table all reach the hook. Run with `HWDER_CAM_LOG=1` to log
screen 0's eye, target, up, fov, aspect, near, far and roll every tick, with a host timestamp.

## BuildView and viewProj, reproduced

`Camera::BuildView` (0x100057cc3c), read from its generated C (`generated/rc_main_057.c`), in
row-vector convention (axes in columns 0..2, translation in row 3):

* `d = eye − at`, `f = d / |d|`; `r = up × d` (d not normalised), normalised; `u = f × r`.
* `view = [r u f 0]` per row x/y/z, row 3 = `(−r·eye, −u·eye, −f·eye, 1)`; every dot product is
  `(x·x + y·y) + z·z`, separate multiplies and adds.
* Roll ≠ 0: columns 0/1 rotated by `sincos(−roll)` (the game's own sincosf at 0x100000fd70).

`Camera::Update` (0x100057cb78): `viewProj = view × proj`, each output row an FMA chain
`fma(P3, V[r][3], fma(P2, V[r][2], fma(P1, V[r][1], P0 · V[r][0])))`.

`runtime/src/interp/camera.cpp` (`build_view`, `mul_view_proj`, compiled with FP contraction off)
is bit-exact against the recompiled game functions on 1,000,000 random cameras with roll 0 (an
offline harness linking the generated BuildView/Update). With roll ≠ 0 it uses the host's sin/cos,
which may differ in the last bit. In the game, every recorded tick is checked again and the result
logged after 60 ticks and every 3600 (`cam: BuildView reproduction ...`).

## Camera interpolation (`HWDER_INTERP_MODE=camera`)

Frame interpolation (`HWDER_INTERP=<Hz>`, or `-1` for the monitor's rate; the overlay option is
disabled in this build) replays each game frame's recorded Vulkan command stream k times per frame
at the display rate. The default mode lerps every float of every constant buffer against the
previous frame. Camera mode keeps the replay and changes what is blended:

* **Record:** the hook copies screen 0's camera after the original runs into an 8-entry ring
  (mutex; game thread writes, presenter reads) with its tick count and a host QPC timestamp.
* **Identify:** at present, every constant buffer of the frame is searched for each snapshot's
  view and viewProj, exactly, in four layouts: 4x4 as stored, transposed, the first three rows
  as stored, the first three rows of the transpose (column-major 3x4). Near-trivial matrices
  (fewer than four values other than 0/±1) are not searched. The snapshot with the most hits is
  the frame's tick b; a is the snapshot with tick b − 1.
* **Interpolate:** one tick behind. Tick b is shown unblended at the frame's last output slot, so
  output j of k is `(k − j)` slots earlier: `alpha = 1 − (k − j) · slot / (t_b − t_a)` clamped to
  0..1, from host timestamps. eye/at/up/roll are lerped as `b − (b − a)(1 − alpha)` (exactly b at
  alpha 1), up renormalised, then view rebuilt with `build_view` and viewProj with b's projection.
* **Substitute:** in the replay copy of each constant buffer only the matched runs are replaced
  (same layout). Every other constant comes from the frame unchanged, texel buffers too.
* **Snap** (the real frame is shown for that slot): no match, no tick b − 1 in the ring, eye or at
  moving more than 500 units in one tick, any projection parameter or proj matrix changing, more
  than 250 ms between the ticks (pause, load), or alpha 1.

Not substituted (by design for now, may show as small mismatches): other camera-derived constants
(inverse view `matV2W_*`, eye position, frustum data), other cameras (shadows, reflections), and a
previous-tick viewProj the frame may hold for its own effects (counted in the log as "also hold
another tick").

`HWDER_CAM_LOG=<n>` (n > 0) logs, once per second:

* `cam: match: M/N frames matched (tick lag 0/1/2+ ...) | hits per frame: view 4x4 .. 4x4T .. 3x4
  .. 3x4T .., viewProj ... | scan ... cbufs ... KiB per frame, ... ms` — coverage per layout; tick
  lag 0 means the frame used the newest tick at present time.
* `cam: replays: B blended (mean alpha ..), unblended: ... no match, ... no previous tick, ...
  tick gap, ... cut/projection change/alpha 1`.
