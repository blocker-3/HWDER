// Guest structures of the game, as found in docs/camera.md and docs/frame-loop.md. Offsets are checked
// at compile time, so a layout mistake fails the build instead of silently reading the wrong field.
//
// Guest memory is identity mapped: a guest address is a host pointer (hwder/cpu.h), so a `Camera*`
// obtained with game::at<Camera>(addr) reads and writes the game's own object. Fields the game aligns
// to 16 bytes are padded explicitly; nothing here is packed.
#pragma once
#include <stddef.h>

#include "hwder/cpu.h"

namespace game {

// The game's camera object (0x170 bytes). Camera_BuildView reads eye/at/up (+roll) into view; Camera_BuildProj
// writes proj from fov/aspect/near/far; Camera_Update multiplies them into viewproj.
struct Camera {
    u8 header[0x10];
    float eye[3], eye_pad;      // +0x10
    float at[3], at_pad;        // +0x20
    float up[3], up_pad;        // +0x30
    float basis[16];            // +0x40 view basis before roll
    float view[16];             // +0x80 row-major as stored
    float proj[16];             // +0xc0
    float viewproj[16];         // +0x100
    float near_, far_;          // +0x140, +0x144
    float fov;                  // +0x148 vertical fov, radians
    float aspect;               // +0x14c
    float roll;                 // +0x150
    float proj_extra;           // +0x154
    u32 flags;                  // +0x158 bit 0: orthographic (size from eye-target distance), bit 1: near/far swapped
    float aperture;             // +0x15c
    u8 view_dirty, proj_dirty;  // +0x160, +0x161 (Camera_Update rebuilds what is dirty)
    u8 tail[0x170 - 0x162];
    static constexpr u32 kOrthographic = 1u;
    bool orthographic() const { return (flags & kOrthographic) != 0; }
};
static_assert(sizeof(Camera) == 0x170, "Camera size");
static_assert(offsetof(Camera, eye) == 0x10, "Camera::eye");
static_assert(offsetof(Camera, at) == 0x20, "Camera::at");
static_assert(offsetof(Camera, up) == 0x30, "Camera::up");
static_assert(offsetof(Camera, view) == 0x80, "Camera::view");
static_assert(offsetof(Camera, proj) == 0xc0, "Camera::proj");
static_assert(offsetof(Camera, viewproj) == 0x100, "Camera::viewproj");
static_assert(offsetof(Camera, near_) == 0x140, "Camera::near");
static_assert(offsetof(Camera, fov) == 0x148, "Camera::fov");
static_assert(offsetof(Camera, aspect) == 0x14c, "Camera::aspect");
static_assert(offsetof(Camera, roll) == 0x150, "Camera::roll");
static_assert(offsetof(Camera, proj_extra) == 0x154, "Camera::proj_extra");
static_assert(offsetof(Camera, flags) == 0x158, "Camera::flags");
static_assert(offsetof(Camera, aperture) == 0x15c, "Camera::aperture");
static_assert(offsetof(Camera, proj_dirty) == 0x161, "Camera::proj_dirty");

// Render cameras live in the scene object S, triple-buffered: S + kRenderCamBase + (slot % 3) * kRenderCamBufStride
// + screen * sizeof(Camera). The write slot index is at S + kRenderCamWriteSlot (docs/camera.md).
namespace scene {
constexpr u64 kRenderCamBase = 0x71360;
constexpr u64 kRenderCamBufStride = 0x2e0;  // two screens of 0x170
constexpr u64 kRenderCamWriteSlot = 0x710dc;
constexpr u64 kRenderCamReadSlot = 0x710e0;
constexpr u32 kVtablePresentWindowQuery = 192;  // vcall scene+192: does the scene want GfxDevice_PresentWindow this frame
inline u64 render_camera(u64 scene_addr, u32 slot, u32 screen) {
    return scene_addr + kRenderCamBase + (slot % 3) * kRenderCamBufStride + screen * sizeof(Camera);
}
}  // namespace scene

// Battle_Tick's frame state (x20): [[state + kFrameStateDevice] + kSkipPresent] non-zero skips the present.
namespace frame_state {
constexpr u64 kDevicePtr = 544;
constexpr u64 kSkipPresent = 0x2d420;
}  // namespace frame_state

}  // namespace game
