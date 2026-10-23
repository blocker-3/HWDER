#include "gpu/settings.h"

#include "hid/host_input.h"

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
static Launch g_launch;
static const char* kFile = "hwder_settings.ini";
static const int kVersion = 3;  // bump when a default changes meaning; load() migrates older files (v3: input remaps)
static const char* kFamilyKeys[kPadFamilies] = {"remap_xbox", "remap_playstation", "remap_nintendo", "remap_generic"};

Input::Input() {
    for (int f = 0; f < kPadFamilies; f++) hid::default_remap(remap[f], f, face_labels);
}

// "1,0,3,..." with exactly kRemapSlots valid host buttons (-1 = unbound); anything else keeps the defaults.
static void parse_remap(const char* v, int* out) {
    int tmp[kRemapSlots];
    int n = 0;
    const char* p = v;
    while (*p && *p != '\r' && *p != '\n') {
        char* end;
        const long b = strtol(p, &end, 10);
        if (end == p || n == kRemapSlots || b < -1 || b >= hid::kHostButtonCount) return;
        tmp[n++] = (int)b;
        if (*end == ',') p = end + 1;
        else if (!*end || *end == '\r' || *end == '\n') p = end;
        else return;
    }
    if (n == kRemapSlots) memcpy(out, tmp, sizeof(tmp));
}

Video& video() { return g_video; }
Audio& audio() { return g_audio; }
Input& input() { return g_input; }
Debug& debug() { return g_debug; }
Launch& launch() { return g_launch; }

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
    static bool loaded = false;  // main() loads early for the startup screen; display::init loads again
    if (loaded) return;
    loaded = true;
    FILE* f = fopen(kFile, "r");
    if (!f) return;
    // By printed label is the first-run default only: a file written before it (no face_labels line) keeps
    // the old by-position layout.
    g_input.face_labels = false;
    for (int fam = 0; fam < kPadFamilies; fam++) hid::default_remap(g_input.remap[fam], fam, false);
    int version = 1;  // files without a version line predate versioning
    char line[2048];  // long enough for the file paths
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
        else if (k == "ultrawide") g_video.ultrawide = atoi(v) != 0;
        else if (k == "fps120") g_video.fps120.store(atoi(v) != 0, std::memory_order_relaxed);
        else if (k == "show_fps") g_video.show_fps = atoi(v) != 0;
        else if (k == "display_60hz") g_video.display_60hz = atoi(v) != 0;
        else if (k == "volume") g_audio.volume = std::clamp(atoi(v), 0, 100);
        else if (k == "mute") g_audio.mute = atoi(v) != 0;
        else if (k == "mute_background") g_audio.mute_background = atoi(v) != 0;
        else if (k == "hold_view_menu") g_input.hold_view_menu = atoi(v) != 0;
        else if (k == "rumble") g_input.rumble = atoi(v) != 0;
        else if (k == "background_pad") g_input.background_pad = atoi(v) != 0;
        else if (k == "rumble_strength") g_input.rumble_strength = std::clamp(atoi(v), 0, 100);
        else if (k == "face_labels") g_input.face_labels = atoi(v) != 0;
        else if (k == "deadzone_l") g_input.deadzone_l = std::clamp(atoi(v), 0, 90);
        else if (k == "deadzone_r") g_input.deadzone_r = std::clamp(atoi(v), 0, 90);
        else if (k == "invert_lx") g_input.invert_lx = atoi(v) != 0;
        else if (k == "invert_ly") g_input.invert_ly = atoi(v) != 0;
        else if (k == "invert_rx") g_input.invert_rx = atoi(v) != 0;
        else if (k == "invert_ry") g_input.invert_ry = atoi(v) != 0;
        else if (k == "motion") g_input.motion = atoi(v) != 0;
        else if (k == "keyboard_slot") g_input.keyboard_slot = std::clamp(atoi(v), -1, 4);
        else if (k.rfind("remap_", 0) == 0) {
            for (int fam = 0; fam < kPadFamilies; fam++)
                if (k == kFamilyKeys[fam]) parse_remap(v, g_input.remap[fam]);
        }
        else if (k == "menu_scale") g_video.menu_scale = std::clamp(atoi(v), 50, 300);
        else if (k == "contrast") g_video.contrast = std::clamp(atoi(v), 50, 150);
        else if (k == "bloom") g_video.bloom = std::clamp(atoi(v), 0, 200);
        // Custom FOV is hidden until officer nameplates use the same projection.
        // Ignore old overrides so a hidden setting cannot leave labels misplaced.
        else if (k == "fov") g_video.fov = 100;
        else if (k == "fxaa") g_video.fxaa = atoi(v) != 0;
        else if (k == "sharpen") g_video.sharpen = std::clamp(atoi(v), 0, 100);
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
        else if (k == "nsp_path" || k == "keys_path" || k == "save_dir" || k == "save_copy_from") {
            std::string path = v;  // UTF-8, as written by save()
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
            (k == "nsp_path"    ? g_launch.nsp_path
             : k == "keys_path" ? g_launch.keys_path
             : k == "save_dir"  ? g_launch.save_dir
                                : g_launch.save_copy_from) = path;
        }
        else if (k == "skip_prelaunch") g_launch.skip_prelaunch = atoi(v) != 0;
        // unknown keys (e.g. v1's unused "refresh") are ignored and dropped on the next save
    }
    fclose(f);
    if (version < 2) {
        // v1 auto-enabled the 60 Hz monitor switch on non-60-multiple panels, so a saved 1 was most likely that
        // default rather than a choice. It is opt-in now.
        g_video.display_60hz = false;
        save();
    }
    apply_env();
}

void save() {
    FILE* f = fopen(kFile, "w");
    if (!f) return;
    fprintf(f, "version=%d\nvsync=%d\naniso=%d\nwindow_w=%d\nwindow_h=%d\nborderless=%d\nres_scale=%.3f\nunlocked_res=%d\nshow_fps=%d\ndisplay_60hz=%d\nultrawide=%d\n",
            kVersion, (int)g_video.vsync, g_video.aniso, g_video.window_w, g_video.window_h, (int)g_video.borderless,
            g_video.res_scale, (int)g_video.unlocked_res, (int)g_video.show_fps, (int)g_video.display_60hz, (int)g_video.ultrawide);
    fprintf(f, "volume=%d\nmute=%d\nmute_background=%d\nhold_view_menu=%d\nrumble=%d\nbackground_pad=%d\nmenu_scale=%d\nasync_shaders=%d\n",
            g_audio.volume, (int)g_audio.mute, (int)g_audio.mute_background, (int)g_input.hold_view_menu, (int)g_input.rumble,
            (int)g_input.background_pad, g_video.menu_scale, (int)g_debug.async_shaders);
    fprintf(f, "contrast=%d\nbloom=%d\nfov=%d\nfxaa=%d\nsharpen=%d\nfps120=%d\n", g_video.contrast, g_video.bloom,
            g_video.fov, (int)g_video.fxaa, g_video.sharpen, (int)g_video.fps120.load(std::memory_order_relaxed));
    fprintf(f, "rumble_strength=%d\nface_labels=%d\ndeadzone_l=%d\ndeadzone_r=%d\ninvert_lx=%d\ninvert_ly=%d\ninvert_rx=%d\ninvert_ry=%d\n"
               "motion=%d\n",
            g_input.rumble_strength, (int)g_input.face_labels, g_input.deadzone_l, g_input.deadzone_r, (int)g_input.invert_lx,
            (int)g_input.invert_ly, (int)g_input.invert_rx, (int)g_input.invert_ry, (int)g_input.motion);
    for (int fam = 0; fam < kPadFamilies; fam++) {
        fprintf(f, "%s=", kFamilyKeys[fam]);
        for (int i = 0; i < kRemapSlots; i++) fprintf(f, i ? ",%d" : "%d", g_input.remap[fam][i]);
        fputc('\n', f);
    }
    std::string env;
    for (const std::string& name : g_debug.env) env += (env.empty() ? "" : ",") + name;
    fprintf(f, "debug_env=%s\n", env.c_str());
    fprintf(f, "nsp_path=%s\nkeys_path=%s\nskip_prelaunch=%d\n", g_launch.nsp_path.c_str(), g_launch.keys_path.c_str(),
            (int)g_launch.skip_prelaunch);
    fprintf(f, "save_dir=%s\n", g_launch.save_dir.c_str());
    if (!g_launch.save_copy_from.empty()) fprintf(f, "save_copy_from=%s\n", g_launch.save_copy_from.c_str());
    fclose(f);
}

}  // namespace settings
