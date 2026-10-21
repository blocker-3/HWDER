// The 120 fps (game-drawn in-between frame) module's interface to the rest of the runtime. One place for
// the symbols that nvdrv, vi, hid and the renderer used to declare ad hoc. See docs/high-framerate.md.
#pragma once
#include <atomic>

#include "hwder/cpu.h"

extern "C" {
// Tag of the GPU work being submitted: (tick << 1) | 1 during an extra frame. nvdrv copies it into each
// submission; the renderer reads it back while executing (gpu::exec_gi_tag) and blends object transforms.
extern std::atomic<unsigned long long> hw_gi_submit_tag;
// Draws in the last presented frame (gpu/vk). Read by the autoplay input script to tell battle from menus.
extern std::atomic<u32> hw_last_frame_draws;
// Whether the 120 fps mode is on (setting or HWDER_GAME_INTERP override).
bool hw_game_interp_on();
// QueueBuffer (vi.cpp, guest thread): release rate of the frame being queued. 120 for an extra frame and its
// paired real frame, 60 for an unpaired tick with the mode on, 0 for the presenter's own defaults.
u32 hw_game_interp_release_hz();
// IPC (ipc.cpp): counts service calls made by the extra-draw thread (HWDER_GI_LOG diagnostics).
void hw_gi_note_ipc(const char* service, u32 command);
// game/widescreen.cpp, once per game tick from the camera hook: re-apply the window aspect when it changed.
void hw_widescreen_tick();
}
