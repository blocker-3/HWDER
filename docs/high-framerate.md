# High frame rate (120/144 Hz): plan

Hyrule Warriors runs its logic at 60 ticks per second, locked to presented frames. Two ways to show
more frames than that:

| | Stage 1: GPU replay (interp-120, works today) | Stage 2: game-drawn in-between frames |
|---|---|---|
| What runs per in-between frame | the recorded Vulkan commands of the last frame, replayed | the game's own Draw, without Update |
| What moves smoothly | the camera only | camera, characters, objects (then effects) |
| How | exact copies of the tick's view/viewProj in constant buffers are replaced by a blended camera | blended inputs (camera, object matrices) go into the render snapshot; the game derives everything else |
| Weak spots | camera-derived values the game computes differently stay at the tick (shadow cascades, culling, fog); objects step at 60 Hz | Draw may change game state (particles, animation, sound): each case needs a guard |
| Reference | `docs/camera.md` | ZeldaWWHDRecomp `runtime/src/interp.cpp`, `docs/decomp-notes.md` ("Interpolation") |

Stage 1 stays available as the fallback while stage 2 is built.

## Stage 1: ship the camera-only mode as an option

1. Menu option "Smooth camera (experimental)" with the output rate (match display / 120 / 144),
   replacing the `HWDER_INTERP_MODE=camera` + `HWDER_INTERP=-1` switches.
2. Pacing from ZeldaWWHDRecomp: cap to the display's refresh rate; when an in-between frame does
   not fit its slot, show the real frame instead of slowing the game down.
3. Max plays it; the 120 fps capture tests (`HWDER_AUTOPLAY=2`, filmstrip) stay the regression check.

## Stage 2: the game draws its own in-between frames

### 2.1 Map the frame (read the generated code, a few short test runs)
- The per-tick engine object: `f_10001766a4` (publishes the render cameras via `f_1000173270` →
  `f_10003d61a4` → `f_10003d45cc`) and its siblings `f_100017473c` / `f_100017672c` /
  `f_10001767e8` (→ `f_10003d1c9c` → `f_10003d07ec` → `f_10003d0a74`, which uploads the camera
  constants). Find the vtable they sit in, the caller of each, and the order per tick.
- Which guest thread runs the Draw side; how the render-buffer index (`S + 0x710dc` write,
  `S + 0x710e0` read) hands a tick's snapshot from Update to Draw.
- What Draw writes besides GPU commands (guest memory written during Draw: particle/animation
  state, counters, sound requests).
- Deliverable: `docs/frame.md` with the call tree per tick and the state Draw changes.

### 2.2 Camera, drawn by the game
- Hook point: after a tick's Draw, run Draw again for the same snapshot with the render camera
  replaced by the blended one (game's own BuildView/Update on blended eye/at/up/roll, reproduced
  exactly already), once per in-between frame; present each.
- Guard every side effect found in 2.1 on these extra passes (as ZeldaWWHDRecomp does for sound,
  one-shot no-draw flags, counters).
- Check: in-between frames from the game match stage 1's camera, and shadows, fog and culling
  follow the blended camera.

### 2.3 Characters and objects
- Find the per-object world matrices in the render snapshot (or the function that writes them).
- Blend them between the previous and current tick (translation lerp, rotation slerp, snap on
  teleport/spawn or jumps > a threshold), restore the exact ones after each extra pass.

### 2.4 Effects and edge cases
- Particles, trails, attack effects; cutscenes, menus, pause, loading screens, movies.
- Pacing as in stage 1 (drop in-between frames that do not fit).

Each step is testable on its own; after 2.1 the effort of 2.2-2.4 can be estimated properly, or
stage 2 stopped if Draw is too entangled with game state.
