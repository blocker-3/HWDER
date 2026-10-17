// User-facing settings (edited in the in-game menu, persisted in hwder_settings.ini next to the exe).
#pragma once

#include <set>
#include <string>

namespace settings {

struct Video {
    bool vsync = false;         // FIFO present mode when true, MAILBOX otherwise
    int aniso = 0;              // anisotropic filtering override: 0 = game default, 2/4/8/16
    int window_w = 0;           // requested client size, 0 = leave the window alone
    int window_h = 0;
    bool borderless = false;    // borderless fullscreen on the current monitor
    float res_scale = 1.0f;     // internal resolution scale for scene render targets (1.0 = 1920x1080)
    bool unlocked_res = false;  // render at the window's own size / aspect; overrides res_scale
    bool show_fps = false;      // small frame-time readout in the corner
    int interp_hz = 0;          // frame interpolation output rate: 0 off, -1 match display, else Hz (ignored while kInterpEnabled is false)
    bool display_60hz = false;  // opt-in: switch the monitor to 60 Hz while the game runs (even cadence for 60/30 fps content)
    int menu_scale = 100;       // in-game menu text size, percent
};

struct Audio {
    int volume = 100;              // master output volume, 0..100
    bool mute = false;
    bool mute_background = false;  // silent while another window has focus
};

// Controller families with their own button remap (Settings > Input). hid::pad_family() sorts pads into them.
enum PadFamily { kPadXbox, kPadPlayStation, kPadNintendo, kPadGeneric, kPadFamilies };
constexpr int kRemapSlots = 16;  // Switch buttons in the remap table, see hid::switch_button_name()

struct Input {
    bool hold_view_menu = true;  // holding View/Select opens the menu; a tap is the game's - button
    bool rumble = true;
    int rumble_strength = 100;   // percent
    bool background_pad = true;  // read the controller while another window has focus
    bool face_labels = false;    // face buttons by printed label (Xbox A = Switch A) instead of by position
    int deadzone_l = 24;         // radial stick deadzones, percent (XInput's defaults)
    int deadzone_r = 27;
    bool invert_lx = false, invert_ly = false, invert_rx = false, invert_ry = false;
    bool motion = true;          // pass the controller's gyro and accelerometer to the game
    bool gyro_camera = false;    // also turn the camera with the gyro (adds to the right stick)
    float gyro_sens = 1.0f;
    int remap[kPadFamilies][kRemapSlots];  // host button (hid::HostButton) per Switch button, -1 = unbound
    Input();
};

// Startup switches. Each is applied as its environment variable before the subsystems read it, so a
// change takes effect on the next launch. A variable set outside the game always wins.
struct Debug {
    bool async_shaders = true;    // false = HWDER_SYNC_PIPELINES
    std::set<std::string> env;    // HWDER_* debug switches set to 1 at startup
};

// False while frame interpolation is stubbed out: the presenter ignores interp_hz (the saved value is kept) and the
// overlay greys the option out. HWDER_INTERP=<hz> still enables it for experiments.
constexpr bool kInterpEnabled = false;

Video& video();
Audio& audio();
Input& input();
Debug& debug();
void load();
void save();

}  // namespace settings
