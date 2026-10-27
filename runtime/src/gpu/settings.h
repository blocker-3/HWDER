// User-facing settings (edited in the in-game menu, persisted in hwder_settings.ini next to the exe).
#pragma once

#include <atomic>
#include <set>
#include <string>

namespace settings {

// Threading rule for every settings struct: the UI thread (the in-game menu and the .ini load at
// startup) writes the fields; the guest and GPU threads only read them, through video() and friends,
// and pick a change up whenever they next read the field. Only fps120 is atomic: the game thread
// latches it once per tick (BeginFrame) while the UI may flip it at any moment, and the pair logic
// needs one consistent value per tick. Keep new fields under the same rule.
struct Video {
    bool vsync = false;         // FIFO present mode when true, MAILBOX otherwise
    int aniso = 0;              // anisotropic filtering override: 0 = game default, 2/4/8/16
    int window_w = 0;           // requested client size, 0 = leave the window alone
    int window_h = 0;
    bool borderless = false;    // borderless fullscreen on the current monitor
    float res_scale = 1.0f;     // internal resolution scale for scene render targets (1.0 = 1920x1080)
    bool unlocked_res = false;  // render at the window's own size / aspect; overrides res_scale
    bool ultrawide = false;     // with unlocked_res on a wider-than-16:9 window: widen the game's camera to fill it
                                // (true 21:9) instead of pillarboxing; the HUD stays a centred 16:9
    bool show_fps = false;      // small frame-time readout in the corner
    std::atomic<bool> fps120{false};  // UI request, sampled at BeginFrame; read by guest/GPU threads
    bool display_60hz = false;  // opt-in: switch the monitor to 60 Hz while the game runs (even cadence for 60/30 fps content)
    int menu_scale = 100;       // in-game menu text size, percent
    int contrast = 100;         // picture contrast in the game's final tonemap pass, percent (100 = unchanged)
    int bloom = 100;            // bloom strength, percent of the game's own (0 = off)
    int fov = 100;              // held at the game's default while officer-nameplate alignment is unresolved
    bool fxaa = false;          // FXAA on the 3D picture before the HUD
    int sharpen = 0;            // contrast-adaptive sharpening of the 3D picture, percent (0 = off)
    bool keep_crowd_props = true;  // keep small stage props (fences, rubble) drawn when crowds are large (game/stage_props.cpp)
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
    bool face_labels = true;     // face buttons by printed label (Xbox A = Switch A) instead of by position
    int deadzone_l = 24;         // radial stick deadzones, percent (XInput's defaults)
    int deadzone_r = 27;
    bool invert_lx = false, invert_ly = false, invert_rx = false, invert_ry = false;
    bool motion = true;          // pass the controller's gyro and accelerometer to the game
    int keyboard_slot = 0;       // 0 = lowest slot without a controller, 1..4 = that player, -1 = off
    int remap[kPadFamilies][kRemapSlots];  // host button (hid::HostButton) per Switch button, -1 = unbound
    Input();
};

// Startup screen: files picked there, whether it shows when everything checks out, and the save folder.
struct Launch {
    std::string nsp_path;   // game file; empty = search beside the exe (HWDER_NSP wins)
    std::string keys_path;  // prod.keys; empty = search the usual places (HWDER_KEYS wins)
    std::string update_path;  // the game update unpacked by Setup; empty = base game only (HWDER_UPDATE wins)
    bool skip_prelaunch = false;
    std::string save_dir;        // save folder; empty = <exe dir>\save (see fs/save_dir.h)
    std::string save_copy_from;  // copy saves from this folder into save_dir at the next launch (set in the in-game menu)
    int game_memory_mb = 0;          // memory the game is told it has (TotalMemorySize); 0 = a Switch's 3285 MiB
    int game_memory_active_mb = 0;   // what this run told the game (set by the kernel at boot; not saved)
    std::string mods_dir;            // mods folder; empty = the default (fs/mods.h, HWDER_MODS wins)
    std::set<std::string> mods_off;  // mod folder names switched off in the menu (saved '/'-separated)
};

// Startup switches. Each is applied as its environment variable before the subsystems read it, so a
// change takes effect on the next launch. A variable set outside the game always wins.
struct Debug {
    bool async_shaders = true;    // false = HWDER_SYNC_PIPELINES
    std::set<std::string> env;    // HWDER_* debug switches set to 1 at startup
};

Video& video();
Audio& audio();
Input& input();
Debug& debug();
Launch& launch();
void load();  // once; later calls do nothing
void save();

}  // namespace settings
