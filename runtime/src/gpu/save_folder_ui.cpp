// The save folder option, shared by the startup screen and the in-game menu (overlay_ui.h).
// Changing the folder never moves or deletes saves: when the old folder has any, the user is asked whether to copy
// them, and whatever the copy would overwrite in the new folder is backed up first (hwfs::copy_saves). On the startup
// screen the copy runs at once; in the game it runs at the next launch, so it includes everything saved until then.
#include "gpu/overlay_ui.h"

#include "fs/save_dir.h"
#include "gpu/display.h"
#include "gpu/settings.h"
#include "hwder/runtime.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace overlay {

namespace {

const ImVec4 kOk(0.47f, 0.80f, 0.47f, 1.0f);
const ImVec4 kPending(1.0f, 0.7f, 0.25f, 1.0f);
const ImVec4 kError(0.95f, 0.42f, 0.36f, 1.0f);
const char* kPopup = "Copy your saves?";

std::mutex g_m;
bool g_picking = false;  // the folder dialog is open (on its own thread, so the frame keeps drawing)
bool g_picked = false;
std::string g_pick;      // its result, "" = cancelled

std::string g_ask;       // folder waiting on the copy question
bool g_ask_there = false;  // it already has saves
std::string g_status;
ImVec4 g_status_color = kOk;

std::wstring to_wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string to_utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void pick_thread(std::string start) {
    std::string out;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        dlg->SetTitle(L"Choose a folder for your saves");
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        // Start in the current folder, or the nearest one that exists.
        for (std::filesystem::path p = to_wide(start); !p.empty(); p = p.parent_path()) {
            IShellItem* folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
                dlg->SetFolder(folder);
                folder->Release();
                break;
            }
            if (p == p.parent_path()) break;
        }
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
    } else {
        hw_log("save folder: folder dialog unavailable");
    }
    if (SUCCEEDED(co)) CoUninitialize();
    std::lock_guard<std::mutex> l(g_m);
    g_pick = out;
    g_picked = true;
    g_picking = false;
}

bool writable(const std::string& dir) {
    std::error_code ec;
    std::filesystem::create_directories(to_wide(dir), ec);
    HANDLE h = CreateFileW(to_wide(dir + "\\hwder-write-test.tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void set_status(const ImVec4& color, const std::string& text) {
    g_status_color = color;
    g_status = text;
    hw_log("save folder: %s", text.c_str());
}

// Switches to `dir`, copying the saves the game uses now when `copy` is set.
void apply(const std::string& dir, bool copy, bool in_game, bool& changed) {
    settings::Launch& l = settings::launch();
    const std::string from = hwfs::active_save_dir(), previous = l.save_dir;
    l.save_dir = hwfs::same_dir(dir, hwfs::default_save_dir()) ? "" : dir;
    l.save_copy_from.clear();
    changed = true;
    const std::string restart = in_game && !hwfs::same_dir(dir, from) ? " Restart the game to use it." : "";
    if (!copy) {
        set_status(kOk, "Save folder set to " + dir + "." + restart);
    } else if (in_game) {
        l.save_copy_from = from;
        set_status(kOk, "Your saves will be copied here from " + from + " when you next start the game. Nothing in " +
                            from + " is moved or deleted.");
    } else {
        std::string report;
        if (hwfs::copy_saves(from, dir, report)) {
            set_status(kOk, report);
        } else {
            l.save_dir = previous;
            set_status(kError, report + " The save folder was not changed.");
        }
    }
}

void on_pick(const std::string& dir, bool in_game, bool& changed) {
    if (!writable(dir)) {
        set_status(kError, "Can't write to " + dir + ". Choose another folder.");
        return;
    }
    if (hwfs::same_dir(dir, hwfs::chosen_save_dir())) {
        set_status(kOk, "That is already the save folder.");
        return;
    }
    const std::string from = hwfs::active_save_dir();
    if (!hwfs::same_dir(dir, from) && hwfs::has_files(from)) {
        g_ask = dir;  // the question opens this frame
        g_ask_there = hwfs::has_files(dir);
    } else {
        apply(dir, false, in_game, changed);
    }
}

}  // namespace

void opt_save_folder(bool in_game, bool& changed) {
    {
        std::unique_lock<std::mutex> l(g_m);
        if (g_picked) {
            g_picked = false;
            const std::string pick = g_pick;
            l.unlock();
            if (!pick.empty()) on_pick(pick, in_game, changed);
        }
    }
    const settings::Launch& ls = settings::launch();
    const std::string chosen = hwfs::chosen_save_dir(), active = hwfs::active_save_dir();
    ImGui::TextWrapped("%s%s", chosen.c_str(), ls.save_dir.empty() ? "  (default)" : "");
    if (in_game && !hwfs::same_dir(chosen, active))
        ImGui::TextColored(kPending, "Restart the game to use it. Until then it keeps saving to %s.", active.c_str());
    if (!ls.save_copy_from.empty())
        ImGui::TextColored(kPending, "Your saves will be copied from %s at the next start.", ls.save_copy_from.c_str());

    bool picking;
    {
        std::lock_guard<std::mutex> l(g_m);
        picking = g_picking;
    }
    ImGui::BeginDisabled(picking);
    if (ImGui::Button("Choose save folder...")) {
        std::lock_guard<std::mutex> l(g_m);
        g_picking = true;
        std::thread(pick_thread, chosen).detach();
    }
    ImGui::EndDisabled();
    help("Where your saves are kept. The default is the save folder next to hwder.exe.\n"
         "Changing it never moves or deletes saves: if the current folder has any, you are asked whether to copy them,\n"
         "and anything the copy would replace in the new folder is backed up beside it first.");
    ImGui::SameLine();
    ImGui::BeginDisabled(ls.save_dir.empty());
    if (ImGui::Button("Use default")) on_pick(hwfs::default_save_dir(), in_game, changed);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open save folder")) {
        std::error_code ec;
        std::filesystem::create_directories(to_wide(chosen), ec);
        ShellExecuteW(nullptr, L"open", to_wide(chosen).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (!g_status.empty()) ImGui::TextColored(g_status_color, "%s", g_status.c_str());

    if (!g_ask.empty() && !ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
    const ImVec2 view = ImGui::GetMainViewport()->Size;
    const float s = ui_scale();
    ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, view.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(view.x * 0.9f, 640.0f * s), 0));
    if (ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        const std::string from = hwfs::active_save_dir();
        const bool there = g_ask_there;
        ImGui::TextWrapped("Your saves are in %s.", from.c_str());
        ImGui::TextWrapped("Copy them to %s?", g_ask.c_str());
        if (there)
            ImGui::TextColored(kPending, "That folder already has saves. Copying replaces them, after backing them up beside it.");
        ImGui::TextDisabled("Nothing in the current folder is moved or deleted.");
        if (in_game) ImGui::TextDisabled("The copy is made when you next start the game, so it includes this session's saves.");
        ImGui::Spacing();
        const float bw = 220.0f * s;
        bool close = false;
        if (ImGui::Button("Copy my saves", ImVec2(bw, 0))) {
            apply(g_ask, true, in_game, changed);
            close = true;
        }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button(there ? "Use the saves there" : "Start with no saves", ImVec2(bw, 0))) {
            apply(g_ask, false, in_game, changed);
            close = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(bw * 0.5f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            close = true;
        if (close) {
            g_ask.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace overlay
