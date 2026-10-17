#include "gpu/overlay.h"

#include "gpu/settings.h"
#include "hid/host_input.h"
#include "hwder/runtime.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <string>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

float hwder_render_scale_x();  // texture_cache.cpp: active internal resolution scale
float hwder_render_scale_y();

namespace overlay {

// Recursive: ImGui's Win32 handler calls ReleaseCapture()/SetCursor(), which re-enter wnd_proc synchronously.
static std::recursive_mutex g_m;
static bool g_inited = false;
static bool g_visible = false;
static bool g_swapchain_dirty = false;
static HWND g_hwnd;
static VkDevice g_dev;
static VkInstance g_instance;
static VkFormat g_fmt;
static std::unordered_map<VkImage, VkImageView> g_views;
static double g_frame_ms[120];
static int g_frame_i = 0;
static LARGE_INTEGER g_last_t;
static bool g_close_armed = false;  // Escape / B must be released after opening before they close the menu
static bool g_focus_menu = false;   // give the menu keyboard/gamepad focus on its first frame
static int g_tab = 0;

static void set_visible(bool on);

static void check_vk(VkResult r) {
    if (r != VK_SUCCESS) hw_log("overlay: vulkan error %d", (int)r);
}

void init(HWND hwnd, VkInstance instance, VkPhysicalDevice phys, VkDevice dev, uint32_t queue_family, VkQueue queue,
          VkFormat swapchain_format) {
    std::lock_guard<std::recursive_mutex> l(g_m);
    g_hwnd = hwnd;
    g_dev = dev;
    g_instance = instance;
    g_fmt = swapchain_format;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window positions are not worth a file
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplVulkan_LoadFunctions(
        VK_API_VERSION_1_3,
        [](const char* name, void* ud) { return vkGetInstanceProcAddr((VkInstance)ud, name); }, instance);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = instance;
    ii.PhysicalDevice = phys;
    ii.Device = dev;
    ii.QueueFamily = queue_family;
    ii.Queue = queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 3;
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_fmt;
    ii.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        hw_log("overlay: ImGui Vulkan backend init failed - overlay disabled");
        return;
    }
    g_inited = true;
    if (getenv("HWDER_OVERLAY")) set_visible(true);
    QueryPerformanceCounter(&g_last_t);
    hw_log("overlay: ready (Escape, F1 or holding View opens the menu)");
}

bool wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_inited) return false;
    std::lock_guard<std::recursive_mutex> l(g_m);
    // ImGui only gets input while the menu is open: no NewFrame runs while it is closed, so anything
    // queued then (clicks, the Escape that opens it) would replay into the menu.
    if (!g_visible) return false;
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE && (lp & (1 << 30))) return true;  // held Escape: no auto-repeat close
    ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
    switch (msg) {
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MOUSEWHEEL: case WM_LBUTTONDBLCLK: case WM_CHAR: case WM_KEYUP:
        return true;
    case WM_KEYDOWN:
        return wp != VK_F1 && wp != VK_F11;
    }
    return false;
}

// Opening clears ImGui's input and blocks the game's; closing hands input back to the game.
static void set_visible(bool on) {
    if (on && !g_visible) {
        ImGuiIO& io = ImGui::GetIO();
        io.ClearEventsQueue();
        io.ClearInputKeys();
        g_close_armed = false;
        g_focus_menu = true;
    }
    g_visible = on;
    hid::set_game_input_blocked(on);
}

void toggle() {
    std::lock_guard<std::recursive_mutex> l(g_m);
    if (g_inited) set_visible(!g_visible);
}
bool visible() { return g_visible; }
bool swapchain_dirty() {
    bool d = g_swapchain_dirty;
    g_swapchain_dirty = false;
    return d;
}

void on_swapchain(const std::vector<VkImage>& images, VkFormat fmt) {
    std::lock_guard<std::recursive_mutex> l(g_m);
    if (!g_dev) return;  // swapchain created before the overlay exists: display re-registers it after init()
    for (auto& [img, view] : g_views) vkDestroyImageView(g_dev, view, nullptr);
    g_views.clear();
    g_fmt = fmt;
    for (VkImage img : images) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = fmt;
        ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(g_dev, &ci, nullptr, &v) == VK_SUCCESS) g_views[img] = v;
    }
}

static void post_window_change() {
    settings::Video& v = settings::video();
    PostMessageW(g_hwnd, kMsgBorderless, 0, v.borderless ? 1 : 0);
    if (!v.borderless && v.window_w && v.window_h) PostMessageW(g_hwnd, kMsgClientSize, 0, (LPARAM)((v.window_w << 16) | v.window_h));
}

// Save file helpers for the Gameplay and Tools tabs.
static std::string save_path() {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto& t : fs::directory_iterator("save", ec))
        for (auto& u : fs::directory_iterator(t.path(), ec)) {
            fs::path p = u.path() / "zmha.bin";
            if (fs::exists(p, ec)) return p.string();
        }
    return "";
}

static std::vector<unsigned char> read_all(const std::string& p) {
    std::vector<unsigned char> d;
    if (FILE* f = fopen(p.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        d.resize(n > 0 ? n : 0);
        if (n > 0) fread(d.data(), 1, n, f);
        fclose(f);
    }
    return d;
}

// Tooltip for the widget just drawn, shown on mouse hover and on keyboard/gamepad focus, plus a "(?)".
static void help(const char* text) {
    ImGui::SetItemTooltip("%s", text);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// Marker for settings read once at startup. Highlighted when the saved value differs from the running one.
static void restart_marker(bool pending) {
    ImGui::SameLine();
    if (pending) ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.25f, 1.0f), "restart required");
    else ImGui::TextDisabled("on restart");
}

static void tab_display(VkExtent2D extent, bool& changed) {
    settings::Video& v = settings::video();
    ImGui::SeparatorText("Frame rate");
    {
        // interp_hz: 0 off, -1 match the display, else a fixed output rate. Greyed out while the feature is stubbed.
        const int values[] = {0, -1, 120, 144};
        const char* items[] = {"Off (60 fps)", "Match display refresh", "120 Hz", "144 Hz"};
        int cur = 0;
        if (settings::kInterpEnabled)
            for (int i = 0; i < 4; i++)
                if (values[i] == v.interp_hz) cur = i;
        ImGui::BeginDisabled(!settings::kInterpEnabled);
        if (ImGui::Combo("Frame interpolation (Experimental)", &cur, items, 4)) {
            v.interp_hz = values[cur];
            changed = true;
        }
        ImGui::EndDisabled();
        help(settings::kInterpEnabled
                 ? "Experimental: re-renders each game frame with blended 3D positions for 120/144 Hz panels.\n"
                   "Expect flashing, ghosting and broken effects in menus and gameplay."
                 : "Experimental: re-renders each game frame with blended 3D positions for 120/144 Hz panels.\n"
                   "Expect flashing, ghosting and broken effects in menus and gameplay.\n"
                   "Disabled in this build; the game stays at its native 60 fps.");
    }
    if (ImGui::Checkbox("Switch monitor to 60 Hz while playing", &v.display_60hz)) {
        changed = true;
        PostMessageW(g_hwnd, kMsgDisplay60, 0, v.display_60hz ? 1 : 0);
    }
    help("The game renders at 60 fps and its movies at 30 fps. On a 144 Hz panel each frame is shown for\n"
         "2 or 3 (movies: 4 or 5) refreshes in turn, which looks like stutter. At 60 Hz every frame is\n"
         "shown exactly once (movies: twice). Reverts when the game exits.");
    if (ImGui::Checkbox("VSync (FIFO)", &v.vsync)) {
        changed = true;
        g_swapchain_dirty = true;
    }
    help("Off = mailbox presentation (lowest latency).\nOn = classic vsync; use it if you see tearing.");
    ImGui::TextWrapped("Game logic runs at a fixed 60 fps: the game advances one step per frame, so releasing frames faster "
                       "speeds the whole game up. Presentation pacing is locked to 60 Hz; true 120/144 fps needs a "
                       "game-side timestep change (on the roadmap).");

    ImGui::SeparatorText("Window");
    if (ImGui::Checkbox("Borderless fullscreen (F11)", &v.borderless)) {
        changed = true;
        post_window_change();
    }
    {
        static const int sizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160}};
        char cur[32];
        snprintf(cur, sizeof(cur), "%ux%u", extent.width, extent.height);
        ImGui::BeginDisabled(v.borderless);
        if (ImGui::BeginCombo("Window size", cur)) {
            for (auto& s : sizes) {
                char item[32];
                snprintf(item, sizeof(item), "%dx%d", s[0], s[1]);
                if (ImGui::Selectable(item, (int)extent.width == s[0] && (int)extent.height == s[1])) {
                    v.window_w = s[0];
                    v.window_h = s[1];
                    changed = true;
                    post_window_change();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
    }

    ImGui::SeparatorText("Overlay");
    if (ImGui::Checkbox("Show frame time", &v.show_fps)) changed = true;
    {
        static const int scales[] = {100, 125, 150, 200};
        char cur[16];
        snprintf(cur, sizeof(cur), "%d%%", v.menu_scale);
        if (ImGui::BeginCombo("Menu text size", cur)) {
            for (int s : scales) {
                char item[16];
                snprintf(item, sizeof(item), "%d%%", s);
                if (ImGui::Selectable(item, v.menu_scale == s)) {
                    v.menu_scale = s;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    }
}

static void tab_graphics(bool& changed) {
    settings::Video& v = settings::video();
    {
        static const int levels[] = {0, 2, 4, 8, 16};
        static const char* names[] = {"Game default", "2x anisotropic", "4x anisotropic", "8x anisotropic", "16x anisotropic"};
        int cur = 0;
        for (int i = 0; i < 5; i++)
            if (levels[i] == v.aniso) cur = i;
        if (ImGui::BeginCombo("Texture filtering", names[cur])) {
            for (int i = 0; i < 5; i++)
                if (ImGui::Selectable(names[i], cur == i)) {
                    v.aniso = levels[i];
                    changed = true;
                }
            ImGui::EndCombo();
        }
    }
    {
        static const float steps[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
        int cur = 2;
        for (int i = 0; i < 7; i++)
            if (fabsf(steps[i] - v.res_scale) < 0.01f) cur = i;
        char label[64];
        if (v.unlocked_res) snprintf(label, sizeof(label), "Overridden by Unlocked resolution");
        else snprintf(label, sizeof(label), "%.2fx  (%dx%d)", steps[cur], (int)(1920 * steps[cur]), (int)(1080 * steps[cur]));
        ImGui::BeginDisabled(v.unlocked_res);
        if (ImGui::BeginCombo("Internal resolution", label)) {
            for (int i = 0; i < 7; i++) {
                char item[64];
                snprintf(item, sizeof(item), "%.2fx  (%dx%d)", steps[i], (int)(1920 * steps[i]), (int)(1080 * steps[i]));
                if (ImGui::Selectable(item, cur == i)) {
                    v.res_scale = steps[i];
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::Checkbox("Unlocked resolution (render at the window's size)", &v.unlocked_res)) changed = true;
        help("Scene render targets are allocated at this scale (UI and small post-process targets stay native).\n"
             "Unlocked = the window's own size and aspect (ultrawide fills the screen); it overrides the scale above.\n"
             "On a 1920x1080 window that equals 1.00x, i.e. the game's native resolution. Applies immediately.");
        {   // the scale actually in effect (also reflects HWDER_RES_SCALE* overrides and the 0.5x-4x clamp)
            float sx = ::hwder_render_scale_x(), sy = ::hwder_render_scale_y();
            int rw = (int)(1920 * sx + 0.5f), rh = (int)(1080 * sy + 0.5f);
            ImGui::TextDisabled("Rendering 3D at %dx%d (%.2fx x %.2fx)%s", rw, rh, sx, sy,
                                v.unlocked_res ? "  [unlocked: follows the window]" : "");
            float cost = sx * sy;  // pixel count relative to native 1920x1080
            if ((double)rw * rh > 3840.0 * 2160.0 * 1.01)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%dx%d: about %.0fx the native pixel count, expect a large fps drop",
                                   rw, rh, cost);
        }
    }

    ImGui::SeparatorText("Shaders");
    settings::Debug& d = settings::debug();
    if (ImGui::Checkbox("Compile shaders in the background", &d.async_shaders)) changed = true;
    help("On: a new shader is compiled on a worker thread and its draws are skipped for the frames that takes\n"
         "(no hitch, an object may pop in late). Off: the frame waits for the compile (a short hitch, nothing\n"
         "missing). Light frames such as menus always compile synchronously. Same as HWDER_SYNC_PIPELINES.");
    restart_marker(d.async_shaders == (getenv("HWDER_SYNC_PIPELINES") != nullptr));
}

static void tab_audio(bool& changed) {
    settings::Audio& a = settings::audio();
    ImGui::BeginDisabled(a.mute);
    if (ImGui::SliderInt("Volume", &a.volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) changed = true;
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Mute", &a.mute)) changed = true;
    if (ImGui::Checkbox("Mute while another window is active", &a.mute_background)) changed = true;
    if (getenv("HWDER_MUTE")) ImGui::TextDisabled("HWDER_MUTE is set: audio output is off for this run.");
}

static void tab_input(bool& changed) {
    settings::Input& in = settings::input();
    if (ImGui::Checkbox("Hold View / Select to open this menu", &in.hold_view_menu)) changed = true;
    help("Hold for half a second to open or close the menu. A tap is still the game's - button; the game sees it\n"
         "when you let go. Off: View is the - button straight away and the menu opens with Escape or F1 only.");
    if (ImGui::Checkbox("Rumble", &in.rumble)) changed = true;
    if (ImGui::Checkbox("Read the controller while another window is active", &in.background_pad)) changed = true;
    help("Off: the game ignores the controller while it is in the background. The keyboard is always ignored then.");

    ImGui::SeparatorText("Controls");
    if (ImGui::BeginTable("##controls", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Switch");
        ImGui::TableSetupColumn("Xbox controller");
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableHeadersRow();
        static const char* rows[][3] = {
            {"A / B / X / Y", "B / A / Y / X (by position)", "K or Enter / J or Backspace / I / L"},
            {"L / R", "LB / RB", "Q / E"},
            {"ZL / ZR", "LT / RT", "U / O"},
            {"+ / -", "Menu / View (tap)", "Space / Tab"},
            {"Left stick", "Left stick", "W A S D"},
            {"Right stick", "Right stick", "Arrow keys"},
            {"D-pad", "D-pad", "1 2 3 4"},
            {"Stick clicks", "LS / RS", "Z / C"},
            {"HWDER menu", "Hold View", "Escape or F1"},
        };
        for (auto& r : rows) {
            ImGui::TableNextRow();
            for (int c = 0; c < 3; c++) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TextUnformatted(r[c]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("In this menu: D-pad or left stick to move, A to select, B to go back or close, LB / RB to switch tabs.");
}

static void tab_gameplay() {
    ImGui::SeparatorText("Cheats (save file)");
    static std::string status;
    std::string sp = save_path();
    ImGui::TextDisabled("%s", sp.empty() ? "no save found under ./save" : sp.c_str());
    if (ImGui::Button("Import unlocks from reference")) {
        // Reference = a 100% save at save_reference/zmha.bin. Every byte that is 0 here and 1 there
        // is set (unlock/clear flags); counters and other values are left alone. The game reads its
        // save at boot, so this takes effect on the next launch.
        auto ref = read_all("save_reference/zmha.bin"), cur = read_all(sp);
        size_t n = std::min(ref.size(), cur.size()), changed = 0;
        for (size_t i = 0x40; i < n; i++)
            if (cur[i] == 0 && ref[i] == 1) { cur[i] = 1; changed++; }
        if (!ref.empty() && !cur.empty()) {
            if (FILE* f = fopen((sp + ".bak").c_str(), "wb")) { auto o = read_all(sp); fwrite(o.data(), 1, o.size(), f); fclose(f); }
            if (FILE* f = fopen(sp.c_str(), "wb")) { fwrite(cur.data(), 1, cur.size(), f); fclose(f); }
            status = std::to_string(changed) + " flags set - restart the game (backup: zmha.bin.bak)";
        } else {
            status = ref.empty() ? "no reference save at save_reference/zmha.bin" : "could not read the save";
        }
    }
    help("Copies every unlock flag from a 100% save placed at save_reference/zmha.bin into your save.\n"
         "Counters and other values are left alone. Backs up your save first. Takes effect on the next launch.");
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
}

static void tab_tools(bool& changed) {
    ImGui::SeparatorText("Files");
    if (ImGui::Button("Open hwder.log")) ShellExecuteW(nullptr, L"open", L"hwder.log", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::Button("Open game folder")) ShellExecuteW(nullptr, L"open", L".", nullptr, nullptr, SW_SHOWNORMAL);

    ImGui::SeparatorText("Debug switches");
    ImGui::TextDisabled("Each writes extra detail to hwder.log and can slow the game down. Read once at startup.");
    {
        static const char* switches[][2] = {
            {"HWDER_VALIDATION", "Vulkan validation layers (needs the Vulkan SDK)"},
            {"HWDER_GPU_TRACE", "GPU command trace"},
            {"HWDER_IPC_TRACE", "Service call (IPC) trace"},
            {"HWDER_FS_TRACE", "File system trace"},
            {"HWDER_SYNC_TRACE", "Thread synchronisation trace"},
            {"HWDER_PACE_TRACE", "Frame pacing trace"},
            {"HWDER_PRESENT_TRACE", "Present trace"},
            {"HWDER_VI_TRACE", "Display service (vi) trace"},
        };
        settings::Debug& d = settings::debug();
        for (auto& sw : switches) {
            bool on = d.env.count(sw[0]) != 0;
            if (ImGui::Checkbox(sw[1], &on)) {
                if (on) d.env.insert(sw[0]);
                else d.env.erase(sw[0]);
                changed = true;
            }
            ImGui::SetItemTooltip("%s", sw[0]);
            restart_marker(on != (getenv(sw[0]) != nullptr));
        }
    }

    ImGui::SeparatorText("Save file");
    static std::string status;
    static std::vector<std::pair<size_t, size_t>> clusters;
    std::string sp = save_path();
    if (ImGui::Button("Snapshot save")) {
        auto d = read_all(sp);
        if (FILE* f = fopen("save_snapshot.bin", "wb")) { fwrite(d.data(), 1, d.size(), f); fclose(f); }
        status = "snapshot taken (" + std::to_string(d.size()) + " bytes)";
    }
    ImGui::SameLine();
    if (ImGui::Button("Diff vs snapshot")) {
        auto a = read_all("save_snapshot.bin"), b = read_all(sp);
        clusters.clear();
        size_t n = std::min(a.size(), b.size()), start = SIZE_MAX, prev = 0, total = 0;
        for (size_t i = 0; i < n; i++) {
            if (a[i] == b[i]) continue;
            total++;
            if (start == SIZE_MAX) start = i;
            else if (i - prev > 64) { clusters.push_back({start, prev}); start = i; }
            prev = i;
        }
        if (start != SIZE_MAX) clusters.push_back({start, prev});
        status = std::to_string(total) + " bytes changed in " + std::to_string(clusters.size()) + " regions";
        for (auto& c : clusters) hw_log("cheats: save changed 0x%zx-0x%zx (%zu bytes)", c.first, c.second, c.second - c.first + 1);
    }
    if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
    for (size_t i = 0; i < clusters.size() && i < 8; i++)
        ImGui::TextDisabled("  0x%zx - 0x%zx (%zu bytes)", clusters[i].first, clusters[i].second, clusters[i].second - clusters[i].first + 1);
}

static void build_ui(VkExtent2D extent) {
    settings::Video& v = settings::video();
    bool changed = false;

    if (v.show_fps) {
        double sum = 0, worst = 0;
        for (double f : g_frame_ms) sum += f, worst = f > worst ? f : worst;
        double avg = sum / 120.0;
        ImGui::SetNextWindowPos(ImVec2(8, 8));
        ImGui::SetNextWindowBgAlpha(0.45f);
        ImGui::Begin("##fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        ImGui::Text("%.0f fps  %.1f ms (worst %.0f)", avg > 0 ? 1000.0 / avg : 0.0, avg, worst);
        ImGui::End();
    }
    if (!g_visible) return;

    // A fixed, centred menu: it sizes itself to the current tab, up to 90% of the window height.
    ImGui::SetNextWindowPos(ImVec2(extent.width * 0.5f, extent.height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(600 * v.menu_scale / 100.0f, 0), ImVec2(extent.width * 0.95f, extent.height * 0.9f));
    if (g_focus_menu) ImGui::SetNextWindowFocus();
    g_focus_menu = false;
    bool open = true;
    ImGui::Begin("HWDER", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

    // LB / RB (and Q / E, Page Up / Page Down) switch tabs, like Dusklight's menu.
    enum { kDisplay, kGraphics, kAudio, kInput, kGameplay, kTools, kTabs };
    static const char* names[kTabs] = {"Display", "Graphics", "Audio", "Input", "Gameplay", "Tools"};
    int select = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_Q) || ImGui::IsKeyPressed(ImGuiKey_PageUp))
        select = (g_tab + kTabs - 1) % kTabs;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_E) || ImGui::IsKeyPressed(ImGuiKey_PageDown))
        select = (g_tab + 1) % kTabs;
    if (ImGui::BeginTabBar("##tabs")) {
        for (int i = 0; i < kTabs; i++) {
            if (!ImGui::BeginTabItem(names[i], nullptr, select == i ? ImGuiTabItemFlags_SetSelected : 0)) continue;
            g_tab = i;
            switch (i) {
            case kDisplay: tab_display(extent, changed); break;
            case kGraphics: tab_graphics(changed); break;
            case kAudio: tab_audio(changed); break;
            case kInput: tab_input(changed); break;
            case kGameplay: tab_gameplay(); break;
            case kTools: tab_tools(changed); break;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Saved to hwder_settings.ini as you change them.   Esc / B / F1 / hold View closes.");

    // Escape or B closes the menu, unless they were busy closing a dropdown or ending a slider edit: ImGui
    // handles that in NewFrame, so check what was open on the previous frame. They must also be released
    // once after opening, so the press that opened the menu does not close it again.
    static bool busy_last = false;
    const bool busy = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) || ImGui::IsAnyItemActive();
    if (!ImGui::IsKeyDown(ImGuiKey_Escape) && !ImGui::IsKeyDown(ImGuiKey_GamepadFaceRight)) g_close_armed = true;
    else if (g_close_armed && !busy && !busy_last &&
             (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        open = false;
    busy_last = busy;
    ImGui::End();
    if (!open) set_visible(false);
    if (changed) settings::save();
}

void render(VkCommandBuffer cb, VkImage image, VkExtent2D extent) {
    if (!g_inited) return;
    std::lock_guard<std::recursive_mutex> l(g_m);
    if (hid::take_menu_request()) set_visible(!g_visible);
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    g_frame_ms[g_frame_i++ % 120] = (t.QuadPart - g_last_t.QuadPart) * 1000.0 / f.QuadPart;
    g_last_t = t;
    settings::Video& v = settings::video();
    if (!g_visible && !v.show_fps) return;
    auto vit = g_views.find(image);
    if (vit == g_views.end()) return;
    ImGui::GetStyle().FontScaleMain = v.menu_scale / 100.0f;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    build_ui(extent);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount == 0) return;
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = vit->second;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    vkCmdEndRendering(cb);
}

}  // namespace overlay
