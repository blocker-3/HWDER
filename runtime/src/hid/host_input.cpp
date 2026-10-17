// Host input: SDL3 gamepads (DualSense, DualShock 4, Switch Pro, Xbox, 8BitDo, ...) with gyro and rumble,
// XInput (loaded dynamically) when SDL cannot start, and a keyboard fallback while the HWDER window has focus.
#include "host_input.h"

#include "gpu/settings.h"

#include <windows.h>
#include <xinput.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace hid {

static_assert(kHostMisc6 == SDL_GAMEPAD_BUTTON_MISC6 && kHostLeftTrigger == SDL_GAMEPAD_BUTTON_COUNT,
              "HostButton mirrors SDL_GamepadButton");
static_assert(kPadTypeXboxOne == SDL_GAMEPAD_TYPE_XBOXONE, "kPadTypeXboxOne mirrors SDL_GamepadType");
static_assert(kHostButtonCount <= 32, "RawPad::buttons is a u32");

namespace {

std::atomic<bool> g_blocked{false};
std::atomic<bool> g_menu_request{false};
bool g_wait_release = false;

std::mutex g_raw_m;
RawPad g_raw;  // the HID thread writes it each poll, the overlay reads it

// Rumble the game asked for (from its service threads), applied on the HID thread.
std::atomic<float> g_rumble_low{0.0f}, g_rumble_high{0.0f};

constexpr s16 kTriggerThreshold = 8192;  // ZL/ZR press point, 25% of the travel
constexpr float kGravity = 9.80665f;
constexpr float kTwoPi = 6.2831853f;

// Remap table rows: settings::kRemapSlots Switch buttons in display order.
constexpr const char* kSlotNames[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "-", "L stick click", "R stick click",
                                      "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"};
constexpr u64 kSlotBits[] = {kBtnA, kBtnB, kBtnX, kBtnY, kBtnL, kBtnR, kBtnZL, kBtnZR, kBtnPlus, kBtnMinus,
                             kBtnStickL, kBtnStickR, kBtnUp, kBtnDown, kBtnLeft, kBtnRight};
static_assert(sizeof(kSlotBits) / sizeof(kSlotBits[0]) == settings::kRemapSlots, "one bit per remap row");
// Default by position: Xbox A (the bottom button) is Switch B, the button in the same place.
constexpr int kDefaultPositional[] = {kHostEast, kHostSouth, kHostNorth, kHostWest, kHostLeftShoulder, kHostRightShoulder,
                                      kHostLeftTrigger, kHostRightTrigger, kHostStart, kHostBack, kHostLeftStick,
                                      kHostRightStick, kHostDpadUp, kHostDpadDown, kHostDpadLeft, kHostDpadRight};

int family_of(int type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return settings::kPadXbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return settings::kPadPlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return settings::kPadNintendo;
    default: return settings::kPadGeneric;
    }
}

// View/Select: a tap is whatever it is bound to (the game's - button by default), a hold opens the in-game
// menu. The game only sees the tap once the button is released, held for long enough that a 60 Hz game
// frame samples it.
constexpr u64 kMenuHoldMs = 500, kMinusTapMs = 60;
u64 g_view_down_at = 0;
bool g_view_held = false;
u64 g_minus_until = 0;

void update_view_button(bool down, u64 tap_bits, u64& buttons) {
    if (!settings::input().hold_view_menu) {
        if (down) buttons |= tap_bits;
        g_view_down_at = 0;
        return;
    }
    const u64 now = GetTickCount64();
    if (down) {
        if (!g_view_down_at) g_view_down_at = now;
        if (!g_view_held && now - g_view_down_at >= kMenuHoldMs) {
            g_view_held = true;
            g_menu_request = true;
        }
    } else if (g_view_down_at) {
        if (!g_view_held) g_minus_until = now + kMinusTapMs;
        g_view_down_at = 0;
        g_view_held = false;
    }
    if (now < g_minus_until) buttons |= tap_bits;
}

s16 clamp16(int v) { return (s16)std::clamp(v, -kStickMax, kStickMax); }

// ------------------------------------------------------------------ XInput fallback
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XInputGetStateFn g_get_state = nullptr;
XInputSetStateFn g_set_state = nullptr;
bool g_xinput_loaded = false;
int g_active_pad = -1;
u32 g_probe_counter = 0;
bool g_pad_present[4] = {true, true, true, true};

void load_xinput() {
    if (g_xinput_loaded) return;
    g_xinput_loaded = true;
    const char* dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char* d : dlls) {
        HMODULE m = LoadLibraryA(d);
        if (!m) continue;
        g_get_state = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
        g_set_state = (XInputSetStateFn)GetProcAddress(m, "XInputSetState");
        if (g_get_state) {
            hw_log("hid: using %s", d);
            return;
        }
    }
    hw_log("hid: XInput not available - keyboard only");
}

void poll_xinput(RawPad& raw) {
    load_xinput();
    if (!g_get_state) return;
    // Disconnected pads are expensive to query; re-probe them only every ~0.5 s.
    bool probe = (++g_probe_counter % 100) == 0;
    g_active_pad = -1;
    for (DWORD i = 0; i < 4; i++) {
        if (!g_pad_present[i] && !probe) continue;
        XINPUT_STATE xs;
        if (g_get_state(i, &xs) != ERROR_SUCCESS) {
            g_pad_present[i] = false;
            continue;
        }
        g_pad_present[i] = true;
        if (g_active_pad >= 0) continue;
        g_active_pad = (int)i;
        const XINPUT_GAMEPAD& g = xs.Gamepad;
        static const std::pair<WORD, int> map[] = {
            {XINPUT_GAMEPAD_A, kHostSouth}, {XINPUT_GAMEPAD_B, kHostEast}, {XINPUT_GAMEPAD_X, kHostWest},
            {XINPUT_GAMEPAD_Y, kHostNorth}, {XINPUT_GAMEPAD_BACK, kHostBack}, {XINPUT_GAMEPAD_START, kHostStart},
            {XINPUT_GAMEPAD_LEFT_THUMB, kHostLeftStick}, {XINPUT_GAMEPAD_RIGHT_THUMB, kHostRightStick},
            {XINPUT_GAMEPAD_LEFT_SHOULDER, kHostLeftShoulder}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, kHostRightShoulder},
            {XINPUT_GAMEPAD_DPAD_UP, kHostDpadUp}, {XINPUT_GAMEPAD_DPAD_DOWN, kHostDpadDown},
            {XINPUT_GAMEPAD_DPAD_LEFT, kHostDpadLeft}, {XINPUT_GAMEPAD_DPAD_RIGHT, kHostDpadRight},
        };
        raw.connected = true;
        raw.type = SDL_GAMEPAD_TYPE_XBOXONE;
        raw.family = settings::kPadXbox;
        raw.has_rumble = true;
        snprintf(raw.name, sizeof(raw.name), "XInput controller %u", (unsigned)i + 1);
        for (auto& [bit, host] : map)
            if (g.wButtons & bit) raw.buttons |= 1u << host;
        raw.lt = (s16)(g.bLeftTrigger * 32767 / 255);
        raw.rt = (s16)(g.bRightTrigger * 32767 / 255);
        if (raw.lt > kTriggerThreshold) raw.buttons |= 1u << kHostLeftTrigger;
        if (raw.rt > kTriggerThreshold) raw.buttons |= 1u << kHostRightTrigger;
        raw.lx = clamp16(g.sThumbLX);
        raw.ly = clamp16(g.sThumbLY);
        raw.rx = clamp16(g.sThumbRX);
        raw.ry = clamp16(g.sThumbRY);
    }
}

// ------------------------------------------------------------------ SDL3
struct SdlPad {
    SDL_Gamepad* gp = nullptr;
    SDL_JoystickID id = 0;
    u64 last_used = 0;  // tick of the last button press or big stick move: the most recent pad is active
    u32 prev_buttons = 0;
    bool gyro = false, accel = false, rumble = false;
    bool sensors_on = false;
};
bool g_sdl_tried = false, g_sdl_ok = false;
std::vector<SdlPad> g_pads;
SDL_JoystickID g_rumble_pad = 0;
u16 g_rumble_sent[2] = {0, 0};
u64 g_rumble_at = 0;

void sdl_open(SDL_JoystickID id) {
    for (const SdlPad& p : g_pads)
        if (p.id == id) return;
    SDL_Gamepad* gp = SDL_OpenGamepad(id);
    if (!gp) {
        hw_log("hid: cannot open controller %u (%s)", (unsigned)id, SDL_GetError());
        return;
    }
    SdlPad p;
    p.gp = gp;
    p.id = id;
    p.gyro = SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO);
    p.accel = SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL);
    p.rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(gp), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    const char* name = SDL_GetGamepadName(gp);
    hw_log("hid: controller connected: %s (%s, vid %04x pid %04x%s%s)", name ? name : "?",
           pad_type_name(SDL_GetGamepadType(gp)), SDL_GetGamepadVendor(gp), SDL_GetGamepadProduct(gp),
           p.gyro && p.accel ? ", gyro" : "", p.rumble ? ", rumble" : "");
    g_pads.push_back(p);
}

void sdl_close(SDL_JoystickID id) {
    for (size_t i = 0; i < g_pads.size(); i++) {
        if (g_pads[i].id != id) continue;
        const char* name = SDL_GetGamepadName(g_pads[i].gp);
        hw_log("hid: controller disconnected: %s", name ? name : "?");
        SDL_CloseGamepad(g_pads[i].gp);
        g_pads.erase(g_pads.begin() + i);
        return;
    }
}

bool sdl_init() {
    // No SDL window: joystick events must flow without focus, from SDL's own joystick thread. Enhanced
    // reports turn on the DualShock 4 / DualSense gyro and rumble over Bluetooth.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_SENSOR)) {
        hw_log("hid: SDL init failed (%s) - falling back to XInput", SDL_GetError());
        return false;
    }
    const int v = SDL_GetVersion();
    hw_log("hid: SDL %d.%d.%d gamepad input", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; i++) sdl_open(ids[i]);
        SDL_free(ids);
    }
    if (!n) hw_log("hid: no controller connected yet");
    return true;
}

// Pumps SDL (hot-plug included), picks the most recently used pad and fills `raw` from it. `motion` gets
// its six-axis state in the Switch convention.
SdlPad* sdl_poll(bool want_sensors, RawPad& raw, MotionInput& motion) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_GAMEPAD_ADDED) sdl_open(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) sdl_close(e.gdevice.which);
    }
    const u64 now = GetTickCount64();
    SdlPad* best = nullptr;
    u32 best_buttons = 0;
    for (SdlPad& p : g_pads) {
        if (p.sensors_on != want_sensors) {
            if (p.gyro) SDL_SetGamepadSensorEnabled(p.gp, SDL_SENSOR_GYRO, want_sensors);
            if (p.accel) SDL_SetGamepadSensorEnabled(p.gp, SDL_SENSOR_ACCEL, want_sensors);
            p.sensors_on = want_sensors;
        }
        u32 b = 0;
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)
            if (SDL_GetGamepadButton(p.gp, (SDL_GamepadButton)i)) b |= 1u << i;
        if (SDL_GetGamepadAxis(p.gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kTriggerThreshold) b |= 1u << kHostLeftTrigger;
        if (SDL_GetGamepadAxis(p.gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kTriggerThreshold) b |= 1u << kHostRightTrigger;
        bool moved = false;
        for (SDL_GamepadAxis a : {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY})
            moved |= std::abs((int)SDL_GetGamepadAxis(p.gp, a)) > 20000;
        if ((b & ~p.prev_buttons) || moved) p.last_used = now;
        p.prev_buttons = b;
        if (!best || p.last_used > best->last_used) {
            best = &p;
            best_buttons = b;
        }
    }
    if (!best) return nullptr;
    SDL_Gamepad* gp = best->gp;
    raw.connected = true;
    raw.sdl = true;
    raw.buttons = best_buttons;
    raw.lx = clamp16(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
    raw.ly = clamp16(-(int)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));  // SDL: +y = down
    raw.rx = clamp16(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
    raw.ry = clamp16(-(int)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
    raw.lt = std::max<s16>(0, SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    raw.rt = std::max<s16>(0, SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    raw.type = SDL_GetGamepadType(gp);
    raw.family = family_of(raw.type);
    raw.has_gyro = best->gyro && best->accel;
    raw.has_rumble = best->rumble;
    const char* name = SDL_GetGamepadName(gp);
    snprintf(raw.name, sizeof(raw.name), "%s", name ? name : "Controller");

    float a[3], g[3];
    if (want_sensors && raw.has_gyro && SDL_GetGamepadSensorData(gp, SDL_SENSOR_ACCEL, a, 3) &&
        SDL_GetGamepadSensorData(gp, SDL_SENSOR_GYRO, g, 3)) {
        // SDL: x right, y up, z towards the player; m/s^2 and rad/s. Switch: gravity reads -1 on z when
        // the pad lies flat, y points away from the player; g and rotations/s.
        motion.valid = true;
        motion.acc[0] = -a[0] / kGravity;
        motion.acc[1] = a[2] / kGravity;
        motion.acc[2] = -a[1] / kGravity;
        motion.gyro[0] = g[0] / kTwoPi;
        motion.gyro[1] = -g[2] / kTwoPi;
        motion.gyro[2] = g[1] / kTwoPi;
    }
    return best;
}

// ------------------------------------------------------------------ rumble
void apply_rumble(SdlPad* active) {
    const settings::Input& cfg = settings::input();
    const float k = cfg.rumble ? cfg.rumble_strength / 100.0f : 0.0f;
    const u16 lo = (u16)(std::clamp(g_rumble_low.load(), 0.0f, 1.0f) * k * 65535.0f);
    const u16 hi = (u16)(std::clamp(g_rumble_high.load(), 0.0f, 1.0f) * k * 65535.0f);
    if (g_sdl_ok) {
        const SDL_JoystickID id = active ? active->id : 0;
        if (id != g_rumble_pad) {
            for (SdlPad& p : g_pads)
                if (p.id == g_rumble_pad) SDL_RumbleGamepad(p.gp, 0, 0, 0);
            g_rumble_pad = id;
            g_rumble_sent[0] = g_rumble_sent[1] = 0;
            g_rumble_at = 0;
        }
        if (!active || !active->rumble) return;
        // SDL rumble runs for a set time: refresh it while the game keeps it on.
        const u64 now = GetTickCount64();
        if (lo != g_rumble_sent[0] || hi != g_rumble_sent[1] || ((lo || hi) && now - g_rumble_at > 1000)) {
            SDL_RumbleGamepad(active->gp, lo, hi, (lo || hi) ? 2000 : 0);
            g_rumble_sent[0] = lo;
            g_rumble_sent[1] = hi;
            g_rumble_at = now;
        }
        return;
    }
    if (!g_set_state || g_active_pad < 0 || (lo == g_rumble_sent[0] && hi == g_rumble_sent[1])) return;
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = lo;
    v.wRightMotorSpeed = hi;
    g_set_state((DWORD)g_active_pad, &v);
    g_rumble_sent[0] = lo;
    g_rumble_sent[1] = hi;
}

// Radial deadzone rescaled so the stick still reaches full deflection; -1..1 per axis.
void deadzone(s16 x, s16 y, int dz_pct, float& ox, float& oy) {
    const float fx = x / (float)kStickMax, fy = y / (float)kStickMax;
    const float m = std::sqrt(fx * fx + fy * fy), dz = dz_pct / 100.0f;
    if (m <= dz || dz >= 1.0f) {
        ox = oy = 0.0f;
        return;
    }
    const float s = std::min(1.0f, (m - dz) / (1.0f - dz)) / m;
    ox = fx * s;
    oy = fy * s;
}

s32 to_axis(float v) { return (s32)std::lround(std::clamp(v, -1.0f, 1.0f) * kStickMax); }

bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

}  // namespace

// ------------------------------------------------------------------ remap table helpers
const char* switch_button_name(int slot) { return slot >= 0 && slot < settings::kRemapSlots ? kSlotNames[slot] : "?"; }
u64 switch_button_bit(int slot) { return slot >= 0 && slot < settings::kRemapSlots ? kSlotBits[slot] : 0; }
bool face_labels_fixed(int family) { return family == settings::kPadNintendo; }

void default_remap(int* out, int family, bool face_labels) {
    memcpy(out, kDefaultPositional, sizeof(kDefaultPositional));
    if (face_labels && !face_labels_fixed(family)) {
        // By letter: Xbox A / PlayStation Cross (the bottom button) = Switch A, and so on.
        out[0] = kHostSouth;
        out[1] = kHostEast;
        out[2] = kHostWest;
        out[3] = kHostNorth;
    }
}

const char* pad_type_name(int type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_STANDARD: return "Standard controller";
    case SDL_GAMEPAD_TYPE_XBOX360: return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox One / Series";
    case SDL_GAMEPAD_TYPE_PS3: return "DualShock 3";
    case SDL_GAMEPAD_TYPE_PS4: return "DualShock 4";
    case SDL_GAMEPAD_TYPE_PS5: return "DualSense";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro Controller";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return "Joy-Con (L)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return "Joy-Con (R)";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con pair";
    default: return "Controller";
    }
}

const char* host_button_name(int button, int type) {
    if (button < 0 || button >= kHostButtonCount) return "-";
    if (button <= kHostNorth) {
        switch (SDL_GetGamepadButtonLabelForType((SDL_GamepadType)type, (SDL_GamepadButton)button)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default: break;
        }
        static const char* pos[] = {"South", "East", "West", "North"};
        return pos[button];
    }
    if (button == kHostBack) {
        switch (type) {
        case SDL_GAMEPAD_TYPE_XBOXONE: return "View";
        case SDL_GAMEPAD_TYPE_PS3: return "Select";
        case SDL_GAMEPAD_TYPE_PS4: return "Share";
        case SDL_GAMEPAD_TYPE_PS5: return "Create";
        }
    }
    if (button == kHostStart) {
        switch (type) {
        case SDL_GAMEPAD_TYPE_XBOXONE: return "Menu";
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return "Options";
        }
    }
    // Everything else by family: Xbox, PlayStation, Nintendo, generic.
    static const char* names[][4] = {
        {"", "", "", ""}, {"", "", "", ""}, {"", "", "", ""}, {"", "", "", ""},  // face buttons, above
        {"Back", "Select", "-", "Back"},
        {"Xbox", "PS", "Home", "Guide"},
        {"Start", "Start", "+", "Start"},
        {"LS", "L3", "LS", "LS"},
        {"RS", "R3", "RS", "RS"},
        {"LB", "L1", "L", "LB"},
        {"RB", "R1", "R", "RB"},
        {"D-pad Up", "D-pad Up", "D-pad Up", "D-pad Up"},
        {"D-pad Down", "D-pad Down", "D-pad Down", "D-pad Down"},
        {"D-pad Left", "D-pad Left", "D-pad Left", "D-pad Left"},
        {"D-pad Right", "D-pad Right", "D-pad Right", "D-pad Right"},
        {"Share", "Mic", "Capture", "Misc"},
        {"Paddle R1", "Paddle R1", "Paddle R1", "Paddle R1"},
        {"Paddle L1", "Paddle L1", "Paddle L1", "Paddle L1"},
        {"Paddle R2", "Paddle R2", "Paddle R2", "Paddle R2"},
        {"Paddle L2", "Paddle L2", "Paddle L2", "Paddle L2"},
        {"Touchpad", "Touchpad", "Touchpad", "Touchpad"},
        {"Misc 2", "Misc 2", "Misc 2", "Misc 2"},
        {"Misc 3", "Misc 3", "Misc 3", "Misc 3"},
        {"Misc 4", "Misc 4", "Misc 4", "Misc 4"},
        {"Misc 5", "Misc 5", "Misc 5", "Misc 5"},
        {"Misc 6", "Misc 6", "Misc 6", "Misc 6"},
        {"LT", "L2", "ZL", "LT"},
        {"RT", "R2", "ZR", "RT"},
    };
    static_assert(sizeof(names) / sizeof(names[0]) == kHostButtonCount, "one name row per host button");
    return names[button][family_of(type)];
}

// ------------------------------------------------------------------ polling
void poll_host_input(HostInput& out) {
    out = {};
    PadInput& s = out.pad;
    const settings::Input& cfg = settings::input();

    if (!g_sdl_tried) {
        g_sdl_tried = true;
        g_sdl_ok = sdl_init();
    }
    DWORD fg_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
    const bool focused = fg_pid == GetCurrentProcessId();

    RawPad raw;
    SdlPad* active = nullptr;
    if (g_sdl_ok) active = sdl_poll(cfg.motion, raw, out.motion);
    else poll_xinput(raw);
    apply_rumble(active);
    if (!focused && !cfg.background_pad) {
        // Keep the pad's identity for the menu, drop its input.
        raw.buttons = 0;
        raw.lx = raw.ly = raw.rx = raw.ry = raw.lt = raw.rt = 0;
        out.motion = {};
    }
    {
        std::lock_guard<std::mutex> l(g_raw_m);
        g_raw = raw;
    }

    // Buttons through this controller family's remap table. Whatever View is bound to waits for the
    // View hold check (a hold opens the menu instead).
    const int* map = cfg.remap[raw.family];
    u64 tap_bits = 0;
    for (int i = 0; i < settings::kRemapSlots; i++) {
        const int hb = map[i];
        if (hb < 0 || hb >= kHostButtonCount) continue;
        if (hb == kHostBack) tap_bits |= kSlotBits[i];
        else if (raw.buttons & (1u << hb)) s.buttons |= kSlotBits[i];
    }
    update_view_button((raw.buttons >> kHostBack) & 1, tap_bits, s.buttons);

    float lx, ly, rx, ry;
    deadzone(raw.lx, raw.ly, cfg.deadzone_l, lx, ly);
    deadzone(raw.rx, raw.ry, cfg.deadzone_r, rx, ry);
    s.lx = to_axis(cfg.invert_lx ? -lx : lx);
    s.ly = to_axis(cfg.invert_ly ? -ly : ly);
    s.rx = to_axis(cfg.invert_rx ? -rx : rx);
    s.ry = to_axis(cfg.invert_ry ? -ry : ry);
    if (!cfg.motion) out.motion = {};

    if (focused) {
        // WASD move, arrows camera, K/J/I/L = A/B/X/Y (Enter = A, Backspace = B), Q/E = L/R, U/O = ZL/ZR,
        // Space/Tab = +/-, 1-4 = D-pad, Z/C = stick clicks.
        if (key('W')) s.ly = kStickMax;
        if (key('S')) s.ly = -kStickMax;
        if (key('A')) s.lx = -kStickMax;
        if (key('D')) s.lx = kStickMax;
        if (key(VK_UP)) s.ry = kStickMax;
        if (key(VK_DOWN)) s.ry = -kStickMax;
        if (key(VK_LEFT)) s.rx = -kStickMax;
        if (key(VK_RIGHT)) s.rx = kStickMax;
        if (key('K') || key(VK_RETURN)) s.buttons |= kBtnA;
        if (key('J') || key(VK_BACK)) s.buttons |= kBtnB;
        if (key('I')) s.buttons |= kBtnX;
        if (key('L')) s.buttons |= kBtnY;
        if (key('Q')) s.buttons |= kBtnL;
        if (key('E')) s.buttons |= kBtnR;
        if (key('U')) s.buttons |= kBtnZL;
        if (key('O')) s.buttons |= kBtnZR;
        if (key(VK_SPACE)) s.buttons |= kBtnPlus;
        if (key(VK_TAB)) s.buttons |= kBtnMinus;
        if (key('Z')) s.buttons |= kBtnStickL;
        if (key('C')) s.buttons |= kBtnStickR;
        if (key('1')) s.buttons |= kBtnUp;
        if (key('2')) s.buttons |= kBtnDown;
        if (key('3')) s.buttons |= kBtnLeft;
        if (key('4')) s.buttons |= kBtnRight;
    }

    const s32 t = kStickMax / 2;
    if (s.lx < -t) s.buttons |= kBtnStickLLeft;
    if (s.lx > t) s.buttons |= kBtnStickLRight;
    if (s.ly > t) s.buttons |= kBtnStickLUp;
    if (s.ly < -t) s.buttons |= kBtnStickLDown;
    if (s.rx < -t) s.buttons |= kBtnStickRLeft;
    if (s.rx > t) s.buttons |= kBtnStickRRight;
    if (s.ry > t) s.buttons |= kBtnStickRUp;
    if (s.ry < -t) s.buttons |= kBtnStickRDown;

    if (g_blocked) {
        out = {};
        g_wait_release = true;
    } else if (g_wait_release) {
        if (s.buttons) out = {};
        else g_wait_release = false;
    }
}

RawPad raw_pad() {
    std::lock_guard<std::mutex> l(g_raw_m);
    return g_raw;
}

void set_game_input_blocked(bool blocked) { g_blocked = blocked; }
bool take_menu_request() { return g_menu_request.exchange(false); }

void set_rumble(float low, float high) {
    g_rumble_low = low;
    g_rumble_high = high;
}

}  // namespace hid
