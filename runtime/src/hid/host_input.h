// Host input sources (SDL3 gamepads, XInput as a fallback, keyboard on the HWDER window), already
// translated to Switch semantics. Polled by the HID update thread.
#pragma once
#include "hwder/runtime.h"

namespace hid {

// nn::hid::NpadButton bits
enum : u64 {
    kBtnA = 1ull << 0, kBtnB = 1ull << 1, kBtnX = 1ull << 2, kBtnY = 1ull << 3,
    kBtnStickL = 1ull << 4, kBtnStickR = 1ull << 5, kBtnL = 1ull << 6, kBtnR = 1ull << 7,
    kBtnZL = 1ull << 8, kBtnZR = 1ull << 9, kBtnPlus = 1ull << 10, kBtnMinus = 1ull << 11,
    kBtnLeft = 1ull << 12, kBtnUp = 1ull << 13, kBtnRight = 1ull << 14, kBtnDown = 1ull << 15,
    kBtnStickLLeft = 1ull << 16, kBtnStickLUp = 1ull << 17, kBtnStickLRight = 1ull << 18, kBtnStickLDown = 1ull << 19,
    kBtnStickRLeft = 1ull << 20, kBtnStickRUp = 1ull << 21, kBtnStickRRight = 1ull << 22, kBtnStickRDown = 1ull << 23,
};

constexpr s32 kStickMax = 0x7FFF;

struct PadInput {
    u64 buttons = 0;
    s32 lx = 0, ly = 0, rx = 0, ry = 0;  // -32767..32767, +y = up
};

// Six-axis state in the Switch convention (SixAxisSensorState): acceleration in g, angular velocity in
// rotations per second. Invalid = no sensor; the game then sees a controller at rest.
struct MotionInput {
    bool valid = false;
    float acc[3] = {};
    float gyro[3] = {};
};

struct TouchInput {
    bool down = false;
    u32 x = 0, y = 0;  // 1280x720 touch panel space
};

// Player slots: npad ids 0..3. Host controllers take the lowest free slot as they connect (SDL order, or
// the XInput user index order while SDL has no pad open); the keyboard joins per Settings > Input.
constexpr int kPlayerSlots = 4;

struct HostInput {
    PadInput pads[kPlayerSlots];     // per player slot, already remapped
    bool connected[kPlayerSlots] = {};  // a controller or the keyboard drives this slot
    MotionInput motion;              // player 1's controller
    TouchInput touch;
};

// Host gamepad buttons: SDL_GamepadButton's values (positional: South is Xbox A / PlayStation Cross /
// Nintendo B), then the two triggers as buttons.
enum HostButton : int {
    kHostSouth, kHostEast, kHostWest, kHostNorth, kHostBack, kHostGuide, kHostStart, kHostLeftStick,
    kHostRightStick, kHostLeftShoulder, kHostRightShoulder, kHostDpadUp, kHostDpadDown, kHostDpadLeft,
    kHostDpadRight, kHostMisc1, kHostRightPaddle1, kHostLeftPaddle1, kHostRightPaddle2, kHostLeftPaddle2,
    kHostTouchpad, kHostMisc2, kHostMisc3, kHostMisc4, kHostMisc5, kHostMisc6,
    kHostLeftTrigger, kHostRightTrigger, kHostButtonCount
};

constexpr int kPadTypeXboxOne = 3;  // SDL_GAMEPAD_TYPE_XBOXONE: labels shown while no controller is connected

// The active controller as the host sees it, before remapping, deadzones and the in-game menu's block.
// The overlay reads it for menu navigation and button capture.
struct RawPad {
    bool connected = false;
    bool sdl = false;        // false = the XInput fallback
    u32 buttons = 0;         // 1 << HostButton
    s16 lx = 0, ly = 0, rx = 0, ry = 0;  // +y = up
    s16 lt = 0, rt = 0;      // 0..32767
    int type = kPadTypeXboxOne;  // SDL_GamepadType (XInput: Xbox One)
    int family = 0;          // settings::PadFamily
    bool has_gyro = false, has_rumble = false;
    char name[96] = {};
};
RawPad raw_pad();  // thread safe

// Remap table helpers (settings::kRemapSlots rows: A B X Y L R ZL ZR + - L3 R3 Up Down Left Right).
const char* switch_button_name(int slot);
u64 switch_button_bit(int slot);
void default_remap(int* out, int family, bool face_labels);
bool face_labels_fixed(int family);                // Nintendo layout: positions and labels coincide
const char* host_button_name(int button, int type);  // as printed on that controller type: "Cross", "LB", "ZL"
const char* pad_type_name(int type);               // "DualSense", "Xbox One / Series", ...

// Poll every host source (cheap; safe to call at 1 kHz). Not thread safe: call from one thread.
void poll_host_input(HostInput& out);
#ifndef _WIN32
// SDL's main event thread publishes keyboard and hotplug state to the HID thread.
void host_keyboard_event(int scancode, bool down);
void host_keyboard_clear();
void poll_prelaunch_input();
void start_guest_polling();
void host_gamepad_event(u32 type, u32 id);
#endif

// In-game menu: while blocked the game sees a neutral pad and no touch. After unblocking, the game
// sees nothing until every button and stick is released, so the press that closed the menu stays out.
void set_game_input_blocked(bool blocked);
// True once each time View/Select is held past the menu threshold (Settings > Input). The overlay polls it.
bool take_menu_request();

// Which slots currently have a controller or the keyboard (bit per slot). Thread safe; the controller applet
// reads it to answer the game's player-count request.
u32 connected_slots();
// Name of the device in a slot for the menu ("" = empty). Thread safe.
void slot_name(int slot, char* out, size_t n);

// Rumble on the pad in `slot`, applied by the HID thread. Amplitudes 0..1 (low band -> big motor, high band ->
// small motor), scaled by Settings > Input > Rumble strength.
void set_rumble(int slot, float low, float high);
// Short full-amplitude pulse at the current strength, so the Input tab's slider can be felt while it moves.
void preview_rumble();

}  // namespace hid
