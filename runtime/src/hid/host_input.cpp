// Host input: XInput (loaded dynamically) + keyboard fallback while the HWDER window has focus.
#include "host_input.h"

#include "gpu/settings.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>

namespace hid {

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XInputGetStateFn g_get_state = nullptr;
XInputSetStateFn g_set_state = nullptr;
bool g_loaded = false;
int g_active_pad = -1;
u32 g_probe_counter = 0;
bool g_pad_present[4] = {true, true, true, true};
std::atomic<bool> g_blocked{false};
std::atomic<bool> g_menu_request{false};
bool g_wait_release = false;

// View/Select: a tap is the game's - button, a hold opens the in-game menu. The game only sees the
// tap once the button is released, held for long enough that a 60 Hz game frame samples it.
constexpr u64 kMenuHoldMs = 500, kMinusTapMs = 60;
u64 g_view_down_at = 0;
bool g_view_held = false;
u64 g_minus_until = 0;

void update_view_button(bool down, u64& buttons) {
    if (!settings::input().hold_view_menu) {
        if (down) buttons |= kBtnMinus;
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
    if (now < g_minus_until) buttons |= kBtnMinus;
}

void load_xinput() {
    if (g_loaded) return;
    g_loaded = true;
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

bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

s32 axis(SHORT v, SHORT dead) {
    if (v > -dead && v < dead) return 0;
    return std::clamp((s32)v, -kStickMax, kStickMax);
}

}  // namespace

void poll_host_input(HostInput& out) {
    load_xinput();
    PadInput& s = out.pad;
    s = {};
    out.touch = {};

    DWORD fg_pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
    const bool focused = fg_pid == GetCurrentProcessId();
    bool view = false;

    if (g_get_state && (focused || settings::input().background_pad)) {
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
            // Positional mapping: Xbox bottom/right/left/top = Switch B/A/Y/X.
            if (g.wButtons & XINPUT_GAMEPAD_A) s.buttons |= kBtnB;
            if (g.wButtons & XINPUT_GAMEPAD_B) s.buttons |= kBtnA;
            if (g.wButtons & XINPUT_GAMEPAD_X) s.buttons |= kBtnY;
            if (g.wButtons & XINPUT_GAMEPAD_Y) s.buttons |= kBtnX;
            if (g.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) s.buttons |= kBtnL;
            if (g.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) s.buttons |= kBtnR;
            if (g.bLeftTrigger > 64) s.buttons |= kBtnZL;
            if (g.bRightTrigger > 64) s.buttons |= kBtnZR;
            if (g.wButtons & XINPUT_GAMEPAD_START) s.buttons |= kBtnPlus;
            view = (g.wButtons & XINPUT_GAMEPAD_BACK) != 0;
            if (g.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) s.buttons |= kBtnStickL;
            if (g.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) s.buttons |= kBtnStickR;
            if (g.wButtons & XINPUT_GAMEPAD_DPAD_UP) s.buttons |= kBtnUp;
            if (g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) s.buttons |= kBtnDown;
            if (g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) s.buttons |= kBtnLeft;
            if (g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) s.buttons |= kBtnRight;
            s.lx = axis(g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            s.ly = axis(g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            s.rx = axis(g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            s.ry = axis(g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        }
    }

    update_view_button(view, s.buttons);

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

void set_game_input_blocked(bool blocked) { g_blocked = blocked; }
bool take_menu_request() { return g_menu_request.exchange(false); }

void set_rumble(float low, float high) {
    load_xinput();
    if (!g_set_state || g_active_pad < 0) return;
    if (!settings::input().rumble) low = high = 0.0f;
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = (WORD)(std::clamp(low, 0.0f, 1.0f) * 65535.0f);
    v.wRightMotorSpeed = (WORD)(std::clamp(high, 0.0f, 1.0f) * 65535.0f);
    g_set_state((DWORD)g_active_pad, &v);
}

}  // namespace hid
