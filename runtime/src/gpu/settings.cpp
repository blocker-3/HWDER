#include "gpu/settings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace settings {

static Video g_video;
static const char* kFile = "hwder_settings.ini";
static const int kVersion = 2;  // bump when a default changes meaning; load() migrates older files

Video& video() { return g_video; }

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
}

void save() {
    FILE* f = fopen(kFile, "w");
    if (!f) return;
    fprintf(f, "version=%d\nvsync=%d\naniso=%d\nwindow_w=%d\nwindow_h=%d\nborderless=%d\nres_scale=%.3f\nunlocked_res=%d\nshow_fps=%d\ndisplay_60hz=%d\ninterp_hz=%d\n",
            kVersion, (int)g_video.vsync, g_video.aniso, g_video.window_w, g_video.window_h, (int)g_video.borderless,
            g_video.res_scale, (int)g_video.unlocked_res, (int)g_video.show_fps, (int)g_video.display_60hz, g_video.interp_hz);
    fclose(f);
}

}  // namespace settings
