#include "gpu/overlay.h"

#include "gpu/settings.h"
#include "hid/host_input.h"
#include "hwder/runtime.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"

#include <algorithm>
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
static ImFont* g_font_bold = nullptr;  // section headings and the title; null = the regular font
static float g_style_scale = 0.0f;     // scale the style was last built for
// Input tab "press a button to bind": the Switch button (remap row) being bound, when it began, and the host
// buttons already held then (they count once released). Afterwards menu navigation ignores the controller
// until every button is up, so the press that was bound does not also act in the menu.
struct Capture {
    int slot = -1;
    ULONGLONG started = 0;
    uint32_t ignore = 0;
};
static Capture g_capture;
static bool g_nav_hold = false;

constexpr float kFontPx = 18.0f;  // base text size at scale 1
const ImVec4 kGold(0.86f, 0.68f, 0.30f, 1.0f);

static void set_visible(bool on);

static void check_vk(VkResult r) {
    if (r != VK_SUCCESS) hw_log("overlay: vulkan error %d", (int)r);
}

// Dark panels with a gold accent. Sizes are for scale 1; the style is rebuilt whenever the scale changes.
static void apply_theme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.FontSizeBase = kFontPx;
    st.WindowPadding = ImVec2(22, 18);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 8);
    st.ItemInnerSpacing = ImVec2(8, 6);
    st.CellPadding = ImVec2(8, 5);
    st.WindowRounding = 10;
    st.ChildRounding = st.FrameRounding = st.PopupRounding = st.GrabRounding = st.TabRounding = st.ScrollbarRounding = 6;
    st.WindowBorderSize = 1;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;
    st.ScrollbarSize = 14;
    st.GrabMinSize = 12;
    st.SeparatorTextBorderSize = 2;
    st.SeparatorTextPadding = ImVec2(0, 4);
    auto a = [](ImVec4 c, float alpha) { return ImVec4(c.x, c.y, c.z, alpha); };
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.92f, 0.89f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.57f, 0.61f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.96f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.98f);
    c[ImGuiCol_Border] = a(kGold, 0.35f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.15f, 0.18f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.21f, 0.25f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.26f, 0.31f, 1.0f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.07f, 0.08f, 0.10f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.25f, 0.29f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.33f, 0.38f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = a(kGold, 0.8f);
    c[ImGuiCol_CheckMark] = kGold;
    c[ImGuiCol_SliderGrab] = a(kGold, 0.85f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.98f, 0.80f, 0.42f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.19f, 0.23f, 1.0f);
    c[ImGuiCol_ButtonHovered] = a(kGold, 0.55f);
    c[ImGuiCol_ButtonActive] = a(kGold, 0.80f);
    c[ImGuiCol_Header] = a(kGold, 0.30f);
    c[ImGuiCol_HeaderHovered] = a(kGold, 0.45f);
    c[ImGuiCol_HeaderActive] = a(kGold, 0.60f);
    c[ImGuiCol_Separator] = ImVec4(0.24f, 0.25f, 0.29f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.14f, 0.17f, 1.0f);
    c[ImGuiCol_TabHovered] = a(kGold, 0.55f);
    c[ImGuiCol_TabSelected] = ImVec4(0.58f, 0.44f, 0.17f, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = kGold;
    c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
    c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.14f, 0.17f, 1.0f);
    c[ImGuiCol_TableBorderLight] = c[ImGuiCol_TableBorderStrong] = ImVec4(0.22f, 0.23f, 0.27f, 1.0f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_NavCursor] = kGold;
    c[ImGuiCol_TextSelectedBg] = a(kGold, 0.35f);
    st.ScaleAllSizes(scale);
    st.FontScaleMain = scale;
}

// Footer hint: the key in a rounded cap, then what it does.
static void key_hint(const char* key, const char* action) {
    const ImGuiStyle& st = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize(key);
    const ImVec2 pad(st.FramePadding.x * 0.7f, st.FramePadding.y);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(ts.x + pad.x * 2, ts.y + pad.y * 2));
    dl->AddRectFilled(p, ImVec2(p.x + ts.x + pad.x * 2, p.y + ts.y + pad.y * 2), ImGui::GetColorU32(ImGuiCol_FrameBg), st.FrameRounding);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(kGold), key);
    ImGui::SameLine(0, st.ItemInnerSpacing.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", action);
    ImGui::SameLine(0, st.ItemSpacing.x * 2.5f);
}

// Section heading: bold gold text on a rule.
static void section(const char* label) {
    ImGui::Spacing();
    if (g_font_bold) ImGui::PushFont(g_font_bold, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
    if (g_font_bold) ImGui::PopFont();
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
    {
        // Segoe UI (on every Windows install) instead of the 13-pixel bitmap default. Font sizes are dynamic
        // in this ImGui, so one load covers every menu scale.
        char win[MAX_PATH] = {};
        GetWindowsDirectoryA(win, MAX_PATH);
        const std::string dir = std::string(win) + "\\Fonts\\";
        std::error_code ec;
        if (std::filesystem::exists(dir + "segoeui.ttf", ec)) io.Fonts->AddFontFromFileTTF((dir + "segoeui.ttf").c_str(), kFontPx);
        else io.Fonts->AddFontDefault();
        if (std::filesystem::exists(dir + "seguisb.ttf", ec))
            g_font_bold = io.Fonts->AddFontFromFileTTF((dir + "seguisb.ttf").c_str(), kFontPx);
    }
    apply_theme(1.0f);
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
    g_capture.slot = -1;
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

// The host controller drives ImGui's gamepad navigation (the Win32 backend's XInput path is compiled out),
// so PlayStation and Nintendo pads work too. Positional, as ImGui expects: South accepts, East goes back.
// Mirrors imgui_impl_win32's mapping and thresholds.
static void feed_gamepad() {
    ImGuiIO& io = ImGui::GetIO();
    const hid::RawPad p = hid::raw_pad();
    if (p.connected) io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    else io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    uint32_t b = p.buttons;
    int lx = p.lx, ly = p.ly, rx = p.rx, ry = p.ry, lt = p.lt, rt = p.rt;
    if (g_capture.slot < 0 && g_nav_hold && !b) g_nav_hold = false;
    if (g_capture.slot >= 0 || g_nav_hold) b = 0, lx = ly = rx = ry = lt = rt = 0;
    auto button = [&](ImGuiKey k, int host) { io.AddKeyEvent(k, (b >> host) & 1); };
    auto analog = [&](ImGuiKey k, int v, int v0, int v1) {
        const float vn = std::clamp((float)(v - v0) / (float)(v1 - v0), 0.0f, 1.0f);
        io.AddKeyAnalogEvent(k, vn > 0.10f, vn);
    };
    button(ImGuiKey_GamepadStart, hid::kHostStart);
    button(ImGuiKey_GamepadBack, hid::kHostBack);
    button(ImGuiKey_GamepadFaceLeft, hid::kHostWest);
    button(ImGuiKey_GamepadFaceRight, hid::kHostEast);
    button(ImGuiKey_GamepadFaceUp, hid::kHostNorth);
    button(ImGuiKey_GamepadFaceDown, hid::kHostSouth);
    button(ImGuiKey_GamepadDpadLeft, hid::kHostDpadLeft);
    button(ImGuiKey_GamepadDpadRight, hid::kHostDpadRight);
    button(ImGuiKey_GamepadDpadUp, hid::kHostDpadUp);
    button(ImGuiKey_GamepadDpadDown, hid::kHostDpadDown);
    button(ImGuiKey_GamepadL1, hid::kHostLeftShoulder);
    button(ImGuiKey_GamepadR1, hid::kHostRightShoulder);
    button(ImGuiKey_GamepadL3, hid::kHostLeftStick);
    button(ImGuiKey_GamepadR3, hid::kHostRightStick);
    analog(ImGuiKey_GamepadL2, lt, 3855, 32767);  // XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 30 of 255
    analog(ImGuiKey_GamepadR2, rt, 3855, 32767);
    const int dz = 8000;
    analog(ImGuiKey_GamepadLStickLeft, lx, -dz, -32767);
    analog(ImGuiKey_GamepadLStickRight, lx, dz, 32767);
    analog(ImGuiKey_GamepadLStickUp, ly, dz, 32767);
    analog(ImGuiKey_GamepadLStickDown, ly, -dz, -32767);
    analog(ImGuiKey_GamepadRStickLeft, rx, -dz, -32767);
    analog(ImGuiKey_GamepadRStickRight, rx, dz, 32767);
    analog(ImGuiKey_GamepadRStickUp, ry, dz, 32767);
    analog(ImGuiKey_GamepadRStickDown, ry, -dz, -32767);
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
    section("Frame rate");
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

    section("Window");
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

    section("Overlay");
    if (ImGui::Checkbox("Show frame time", &v.show_fps)) changed = true;
    {
        static const int scales[] = {75, 90, 100, 115, 130, 150};
        char cur[16];
        snprintf(cur, sizeof(cur), "%d%%", v.menu_scale);
        if (ImGui::BeginCombo("Menu size", cur)) {
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

    section("Shaders");
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

// Binds the next host button pressed to the remap row being captured; Escape or 5 s cancels. A button
// already bound to another row swaps with this row's old binding, so nothing is lost.
static void update_capture(int family, bool& changed) {
    if (g_capture.slot < 0) return;
    const hid::RawPad p = hid::raw_pad();
    g_capture.ignore &= p.buttons;
    const uint32_t fresh = p.buttons & ~g_capture.ignore;
    bool done = false;
    if (fresh) {
        int host = 0;
        while (!((fresh >> host) & 1)) host++;
        int* map = settings::input().remap[family];
        for (int i = 0; i < settings::kRemapSlots; i++)
            if (i != g_capture.slot && map[i] == host) map[i] = map[g_capture.slot];
        map[g_capture.slot] = host;
        changed = true;
        done = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || GetTickCount64() - g_capture.started > 5000) {
        done = true;
    }
    if (done) {
        g_capture.slot = -1;
        g_nav_hold = true;
        g_close_armed = false;  // the Escape that cancelled must not close the menu too
    }
}

static void tab_input(bool& changed) {
    settings::Input& in = settings::input();
    const hid::RawPad pad = hid::raw_pad();
    const int family = pad.connected ? pad.family : settings::kPadXbox;
    const int type = pad.connected ? pad.type : hid::kPadTypeXboxOne;
    update_capture(family, changed);

    section("Controller");
    if (pad.connected) {
        ImGui::TextUnformatted(pad.name);
        ImGui::SameLine();
        ImGui::TextDisabled("%s%s%s%s", hid::pad_type_name(pad.type), pad.has_gyro ? "  |  gyro" : "",
                            pad.has_rumble ? "  |  rumble" : "", pad.sdl ? "" : "  |  XInput");
    } else {
        ImGui::TextDisabled("No controller connected. The button table edits the Xbox layout.");
    }
    if (ImGui::Checkbox("Hold View / Select to open this menu", &in.hold_view_menu)) changed = true;
    help("Hold for half a second to open or close the menu. A tap is still the game's - button; the game sees it\n"
         "when you let go. Off: View is the - button straight away and the menu opens with Escape or F1 only.");
    if (ImGui::Checkbox("Read the controller while another window is active", &in.background_pad)) changed = true;
    help("Off: the game ignores the controller while it is in the background. The keyboard is always ignored then.");

    section("Buttons");
    {
        const bool fixed = hid::face_labels_fixed(family);
        int mode = in.face_labels && !fixed ? 1 : 0;
        static const char* modes[] = {"By position (Switch layout)", "By printed label"};
        ImGui::BeginDisabled(fixed);
        if (ImGui::Combo("Face buttons", &mode, modes, 2)) {
            in.face_labels = mode == 1;
            for (int f = 0; f < settings::kPadFamilies; f++) {
                int def[settings::kRemapSlots];
                hid::default_remap(def, f, in.face_labels);
                for (int i = 0; i < 4; i++) in.remap[f][i] = def[i];
            }
            changed = true;
        }
        ImGui::EndDisabled();
        help(fixed ? "On Nintendo controllers the printed letters already sit where the Switch has them."
                   : "By position: the bottom face button is Switch B and the right one Switch A, as on a Switch\n"
                     "controller, so the game's button prompts match each button's place.\n"
                     "By printed label: Xbox A / PlayStation Cross is Switch A, so prompts match the letter but\n"
                     "A and B (and X and Y) swap places. Resets the four face buttons in the table below.");
    }
    if (ImGui::BeginTable("##remap", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        char head[64];
        snprintf(head, sizeof(head), "%s button", pad.connected ? hid::pad_type_name(type) : "Xbox");
        ImGui::TableSetupColumn("Switch", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(head, ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableHeadersRow();
        int* map = in.remap[family];
        for (int i = 0; i < settings::kRemapSlots; i++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(hid::switch_button_name(i));
            ImGui::TableSetColumnIndex(1);
            char label[96];
            const bool capturing = g_capture.slot == i;
            if (capturing)
                snprintf(label, sizeof(label), "Press a button... (%d s, Esc cancels)###bind%d",
                         (int)(5 - std::min<ULONGLONG>(5, (GetTickCount64() - g_capture.started) / 1000)), i);
            else
                snprintf(label, sizeof(label), "%s###bind%d", map[i] < 0 ? "(none)" : hid::host_button_name(map[i], type), i);
            if (capturing) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label, ImVec2(-FLT_MIN, 0)) && g_capture.slot < 0) {
                g_capture.slot = i;
                g_capture.started = GetTickCount64();
                g_capture.ignore = pad.buttons;
            }
            if (capturing) ImGui::PopStyleColor();
            ImGui::SetItemTooltip("Select, then press the controller button to use for Switch %s", hid::switch_button_name(i));
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset to defaults")) {
        hid::default_remap(in.remap[family], family, in.face_labels);
        changed = true;
    }
    help("Restores the default layout for this kind of controller (Xbox, PlayStation, Nintendo and other\n"
         "controllers each keep their own). Holding View still opens this menu, whatever it is bound to.");

    section("Sticks");
    if (ImGui::SliderInt("Left stick deadzone", &in.deadzone_l, 0, 50, "%d%%", ImGuiSliderFlags_AlwaysClamp)) changed = true;
    if (ImGui::SliderInt("Right stick deadzone", &in.deadzone_r, 0, 50, "%d%%", ImGuiSliderFlags_AlwaysClamp)) changed = true;
    help("How far a stick must move before the game sees it. Raise it if the camera or character drifts.");
    if (ImGui::Checkbox("Invert left X", &in.invert_lx)) changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert left Y", &in.invert_ly)) changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert right X", &in.invert_rx)) changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert right Y", &in.invert_ry)) changed = true;

    section("Rumble");
    if (ImGui::Checkbox("Rumble", &in.rumble)) {
        changed = true;
        if (in.rumble) hid::preview_rumble();
    }
    ImGui::BeginDisabled(!in.rumble);
    if (ImGui::SliderInt("Rumble strength", &in.rumble_strength, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) {
        changed = true;
        hid::preview_rumble();  // pulses at the new strength while the slider moves
    }
    ImGui::EndDisabled();
    if (pad.connected && !pad.has_rumble) ImGui::TextDisabled("This controller reports no rumble motors.");

    section("Motion");
    if (ImGui::Checkbox("Motion controls", &in.motion)) changed = true;
    help("Passes the controller's gyro and accelerometer to the game (DualSense, DualShock 4, Switch Pro and\n"
         "others with motion sensors). Hyrule Warriors itself barely uses motion.");
    if (pad.connected && !pad.has_gyro) ImGui::TextDisabled("This controller has no motion sensors.");

    section("Keyboard");
    if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Switch");
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableHeadersRow();
        static const char* rows[][2] = {
            {"A / B / X / Y", "K or Enter / J or Backspace / I / L"},
            {"L / R", "Q / E"},
            {"ZL / ZR", "U / O"},
            {"+ / -", "Space / Tab"},
            {"Left stick", "W A S D"},
            {"Right stick", "Arrow keys"},
            {"D-pad", "1 2 3 4"},
            {"Stick clicks", "Z / C"},
            {"HWDER menu", "Escape or F1"},
        };
        for (auto& r : rows) {
            ImGui::TableNextRow();
            for (int c = 0; c < 2; c++) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TextUnformatted(r[c]);
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("In this menu: D-pad or left stick to move, %s to select, %s to go back or close, %s / %s to switch tabs.",
                        hid::host_button_name(hid::kHostSouth, type), hid::host_button_name(hid::kHostEast, type),
                        hid::host_button_name(hid::kHostLeftShoulder, type), hid::host_button_name(hid::kHostRightShoulder, type));
}

static void tab_gameplay() {
    section("Cheats (save file)");
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
    section("Files");
    if (ImGui::Button("Open hwder.log")) ShellExecuteW(nullptr, L"open", L"hwder.log", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (ImGui::Button("Open game folder")) ShellExecuteW(nullptr, L"open", L".", nullptr, nullptr, SW_SHOWNORMAL);

    section("Debug switches");
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

    section("Build");
    // Build identity for bug reports (same string as the first line of hwder.log).
    static char version[160];
    if (!version[0]) snprintf(version, sizeof(version), "HWDER %s (commit %s, built %s)", hw_version(), hw_commit(), hw_build_date());
    ImGui::TextDisabled("%s", version);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(version);

    section("Save file");
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

    const ImGuiStyle& st = ImGui::GetStyle();
    const float s = g_style_scale;
    // Dim the game behind the menu.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2((float)extent.width, (float)extent.height),
                                                  IM_COL32(0, 0, 0, 150));
    // A fixed-size, centred panel; long tabs scroll inside it, so switching tabs never resizes it.
    const float w = std::min(extent.width * 0.94f, 820.0f * s);
    const float h = std::min(extent.height * 0.9f, 680.0f * s);
    ImGui::SetNextWindowPos(ImVec2(extent.width * 0.5f, extent.height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    if (g_focus_menu) ImGui::SetNextWindowFocus();
    g_focus_menu = false;
    bool open = true;
    ImGui::Begin("HWDER", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    {   // header: large title, game name beside it on the same baseline
        const float y = ImGui::GetCursorPosY();
        ImGui::PushFont(g_font_bold, st.FontSizeBase * 1.6f);
        const float big = ImGui::GetTextLineHeight();
        ImGui::TextColored(kGold, "HWDER");
        ImGui::PopFont();
        ImGui::SameLine(0.0f, st.ItemSpacing.x * 1.5f);
        ImGui::SetCursorPosY(y + big - ImGui::GetTextLineHeight() - 2.0f * s);
        ImGui::TextDisabled("Hyrule Warriors: Definitive Edition");
    }
    ImGui::Spacing();
    const float footer = ImGui::GetFrameHeight() + st.ItemSpacing.y * 2.0f + st.SeparatorSize + 2.0f * s;

    // LB / RB (and Q / E, Page Up / Page Down) switch tabs, like Dusklight's menu.
    enum { kDisplay, kGraphics, kAudio, kInput, kGameplay, kTools, kTabs };
    static const char* names[kTabs] = {"Display", "Graphics", "Audio", "Input", "Gameplay", "Tools"};
    int select = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_Q) || ImGui::IsKeyPressed(ImGuiKey_PageUp))
        select = (g_tab + kTabs - 1) % kTabs;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1) || ImGui::IsKeyPressed(ImGuiKey_E) || ImGui::IsKeyPressed(ImGuiKey_PageDown))
        select = (g_tab + 1) % kTabs;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x * 1.8f, st.FramePadding.y * 1.4f));
    const bool tabs = ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_DrawSelectedOverline);
    ImGui::PopStyleVar();
    if (tabs) {
        for (int i = 0; i < kTabs; i++) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x * 1.8f, st.FramePadding.y * 1.4f));
            ImGui::PushFont(g_font_bold, 0.0f);
            const bool sel = ImGui::BeginTabItem(names[i], nullptr, select == i ? ImGuiTabItemFlags_SetSelected : 0);
            ImGui::PopFont();
            ImGui::PopStyleVar();
            if (!sel) continue;
            g_tab = i;
            ImGui::BeginChild("##page", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
            ImGui::Spacing();
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
            switch (i) {
            case kDisplay: tab_display(extent, changed); break;
            case kGraphics: tab_graphics(changed); break;
            case kAudio: tab_audio(changed); break;
            case kInput: tab_input(changed); break;
            case kGameplay: tab_gameplay(); break;
            case kTools: tab_tools(changed); break;
            }
            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (g_tab != kInput) g_capture.slot = -1;
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - st.WindowPadding.y - footer + st.ItemSpacing.y);
    ImGui::Separator();
    {   // footer: key hints on the left, a Close button on the right
        const char* close = "Close";
        const float bw = ImGui::CalcTextSize(close).x + st.FramePadding.x * 4.0f;
        // Labels of the controller in hand: Cross / Circle on PlayStation, B / A on Nintendo.
        const hid::RawPad pad = hid::raw_pad();
        const int type = pad.connected ? pad.type : hid::kPadTypeXboxOne;
        char tabs_key[32], close_key[32];
        snprintf(tabs_key, sizeof(tabs_key), "%s / %s", hid::host_button_name(hid::kHostLeftShoulder, type),
                 hid::host_button_name(hid::kHostRightShoulder, type));
        snprintf(close_key, sizeof(close_key), "%s / Esc", hid::host_button_name(hid::kHostEast, type));
        key_hint(tabs_key, "Tabs");
        key_hint(hid::host_button_name(hid::kHostSouth, type), "Select");
        key_hint(close_key, "Close");
        ImGui::TextDisabled("Changes save automatically");
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - bw));
        if (ImGui::Button(close, ImVec2(bw, 0))) open = false;
    }

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
    // The menu grows with the window: scale 1 at 900 pixels high, times the user's menu size.
    const float scale = std::clamp(extent.height / 900.0f, 0.75f, 3.0f) * v.menu_scale / 100.0f;
    if (fabsf(scale - g_style_scale) > 0.001f) {
        apply_theme(scale);
        g_style_scale = scale;
    }
    if (g_visible) feed_gamepad();
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
