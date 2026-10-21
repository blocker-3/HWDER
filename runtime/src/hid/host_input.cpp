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

// Rumble the game asked for per player slot (from its service threads), applied on the HID thread.
std::atomic<float> g_rumble_low[kPlayerSlots], g_rumble_high[kPlayerSlots];
std::atomic<u64> g_preview_until{0};  // GetTickCount64() until which preview_rumble's pulse runs

// Slots with a controller or the keyboard, and the device names, for the controller applet and the menu.
std::atomic<u32> g_connected_mask{0};
char g_slot_names[kPlayerSlots][96] = {};  // under g_raw_m

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
u64 g_view_down_at[kPlayerSlots] = {};
bool g_view_held[kPlayerSlots] = {};
u64 g_minus_until[kPlayerSlots] = {};

// Per slot: any player's View hold opens the menu.
void update_view_button(int slot, bool down, u64 tap_bits, u64& buttons) {
    u64& down_at = g_view_down_at[slot];
    bool& held = g_view_held[slot];
    u64& minus_until = g_minus_until[slot];
    if (!settings::input().hold_view_menu) {
        if (down) buttons |= tap_bits;
        down_at = 0;
        return;
    }
    const u64 now = GetTickCount64();
    if (down) {
        if (!down_at) down_at = now;
        if (!held && now - down_at >= kMenuHoldMs) {
            held = true;
            g_menu_request = true;
        }
    } else if (down_at) {
        if (!held) minus_until = now + kMinusTapMs;
        down_at = 0;
        held = false;
    }
    if (now < minus_until) buttons |= tap_bits;
}

s16 clamp16(int v) { return (s16)std::clamp(v, -kStickMax, kStickMax); }

// ------------------------------------------------------------------ XInput fallback
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XInputGetStateFn g_get_state = nullptr;
XInputSetStateFn g_set_state = nullptr;
bool g_xinput_loaded = false;
int g_xi_user[kPlayerSlots] = {-1, -1, -1, -1};  // XInput user index per player slot (-1 = none)
u16 g_xi_sent[kPlayerSlots][2] = {};
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

// Present XInput pads take the player slots in user-index order (user 1 is player 1, a gap closes up).
void poll_xinput(RawPad raws[kPlayerSlots]) {
    load_xinput();
    for (int& u : g_xi_user) u = -1;
    if (!g_get_state) return;
    // Disconnected pads are expensive to query; re-probe them only every ~0.5 s.
    bool probe = (++g_probe_counter % 100) == 0;
    int slot = 0;
    for (DWORD i = 0; i < 4; i++) {
        if (!g_pad_present[i] && !probe) continue;
        XINPUT_STATE xs;
        if (g_get_state(i, &xs) != ERROR_SUCCESS) {
            g_pad_present[i] = false;
            continue;
        }
        g_pad_present[i] = true;
        if (slot >= kPlayerSlots) continue;
        RawPad& raw = raws[slot];
        g_xi_user[slot++] = (int)i;
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
    int slot = -1;      // player slot (-1: more pads than slots)
    u64 last_used = 0;  // tick of the last button press or big stick move: the most recent pad is shown in the menu
    u32 prev_buttons = 0;
    bool gyro = false, accel = false, rumble = false;
    bool sensors_on = false;
    u16 rumble_sent[2] = {0, 0};
    u64 rumble_at = 0;
};
bool g_sdl_tried = false, g_sdl_ok = false;
std::vector<SdlPad> g_pads;

int free_slot() {
    for (int s = 0; s < kPlayerSlots; s++) {
        bool used = false;
        for (const SdlPad& p : g_pads) used |= p.slot == s;
        if (!used) return s;
    }
    return -1;
}

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
    p.slot = free_slot();
    p.gyro = SDL_GamepadHasSensor(gp, SDL_SENSOR_GYRO);
    p.accel = SDL_GamepadHasSensor(gp, SDL_SENSOR_ACCEL);
    p.rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(gp), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    const char* name = SDL_GetGamepadName(gp);
    hw_log("hid: controller connected: %s (%s, vid %04x pid %04x%s%s) -> player %d", name ? name : "?",
           pad_type_name(SDL_GetGamepadType(gp)), SDL_GetGamepadVendor(gp), SDL_GetGamepadProduct(gp),
           p.gyro && p.accel ? ", gyro" : "", p.rumble ? ", rumble" : "", p.slot + 1);
    g_pads.push_back(p);
}

void sdl_close(SDL_JoystickID id) {
    for (size_t i = 0; i < g_pads.size(); i++) {
        if (g_pads[i].id != id) continue;
        const char* name = SDL_GetGamepadName(g_pads[i].gp);
        hw_log("hid: controller disconnected: %s (player %d)", name ? name : "?", g_pads[i].slot + 1);
        SDL_RumbleGamepad(g_pads[i].gp, 0, 0, 0);
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

// Reads one pad's state into `raw`.
void sdl_read(SdlPad& p, u32 buttons, RawPad& raw) {
    SDL_Gamepad* gp = p.gp;
    raw.connected = true;
    raw.sdl = true;
    raw.buttons = buttons;
    raw.lx = clamp16(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
    raw.ly = clamp16(-(int)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));  // SDL: +y = down
    raw.rx = clamp16(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
    raw.ry = clamp16(-(int)SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
    raw.lt = std::max<s16>(0, SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    raw.rt = std::max<s16>(0, SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    raw.type = SDL_GetGamepadType(gp);
    raw.family = family_of(raw.type);
    raw.has_gyro = p.gyro && p.accel;
    raw.has_rumble = p.rumble;
    const char* name = SDL_GetGamepadName(gp);
    snprintf(raw.name, sizeof(raw.name), "%s", name ? name : "Controller");
}

// Pumps SDL (hot-plug included), fills each player slot's `raws` entry from its pad and `menu` from the most
// recently used pad. `motion` gets player 1's six-axis state in the Switch convention. Returns false while no
// pad is open.
bool sdl_poll(bool want_sensors, RawPad raws[kPlayerSlots], RawPad& menu, MotionInput& motion) {
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
        if (p.slot >= 0) sdl_read(p, b, raws[p.slot]);
        if (!best || p.last_used > best->last_used) {
            best = &p;
            best_buttons = b;
        }
    }
    if (!best) return false;
    sdl_read(*best, best_buttons, menu);

    SdlPad* first = nullptr;  // player 1's pad drives the six-axis sensor
    for (SdlPad& p : g_pads)
        if (p.slot == 0) first = &p;
    SDL_Gamepad* gp = first ? first->gp : nullptr;
    float a[3], g[3];
    if (want_sensors && first && first->gyro && first->accel && SDL_GetGamepadSensorData(gp, SDL_SENSOR_ACCEL, a, 3) &&
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
    return true;
}

// ------------------------------------------------------------------ rumble
// Each slot's pad gets the amplitude the game sent for its npad. The preview pulse goes to every pad.
void apply_rumble(bool sdl_pads) {
    const settings::Input& cfg = settings::input();
    const float k = cfg.rumble ? cfg.rumble_strength / 100.0f : 0.0f;
    const u64 now = GetTickCount64();
    const bool preview = now < g_preview_until.load();
    u16 lo[kPlayerSlots], hi[kPlayerSlots];
    for (int s = 0; s < kPlayerSlots; s++) {
        lo[s] = (u16)((preview ? 1.0f : std::clamp(g_rumble_low[s].load(), 0.0f, 1.0f)) * k * 65535.0f);
        hi[s] = (u16)((preview ? 1.0f : std::clamp(g_rumble_high[s].load(), 0.0f, 1.0f)) * k * 65535.0f);
    }
    if (sdl_pads) {
        for (SdlPad& p : g_pads) {
            if (p.slot < 0 || !p.rumble) continue;
            const u16 l = lo[p.slot], h = hi[p.slot];
            // SDL rumble runs for a set time: refresh it while the game keeps it on.
            // While the slider drags, send a new preview strength at most every 50 ms.
            if (preview && now - p.rumble_at < 50) continue;
            if (l != p.rumble_sent[0] || h != p.rumble_sent[1] || ((l || h) && now - p.rumble_at > 1000)) {
                SDL_RumbleGamepad(p.gp, l, h, (l || h) ? 2000 : 0);
                p.rumble_sent[0] = l;
                p.rumble_sent[1] = h;
                p.rumble_at = now;
            }
        }
        return;
    }
    // No SDL pad open: the XInput fallback pads (poll_host_input).
    if (!g_set_state) return;
    for (int s = 0; s < kPlayerSlots; s++) {
        if (g_xi_user[s] < 0 || (lo[s] == g_xi_sent[s][0] && hi[s] == g_xi_sent[s][1])) continue;
        XINPUT_VIBRATION v;
        v.wLeftMotorSpeed = lo[s];
        v.wRightMotorSpeed = hi[s];
        g_set_state((DWORD)g_xi_user[s], &v);
        g_xi_sent[s][0] = lo[s];
        g_xi_sent[s][1] = hi[s];
    }
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
// Keyboard keys to Switch input, merged into one slot's pad.
void poll_keyboard(PadInput& s) {
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

// The slot the keyboard plays in (Settings > Input > Keyboard player), -1 for none.
int keyboard_slot(const RawPad raws[kPlayerSlots]) {
    const int want = settings::input().keyboard_slot;
    if (want < 0) return -1;
    if (want >= 1 && want <= kPlayerSlots) return want - 1;
    for (int s = 0; s < kPlayerSlots; s++)
        if (!raws[s].connected) return s;
    return -1;
}

void poll_host_input(HostInput& out) {
    out = {};
    const settings::Input& cfg = settings::input();

    if (!g_sdl_tried) {
        g_sdl_tried = true;
        g_sdl_ok = sdl_init();
    }
    DWORD fg_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
    const bool focused = fg_pid == GetCurrentProcessId();

    RawPad raws[kPlayerSlots];
    RawPad menu;
    bool sdl_pads = false;
    // With no SDL pad open, XInput too: SDL can miss a pad paired while the game runs (no hot-plug event
    // arrives), while XInput sees an Xbox pad as soon as Windows connects it.
    if (g_sdl_ok) sdl_pads = sdl_poll(cfg.motion, raws, menu, out.motion);
    if (!sdl_pads) {
        poll_xinput(raws);
        menu = raws[0];
    }
    apply_rumble(sdl_pads);
    if (!focused && !cfg.background_pad) {
        // Keep the pads' identity for the menu, drop their input.
        auto quiet = [](RawPad& r) {
            r.buttons = 0;
            r.lx = r.ly = r.rx = r.ry = r.lt = r.rt = 0;
        };
        quiet(menu);
        for (RawPad& r : raws) quiet(r);
        out.motion = {};
    }
    const int kb = keyboard_slot(raws);
    u32 mask = 0;
    {
        std::lock_guard<std::mutex> l(g_raw_m);
        g_raw = menu;
        for (int s = 0; s < kPlayerSlots; s++) {
            if (raws[s].connected) snprintf(g_slot_names[s], sizeof(g_slot_names[s]), "%s", raws[s].name);
            else if (s == kb) snprintf(g_slot_names[s], sizeof(g_slot_names[s]), "Keyboard");
            else g_slot_names[s][0] = 0;
        }
    }

    for (int slot = 0; slot < kPlayerSlots; slot++) {
        const RawPad& raw = raws[slot];
        PadInput& s = out.pads[slot];
        out.connected[slot] = raw.connected || slot == kb;
        if (out.connected[slot]) mask |= 1u << slot;

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
        update_view_button(slot, (raw.buttons >> kHostBack) & 1, tap_bits, s.buttons);

        float lx, ly, rx, ry;
        deadzone(raw.lx, raw.ly, cfg.deadzone_l, lx, ly);
        deadzone(raw.rx, raw.ry, cfg.deadzone_r, rx, ry);
        s.lx = to_axis(cfg.invert_lx ? -lx : lx);
        s.ly = to_axis(cfg.invert_ly ? -ly : ly);
        s.rx = to_axis(cfg.invert_rx ? -rx : rx);
        s.ry = to_axis(cfg.invert_ry ? -ry : ry);

        if (focused && slot == kb) poll_keyboard(s);

        const s32 t = kStickMax / 2;
        if (s.lx < -t) s.buttons |= kBtnStickLLeft;
        if (s.lx > t) s.buttons |= kBtnStickLRight;
        if (s.ly > t) s.buttons |= kBtnStickLUp;
        if (s.ly < -t) s.buttons |= kBtnStickLDown;
        if (s.rx < -t) s.buttons |= kBtnStickRLeft;
        if (s.rx > t) s.buttons |= kBtnStickRRight;
        if (s.ry > t) s.buttons |= kBtnStickRUp;
        if (s.ry < -t) s.buttons |= kBtnStickRDown;
    }
    if (!cfg.motion) out.motion = {};
    g_connected_mask = mask;

    // The in-game menu blocks every player's input, then waits for all buttons to be released.
    bool any_button = false;
    for (const PadInput& p : out.pads) any_button |= p.buttons != 0;
    auto silence = [&] {
        for (PadInput& p : out.pads) p = {};
        out.motion = {};
        out.touch = {};
    };
    if (g_blocked) {
        silence();
        g_wait_release = true;
    } else if (g_wait_release) {
        if (any_button) silence();
        else g_wait_release = false;
    }
}

RawPad raw_pad() {
    std::lock_guard<std::mutex> l(g_raw_m);
    return g_raw;
}

void set_game_input_blocked(bool blocked) { g_blocked = blocked; }
bool take_menu_request() { return g_menu_request.exchange(false); }

void preview_rumble() { g_preview_until = GetTickCount64() + 250; }

void set_rumble(int slot, float low, float high) {
    if (slot < 0 || slot >= kPlayerSlots) return;
    g_rumble_low[slot] = low;
    g_rumble_high[slot] = high;
}

u32 connected_slots() { return g_connected_mask.load(); }

void slot_name(int slot, char* out, size_t n) {
    if (!n) return;
    out[0] = 0;
    if (slot < 0 || slot >= kPlayerSlots) return;
    std::lock_guard<std::mutex> l(g_raw_m);
    snprintf(out, n, "%s", g_slot_names[slot]);
}

}  // namespace hid
