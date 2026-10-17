// True ultrawide ("Fill ultrawide screens"): give the game's perspective cameras the window's aspect.
//
// The game's cameras are 16:9. With Unlocked resolution on a wider window the renderer can only
// pillarbox, or stretch the picture and squeeze it back, which shows geometry the game has already
// culled (objects popping in at the sides). Widening the camera itself makes the game build a wider
// projection, so its culling, screen-space effects and the picture all cover the full width. The
// guest still renders into 16:9 targets; the renderer allocates them at the window's shape, which
// undoes the horizontal squeeze (runtime/src/gpu/vk/vk_renderer.cpp, unlocked scale).
//
// Hook: Camera::BuildProj (0x100057cee0, x0 = camera). The camera layout is in docs/camera.md:
// +0x148 vertical fov, +0x14c aspect, +0x158 flags (bit 0: orthographic).
#include <windows.h>

#include <cmath>

#include "gpu/display.h"
#include "gpu/settings.h"
#include "hwder/runtime.h"

namespace {

constexpr float k169 = 16.0f / 9.0f;
float g_set = 0.0f;  // the last aspect this hook wrote (to undo it when the option is turned off)

// The window's aspect when filling an ultrawide screen, else 0.
float wide_aspect() {
    const settings::Video& v = settings::video();
    if (!v.unlocked_res || !v.ultrawide) return 0.0f;
    unsigned w = 0, h = 0;
    display::client_size(&w, &h);
    if (!w || !h) w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    float a = (float)w / (float)h;
    return a > k169 + 0.01f ? a : 0.0f;
}

}  // namespace

extern "C" void f_100057cee0_orig(Ctx* c);

extern "C" void f_100057cee0(Ctx* c) {
    u8* cam = GUESTP(u8, c->x[0]);
    u32 flags;
    memcpy(&flags, cam + 0x158, 4);
    if (!(flags & 1)) {  // perspective
        float aspect;
        memcpy(&aspect, cam + 0x14c, 4);
        float wa = wide_aspect();
        float want = aspect;
        if (wa > 0.0f && (std::fabs(aspect - k169) < 0.01f || aspect == g_set)) want = wa;  // a 16:9 camera (or one we widened)
        else if (wa == 0.0f && g_set != 0.0f && aspect == g_set) want = k169;               // option turned off: restore
        if (want != aspect) {
            memcpy(cam + 0x14c, &want, 4);
            if (want != k169) {
                if (g_set != want) hw_log("widescreen: camera aspect %.3f -> %.3f", aspect, want);
                g_set = want;
            }
        }
    }
    f_100057cee0_orig(c);
}
