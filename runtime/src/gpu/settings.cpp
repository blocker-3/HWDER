#include "gpu/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace settings {

static Video g_video;
static Audio g_audio;
static Input g_input;
static Debug g_debug;
static const char* kFile = "hwder_settings.ini";
static const int kVersion = 2;  // bump when a default changes meaning; load() migrates older files

Video& video() { return g_video; }
Audio& audio() { return g_audio; }
Input& input() { return g_input; }
Debug& debug() { return g_debug; }

// Startup switches become environment variables before anything reads them (settings load first in
// display::init, ahead of Vulkan, the services and the game). Variables set outside the game win.
static void apply_env() {
    auto set = [](const char* name) {
        if (!getenv(name)) _putenv_s(name, "1");
    };
    if (!g_debug.async_shaders) set("HWDER_SYNC_PIPELINES");
    for (const std::string& name : g_debug.env) set(name.c_str());
}

void load() {
    FILE* f = fopen(kFile, "r");
    if (!f) return;
    int version = 1;  // files without a version line predate versioning
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        std::string k = line;
        const char* v = eq + 1;
        if (k == "version") version = atoi(v);
        else if (k == "vsync") g_video.vsync = atoi(v) != 0;
        else if (k == "aniso") g_video.aniso = atoi(v);
        else if (k == "window_w") g_video.window_w = atoi(v);
        else if (k == "window_h") g_video.window_h = atoi(v);
        else if (k == "borderless") g_video.borderless = atoi(v) != 0;
        else if (k == "res_scale") g_video.res_scale = (float)atof(v);
        else if (k == "unlocked_res") g_video.unlocked_res = atoi(v) != 0;
        else if (k == "show_fps") g_video.show_fps = atoi(v) != 0;
        else if (k == "display_60hz") g_video.display_60hz = atoi(v) != 0;
        else if (k == "volume") g_audio.volume = std::clamp(atoi(v), 0, 100);
        else if (k == "mute") g_audio.mute = atoi(v) != 0;
        else if (k == "mute_background") g_audio.mute_background = atoi(v) != 0;
        else if (k == "hold_view_menu") g_input.hold_view_menu = atoi(v) != 0;
        else if (k == "rumble") g_input.rumble = atoi(v) != 0;
        else if (k == "background_pad") g_input.background_pad = atoi(v) != 0;
        else if (k == "menu_scale") g_video.menu_scale = std::clamp(atoi(v), 50, 300);
        else if (k == "async_shaders") g_debug.async_shaders = atoi(v) != 0;
        else if (k == "debug_env") {
            // Comma-separated HWDER_* names; anything else is ignored.
            std::string list = v;
            size_t i = 0;
            while (i < list.size()) {
                size_t j = list.find_first_of(",\r\n", i);
                if (j == std::string::npos) j = list.size();
                std::string name = list.substr(i, j - i);
                if (name.rfind("HWDER_", 0) == 0) g_debug.env.insert(name);
                i = j + 1;
            }
        }
        else if (k == "interp_hz") g_video.interp_hz = atoi(v);
        // unknown keys (e.g. v1's unused "refresh") are ignored and dropped on the next save
    }
    fclose(f);
    if (version < 2) {
        // v1 auto-enabled the 60 Hz monitor switch on non-60-multiple panels, so a saved 1 was most likely that
        // default rather than a choice. Interpolation defaulted on in some test builds. Both are opt-in now.
        g_video.display_60hz = false;
        g_video.interp_hz = 0;
        save();
    }
    apply_env();
}

void save() {
    FILE* f = fopen(kFile, "w");
    if (!f) return;
    fprintf(f, "version=%d\nvsync=%d\naniso=%d\nwindow_w=%d\nwindow_h=%d\nborderless=%d\nres_scale=%.3f\nunlocked_res=%d\nshow_fps=%d\ndisplay_60hz=%d\ninterp_hz=%d\n",
            kVersion, (int)g_video.vsync, g_video.aniso, g_video.window_w, g_video.window_h, (int)g_video.borderless,
            g_video.res_scale, (int)g_video.unlocked_res, (int)g_video.show_fps, (int)g_video.display_60hz, g_video.interp_hz);
    fprintf(f, "volume=%d\nmute=%d\nmute_background=%d\nhold_view_menu=%d\nrumble=%d\nbackground_pad=%d\nmenu_scale=%d\nasync_shaders=%d\n",
            g_audio.volume, (int)g_audio.mute, (int)g_audio.mute_background, (int)g_input.hold_view_menu, (int)g_input.rumble,
            (int)g_input.background_pad, g_video.menu_scale, (int)g_debug.async_shaders);
    std::string env;
    for (const std::string& name : g_debug.env) env += (env.empty() ? "" : ",") + name;
    fprintf(f, "debug_env=%s\n", env.c_str());
    fclose(f);
}

}  // namespace settings
