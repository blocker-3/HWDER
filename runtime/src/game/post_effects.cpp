// Depth of field and radial blur switches (Graphics > Picture).
//
// The game's post-effect uber shader gets its depth of field from the render camera: 0x10003d0a74
// (x0 = post effect, x4 = camera) uploads fEyeToAtLength (|eye - at|) and fAperture (camera +0x15c,
// see docs/camera.md). With "Depth of field" off the hook runs it with the aperture at 0, which the
// shader treats as a pinhole (everything in focus); the camera keeps its value.
//
// The radial (zoom) blur of special attacks is set through 0x10003d0928 (x0 = post effect, w2 = screen,
// s0/s1 = vBlurParam), called each frame by the special-attack screen effect (0x10002244fc). With "Radial
// blur" off the hook passes zero strength.
#include <cstring>

#include "gpu/settings.h"
#include "hwder/runtime.h"

extern "C" void f_10003d0a74_orig(Ctx* c);
extern "C" void f_10003d0928_orig(Ctx* c);

extern "C" void f_10003d0a74(Ctx* c) {
    if (settings::video().dof || !c->x[4]) {
        f_10003d0a74_orig(c);
        return;
    }
    u8* cam = GUESTP(u8, c->x[4]);
    float ap, zero = 0.0f;
    memcpy(&ap, cam + 0x15c, 4);
    memcpy(cam + 0x15c, &zero, 4);
    f_10003d0a74_orig(c);
    memcpy(cam + 0x15c, &ap, 4);
}

extern "C" void f_10003d0928(Ctx* c) {
    if (!settings::video().radial_blur) {
        c->v[0].f[0] = 0.0f;
        c->v[1].f[0] = 0.0f;
    }
    f_10003d0928_orig(c);
}
