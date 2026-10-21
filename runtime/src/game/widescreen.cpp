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

#include <algorithm>
#include <cmath>

#include "gpu/display.h"
#include "gpu/settings.h"
#include "hwder/runtime.h"

namespace {

constexpr float k169 = 16.0f / 9.0f;
float g_set = 0.0f;  // the last aspect this hook wrote (to undo it when the option is turned off)

// Perspective cameras this hook has built a projection for (guest addresses, most recent kept). The game
// rebuilds a projection only when its parameters change (Camera::Update checks the dirty byte +353), so a
// window resize alone would leave the old aspect in place: hw_widescreen_tick marks these dirty instead.
u64 g_cams[8];
u32 g_ncams = 0;

void remember(u64 addr) {
    for (u32 i = 0; i < std::min(g_ncams, 8u); i++)
        if (g_cams[i] == addr) return;
    g_cams[g_ncams++ % 8] = addr;
}

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

// Once per frame (game thread, interp/camera.cpp's render-camera copy): when the window's aspect or the fov
// setting changed, ask the remembered cameras to rebuild their projection on their next update. A camera
// is only touched while it still looks like one this hook handled (perspective, our or a 16:9 aspect).
extern "C" void hw_widescreen_tick() {
    static float last_wa = -1.0f, last_fov = -1.0f;
    const float wa = wide_aspect(), fov = (float)settings::video().fov;
    if (wa == last_wa && fov == last_fov) return;
    const bool first = last_wa < 0.0f;
    last_wa = wa, last_fov = fov;
    if (first) return;
    u32 n = 0;
    for (u32 i = 0; i < std::min(g_ncams, 8u); i++) {
        u8* cam = GUESTP(u8, g_cams[i]);
        if (!cam) continue;
        u32 flags;
        float aspect;
        memcpy(&flags, cam + 0x158, 4);
        memcpy(&aspect, cam + 0x14c, 4);
        if ((flags & 1) || !(aspect == g_set || std::fabs(aspect - k169) < 0.01f)) continue;
        cam[353] = 1;  // projection dirty
        n++;
    }
    hw_log("widescreen: window aspect %.3f, fov %.0f: projection rebuild requested for %u cameras", wa, fov, n);
}

extern "C" void f_100057cee0(Ctx* c) {
    u8* cam = GUESTP(u8, c->x[0]);
    u32 flags;
    memcpy(&flags, cam + 0x158, 4);
    if (!(flags & 1)) {  // perspective
        remember(c->x[0]);
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
    // Field of view (Graphics > Picture): widen the perspective for this build only. The camera keeps the
    // game's own fov, so its camera logic never sees (or compounds) the change; the projection, and with it
    // the game's culling, does. k scales tan(fov / 2), i.e. the visible extent at a given distance.
    float k = settings::video().fov / 100.0f;
    float fov = 0.0f;
    if (!(flags & 1) && k != 1.0f) {
        memcpy(&fov, cam + 0x148, 4);
        float wide = 2.0f * std::atan(std::tan(fov * 0.5f) * k);
        if (wide > 0.0f && wide < 3.0f) memcpy(cam + 0x148, &wide, 4);
        else fov = 0.0f;
    }
    f_100057cee0_orig(c);
    if (fov != 0.0f) memcpy(cam + 0x148, &fov, 4);
}
