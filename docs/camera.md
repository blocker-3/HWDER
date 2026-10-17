# The game's camera (main module)

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
  `S + 0x71360 + (tick % 3) * 0x2e0 + screen * 0x170`. The tick counter for writing is `S + 0x710dc`.
  The render side reads its slot with `S + 0x710e0` (0x10003d6634, `GetRenderCamera(S, screen)`).
* **Per-tick sync:** `0x10003d45cc(S, tick)`, reached through `0x10003d61a4(S)` from 0x1000173270 and
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
