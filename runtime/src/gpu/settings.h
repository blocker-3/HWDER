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

struct Input {
    bool hold_view_menu = true;  // holding View/Select opens the menu; a tap is the game's - button
    bool rumble = true;
    bool background_pad = true;  // read the controller while another window has focus
};

// Startup screen: files picked there and whether it shows when everything checks out.
struct Launch {
    std::string nsp_path;   // game file; empty = search beside the exe (HWDER_NSP wins)
    std::string keys_path;  // prod.keys; empty = search the usual places (HWDER_KEYS wins)
    bool skip_prelaunch = false;
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
Launch& launch();
void load();  // once; later calls do nothing
void save();

}  // namespace settings
