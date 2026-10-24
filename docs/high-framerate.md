# Game-drawn 120 fps

Hyrule Warriors advances logic at 60 ticks per second, tied to presented frames. The 120 fps mode
(Settings > Display, or `HWDER_GAME_INTERP=1`) redraws the scene once more per battle tick with a halfway camera, then
draws the original tick. The game does not run its Update jobs again for the extra frame.
Native 60 remains the default. The jagged-flash fix (whole-block object blending) is merged and playtested;
a slight ghost on very fast limbs remains. The measured loop and research history are in `docs/frame-loop.md`.

## Configuration and pacing

Enable "120 fps (experimental)" in Settings > Display > Frame rate; it defaults off and is saved as
`fps120=`. The test override `HWDER_GAME_INTERP=0/1` takes precedence over that setting.
No `HWDER_FPS` setting is required.
QueueBuffer captures the calling thread's frame classification and carries it to the compositor:

| Queued frame | Release grid |
|---|---|
| Extra battle frame and its paired real frame | Fixed 120 Hz, one release slot each |
| Battle tick whose extra was skipped/rejected | Native 60 Hz, honoring guest swap interval |
| Menus, pause, loading and other single-present screens | Native 60 Hz, honoring guest swap interval |

The UI publishes an atomic request. Every non-extra BeginFrame latches that request for its tick and
clears the previous pair; actually queueing an extra establishes the new pair. Toggling the option
mid-tick finishes that tick's pair at its captured rate; subsequent ticks use the new request.
Classification belongs to the queued frame, so compositor lag does not carry battle pacing
into a menu. There is no 100 ms freshness grace. `HWDER_FPS` and `HWDER_FORCE_INTERVAL1` are ignored
while this mode controls presentation. With it off, existing native defaults and diagnostic
environment overrides retain their previous behavior.

The old renderer replay interpolation (`HWDER_INTERP`, `HWDER_INTERP_MODE=camera`, the `interp_hz`
setting) has been removed; game-drawn interpolation is the only high frame rate path.

## What the extra frame interpolates

The camera hook records each published render camera. Consecutive snapshots are blended at halfway
eye/target/up/roll, then rebuilt with the game's Camera::Update so derived view/projection state uses
the same camera. Camera cuts, projection changes, stale snapshots and mismatched live camera bytes
reject the extra pass. Its original 0x170-byte camera is restored before the real draw.

GPU submissions carry the tick and extra-frame tag; subsequent native frames clear that tag, while
already-queued GPU work retains its classification after a UI toggle. The renderer records owned copies of each real
draw's vertex-stage c3 transform data and blends matching world/bone matrices in the extra draw's GPU
copy. A block (a world matrix or a whole bone palette) is blended entirely or left exactly at the newest
tick: blending only some bones of a skinned mesh stretched it into the giant jagged shapes seen at 120 fps.
Matching uses model draw keys, nearby first transforms and the closest whole block; the palette's 48-byte
grid is chosen by row orthogonality. This is approximate linear matrix interpolation, not rotation slerp
or persistent object identity; very fast moves (over 60 units a tick), crowds of identical poses and
layouts outside the recognized heuristics stay at 60 Hz, and a slight ghost remains on fast limbs.
The earlier strict and per-bone (legacy) matching modes were removed with the fix.
`HWDER_GI_OBJ_TRACE=1` logs the blocks near the camera target, and `HWDER_GI_NO_OBJECTS=1` disables
object interpolation for comparison.

In 120 fps mode the presenter copies each frame (after its rendering finished) into one of three images it owns and
returns the guest buffer before the 120 Hz release wait (`capture_for_present`, `on_captured` in vi.cpp): the game
has only three swap buffers, and holding them through that wait made busy battles skip every other in-between frame.
Without a copy (it failed) a guest buffer is returned only after rendering and its presentation blit's source read
finish. Timeouts produce diagnostics and further waits rather than freeing pending storage. Vulkan
presentation-wait errors use the existing fatal path. A running, capped lateness budget skips an extra
draw when battle ticks fall behind, with no consecutive skips.

## Side effects and validation limits

Stage d is experimental diagnostics. `hw_gi_note_ipc` counts synchronous service calls made by the
extra-draw thread; its thread-local scope avoids sharing unsynchronized flags with other guest
threads. Earlier battle samples showed only GPU/buffer IPC and no visibly doubled effects, but this
does not establish a general audio/input/particle/spawn fence. It neither suppresses service calls nor
identifies every guest-memory update or work scheduled on another thread.

Playtests of the integrated candidate must cover moving Link and enemies, attacks/bone motion,
particles/trails, dense spawns, audio, pause/resume, loading/camera cuts and menu speed. Native-default
play and ultrawide side bands also need checking after renderer integration. Historical frame-loop
measurements document earlier owner builds, not acceptance of every later combination.

`HWDER_GI_LOG=1` logs tick timing and extra-frame skip reasons. `HWDER_GI_LOG_MS` selects the diagnostic
report interval (default 5000 ms). The research switches described in `docs/frame-loop.md`
(`HWDER_FRAME_PROBE`, `HWDER_DRAW_WRITES`, `HWDER_DRAW_TWICE`, `HWDER_CBUF_DUMP`, `HWDER_CAM_LOG`) are
historical and no longer in the code.

## Busy battles (2026-10-24)

Measured on a ~1500-draw stage-4 battle (`hywar/docs/busy-stage-perf.md`): the GPU command thread was the limit
(12.5 ms per frame on main). Batched write-watch queries, persistent vertex/index ranges, the copy-and-return presenter
above, a P-core preference on hybrid CPUs, exact guest sleeps and pacer catch-up bring 120 mode to 8.35 ms per frame
there and native 60 to 16.7 ms.
