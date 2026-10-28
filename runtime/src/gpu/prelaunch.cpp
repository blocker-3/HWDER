#include "gpu/prelaunch.h"

#include "fs/nsp.h"
#include "gpu/display.h"
#include "gpu/overlay.h"
#include "gpu/overlay_ui.h"
#include "gpu/settings.h"
#include "hid/host_input.h"
#include "hwder/runtime.h"

#ifdef _WIN32
#include <shobjidl.h>
#else
#include <SDL3/SDL.h>
#include <condition_variable>
#include <mutex>
#endif
#include "host/timing.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "debug_flags.h"

#ifdef _WIN32
#pragma comment(lib, "ole32.lib")  // lld honours these on Linux too (Zig toolchain): Windows only
#endif

namespace prelaunch {

namespace {

enum class Pick { None, Game, Keys };

std::string g_dir;
Pick g_pick = Pick::None;  // requested in the frame, opened by run() outside the overlay lock
bool g_play = false, g_quit = false;
bool g_first = true;       // first frame: focus the window and the default button
bool g_focus_play = false;  // a pick made both checks pass: move to Play

const ImVec4 kOk(0.47f, 0.80f, 0.47f, 1.0f);
const ImVec4 kPending(1.0f, 0.7f, 0.25f, 1.0f);
const ImVec4 kError(0.95f, 0.42f, 0.36f, 1.0f);

#ifdef _WIN32
std::string to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// The Windows file picker, owned by the game window, starting in the folder of `current` if set.
std::string pick_file(const wchar_t* title, const wchar_t* type_name, const wchar_t* type_spec, const std::string& current) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        hw_log("prelaunch: file dialog unavailable");
        return "";
    }
    COMDLG_FILTERSPEC types[] = {{type_name, type_spec}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    if (size_t slash = current.find_last_of("\\/"); slash != std::string::npos) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(to_wide(current.substr(0, slash)).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    std::string out;
    if (SUCCEEDED(dlg->Show(display::window()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = to_utf8(p);
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

// Re-runs the NSP and key checks with the current picks.
#else
std::string pick_file(const wchar_t*, const wchar_t*, const wchar_t* type_spec, const std::string& current) {
    struct Request {
        std::mutex mutex;
        std::condition_variable wake;
        bool complete = false;
        std::string path;
        SDL_DialogFileFilter filter;
    } request;
    const bool game = std::wstring(type_spec) == L"*.nsp";
    request.filter = game ? SDL_DialogFileFilter{"Nintendo Switch package", "nsp"}
                          : SDL_DialogFileFilter{"Key files", "keys"};
    display::on_main_thread([&] {
        SDL_ShowOpenFileDialog([](void* opaque, const char* const* files, int) {
            auto& result = *static_cast<Request*>(opaque);
            { std::lock_guard lock(result.mutex);
              if (files && files[0]) result.path = files[0];
              result.complete = true;
              result.wake.notify_one(); }
        }, &request, display::window(), &request.filter, 1, current.empty() ? nullptr : current.c_str(), false);
    });
    std::unique_lock lock(request.mutex);
    request.wake.wait(lock, [&] { return request.complete; });
    return request.path;
}
#endif


void check() {
    hwfs::nsp::set_paths(settings::launch().nsp_path, settings::launch().keys_path);
    hwfs::nsp::set_update(settings::launch().update_path);
    hwfs::nsp::reset();
    std::string err;
    hwfs::nsp::init(g_dir, err);
}

void status_line(const ImVec4& color, const char* fmt, const std::string& text) {
    if (overlay::bold_font()) ImGui::PushFont(overlay::bold_font(), 0.0f);
    ImGui::TextColored(color, fmt, text.c_str());
    if (overlay::bold_font()) ImGui::PopFont();
}

void game_section() {
    using hwfs::nsp::Status;
    const hwfs::nsp::Info& inf = hwfs::nsp::info();
    overlay::section("Game file");
    if (inf.status == Status::Ok && !inf.warning.empty()) {
        char line[64];
        snprintf(line, sizeof(line), "%016llX", (unsigned long long)inf.title_id);
        status_line(kPending, "Doesn't look like Hyrule Warriors: Definitive Edition (title %s) - you can still try it", line);
        ImGui::TextWrapped("Found %s.", inf.warning.c_str());
    } else if (inf.status == Status::Ok) status_line(kOk, "%s", "Ready");
    else if (!inf.nsp_found) status_line(kError, "%s", "Not found");
    else if (inf.status == Status::NoKeys) status_line(kPending, "%s", "Found - it is checked once your prod.keys is found");
    else
        status_line(kError, inf.error.find("key") != std::string::npos ? "Can't decrypt: %s" : "Can't read: %s", inf.error);
    if (inf.nsp_path.empty()) ImGui::TextDisabled("No .nsp file next to hwder.exe or in its game folder.");
    else ImGui::TextWrapped("%s", inf.nsp_path.c_str());
    if (inf.title_id) {
        char line[96];
        int n = snprintf(line, sizeof(line), "Title ID %016llX", (unsigned long long)inf.title_id);
        if (inf.has_version) snprintf(line + n, sizeof(line) - n, "   |   Version %u%s", inf.version, inf.version ? "" : " (no update)");
        ImGui::TextDisabled("%s", line);
    }
    if (debug::flag(debug::Nsp)) ImGui::TextColored(kPending, "HWDER_NSP is set: it is used instead of a file chosen here.");
    if (ImGui::Button("Choose game file...")) g_pick = Pick::Game;
    if (g_first && !inf.nsp_found) ImGui::SetItemDefaultFocus();
}

void keys_section() {
    const hwfs::nsp::Info& inf = hwfs::nsp::info();
    overlay::section("Keys");
    if (inf.keys_found) {
        char line[96];
        snprintf(line, sizeof(line), "Found: %zu keys", inf.key_count);
        if (inf.title_key_count) snprintf(line + strlen(line), sizeof(line) - strlen(line), ", %zu title keys", inf.title_key_count);
        status_line(inf.key_count ? kOk : kError, "%s", inf.key_count ? std::string(line) : "No keys could be read from this file");
    } else {
        status_line(kError, "%s", "Not found");
    }
    if (!inf.keys_path.empty()) ImGui::TextWrapped("%s", inf.keys_path.c_str());
    else
        ImGui::TextWrapped("Looked for prod.keys next to hwder.exe, in its keys folder, in %%USERPROFILE%%\\.switch and in the "
                           "yuzu and Ryujinx key folders.");
    if (debug::flag(debug::Keys)) ImGui::TextColored(kPending, "HWDER_KEYS is set: it is used instead of a file chosen here.");
    if (ImGui::Button("Choose prod.keys...")) g_pick = Pick::Keys;
    if (g_first && inf.nsp_found && !inf.keys_found) ImGui::SetItemDefaultFocus();
}

// Why Play is greyed out, in a few words.
const char* blocked_reason() {
    using hwfs::nsp::Status;
    const hwfs::nsp::Info& inf = hwfs::nsp::info();
    if (!inf.nsp_found && !inf.keys_found) return "Choose your game file and prod.keys to play.";
    if (!inf.nsp_found) return "Choose your game file to play.";
    if (!inf.keys_found || inf.key_count == 0) return "Choose your prod.keys to play.";
    return "The game file can't be read with these keys.";
}

void draw(VkExtent2D extent) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float s = overlay::ui_scale();
    const bool ready = hwfs::nsp::active();
    bool changed = false;

    const float w = std::min(extent.width * 0.94f, 860.0f * s);
    const float h = std::min(extent.height * 0.92f, 780.0f * s);
    ImGui::SetNextWindowPos(ImVec2(extent.width * 0.5f, extent.height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    if (g_first) ImGui::SetNextWindowFocus();
    ImGui::Begin("##prelaunch", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    {   // header: the game's name large, HWDER beside it on the same baseline
        const float y = ImGui::GetCursorPosY();
        ImGui::PushFont(overlay::bold_font(), st.FontSizeBase * 1.6f);
        const float big = ImGui::GetTextLineHeight();
        ImGui::TextColored(overlay::kGold, "Hyrule Warriors: Definitive Edition");
        ImGui::PopFont();
        ImGui::SameLine(0.0f, st.ItemSpacing.x * 1.5f);
        ImGui::SetCursorPosY(y + big - ImGui::GetTextLineHeight() - 2.0f * s);
        ImGui::TextDisabled("HWDER %s", hw_version());
    }
    ImGui::Spacing();

    // Footer: Play / Quit, the skip checkbox, key hints. The rest scrolls above it.
    const float footer = ImGui::GetFrameHeight() * 3.0f + st.ItemSpacing.y * 6.0f + 4.0f * s;
    ImGui::BeginChild("##body", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
    game_section();
    keys_section();
    overlay::section("Save folder");
    overlay::opt_save_folder(false, changed);
    overlay::section("Quick settings");
    overlay::opt_borderless(changed);
    overlay::opt_window_size(extent, changed);
    overlay::opt_internal_res(changed);
    overlay::opt_interpolation(changed);
    overlay::opt_volume(changed);
    ImGui::TextDisabled("Everything else is in the in-game menu (Escape, F1 or hold View).");
    ImGui::PopItemWidth();
    ImGui::EndChild();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - st.WindowPadding.y - footer + st.ItemSpacing.y);
    ImGui::Separator();
    {
        const float bw = 150.0f * s;
        if (g_focus_play && ready) ImGui::SetKeyboardFocusHere();
        g_focus_play = false;
        ImGui::BeginDisabled(!ready);
        ImGui::PushFont(overlay::bold_font(), 0.0f);
        if (ImGui::Button("Play", ImVec2(bw, 0)) && ready) {
            if (hwfs::nsp::info().warning.empty()) g_play = true;
            else ImGui::OpenPopup("Unverified game file");
        }
        ImGui::PopFont();
        ImGui::EndDisabled();
        if (g_first && ready) ImGui::SetItemDefaultFocus();
        // A title ID that is not HWDE's is a warning, not a gate: confirm, then play anyway.
        ImGui::SetNextWindowPos(ImVec2(extent.width * 0.5f, extent.height * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(extent.width * 0.9f, 560.0f * s), 0));
        if (ImGui::BeginPopupModal("Unverified game file", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            const hwfs::nsp::Info& inf = hwfs::nsp::info();
            ImGui::TextWrapped("This file doesn't look like Hyrule Warriors: Definitive Edition (title ID %016llX, expected %016llX). "
                               "It may crash or misbehave.",
                               (unsigned long long)inf.title_id, (unsigned long long)hwfs::nsp::kTitleId);
            ImGui::Spacing();
            if (ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
            ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            if (ImGui::Button("Play anyway", ImVec2(bw, 0))) {
                hw_log("prelaunch: playing with unverified game file (title ID %016llX)", (unsigned long long)inf.title_id);
                g_play = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(bw * 0.7f, 0))) g_quit = true;
        if (!ready) {
            ImGui::SameLine(0.0f, st.ItemSpacing.x * 2.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kPending, "%s", blocked_reason());
        }
    }
    if (ImGui::Checkbox("Skip this screen next time (hold Shift at launch to show it again)", &settings::launch().skip_prelaunch))
        changed = true;
    ImGui::Separator();
    char select_key[32];
    snprintf(select_key, sizeof(select_key), "%s / Enter", overlay::accept_button_name());
    overlay::key_hint(select_key, "Select");
    overlay::key_hint("D-pad", "Move");
    overlay::key_hint("Esc", "Quit");
    ImGui::TextDisabled("Settings save automatically");

    // Escape quits, unless it was closing a dropdown or ending a slider edit (see the in-game menu).
    static bool busy_last = false;
    const bool busy = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) || ImGui::IsAnyItemActive();
    if (!busy && !busy_last && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) g_quit = true;
    busy_last = busy;
    ImGui::End();
    g_first = false;
    if (changed) settings::save();
}

}  // namespace

namespace {
std::string preparation_message;
unsigned preparation_done = 0, preparation_total = 0;
void draw_preparation(VkExtent2D extent) {
    ImGui::SetNextWindowPos(ImVec2(extent.width*0.5f,extent.height*0.5f),ImGuiCond_Always,ImVec2(0.5f,0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(float(extent.width)*0.9f,640.0f*overlay::ui_scale()),0));
    ImGui::Begin("Preparing graphics",nullptr,ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextWrapped("%s",preparation_message.c_str());
    if(preparation_total) {
        char label[64];snprintf(label,sizeof(label),"%u / %u",preparation_done,preparation_total);
        ImGui::ProgressBar(float(preparation_done)/preparation_total,ImVec2(-1,0),label);
    }
    ImGui::End();
}
}
void preparation_frame(const char* message,unsigned done,unsigned total) {
    if(!message){overlay::set_screen(nullptr);return;}
    preparation_message=message;preparation_done=done;preparation_total=total;
    display::set_status("preparing graphics");overlay::set_screen(draw_preparation);display::present_overlay_frame();
}

bool run(const std::string& exe_dir) {
    g_dir = exe_dir;
#ifdef _WIN32
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
#endif
    if (!hwfs::nsp::active()) check();
    display::set_status("startup");
    overlay::set_screen(draw);
    hw_log("prelaunch: screen shown");
    const char* frames_env = debug::str(debug::PrelaunchFrames);
    const int max_frames = frames_env ? atoi(frames_env) : 0;
    for (int frame = 1; !g_play && !g_quit; frame++) {
        // The HID thread starts with the game, so poll the controller here for the screen's navigation.
        hid::HostInput pad_in;
        hid::poll_host_input(pad_in);
        display::present_overlay_frame();
        if (g_pick != Pick::None) {
            const Pick p = g_pick;
            g_pick = Pick::None;
            settings::Launch& l = settings::launch();
            const std::string& cur = p == Pick::Game ? (l.nsp_path.empty() ? hwfs::nsp::info().nsp_path : l.nsp_path)
                                                     : (l.keys_path.empty() ? hwfs::nsp::info().keys_path : l.keys_path);
            std::string path = p == Pick::Game ? pick_file(L"Choose your Hyrule Warriors: Definitive Edition game file",
                                                           L"Nintendo Switch package (*.nsp)", L"*.nsp", cur)
                                               : pick_file(L"Choose your prod.keys", L"Key files (*.keys)", L"*.keys", cur);
            if (!path.empty()) {
                (p == Pick::Game ? l.nsp_path : l.keys_path) = path;
                settings::save();
                hw_log("prelaunch: %s set to %s", p == Pick::Game ? "game file" : "keys", path.c_str());
                check();
                g_focus_play = hwfs::nsp::active();
            }
        }
        if (max_frames && frame >= max_frames) g_quit = true;
        host::sleep_ms(15);
    }
    overlay::set_screen(nullptr);
#ifdef _WIN32
    if (SUCCEEDED(co)) CoUninitialize();
#endif
    hw_log("prelaunch: %s", g_play ? "play" : "quit");
    return g_play;
}

}  // namespace prelaunch
