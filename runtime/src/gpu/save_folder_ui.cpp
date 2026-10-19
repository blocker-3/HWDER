// The save option, shared by the startup screen and the in-game menu (overlay_ui.h). The picker asks for the save
// file itself (zmha.bin; hwfs::inspect_save_file): a save in the console layout is used where it is (the save folder
// switches to its root), any other is imported into the current save folder. Nothing is moved or deleted, and
// whatever an import would overwrite is backed up first (hwfs::import_save). On the startup screen an import runs at
// once; in the game it runs at the next launch, so the running game cannot save over it.
#include "gpu/overlay_ui.h"

#include "fs/save_dir.h"
#include "gpu/display.h"
#include "gpu/settings.h"
#include "hwder/runtime.h"

#ifdef _WIN32
#include <shellapi.h>
#include <shobjidl.h>
#else
#include <SDL3/SDL.h>
#include <condition_variable>
#include <unistd.h>
#endif

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
const char* kPopup = "Import this save?";

std::mutex g_m;
bool g_picking = false;  // the file dialog is open (on its own thread, so the frame keeps drawing)
bool g_picked = false;
std::string g_pick;      // its result, "" = cancelled

std::string g_ask;         // folder whose save waits on the import question
bool g_ask_there = false;  // the save folder already has a save the import would replace
std::string g_status;
ImVec4 g_status_color = kOk;

#ifdef _WIN32
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
        dlg->SetTitle(L"Choose your save file (zmha.bin)");
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
        const COMDLG_FILTERSPEC filter[] = {{L"Hyrule Warriors save (zmha.bin)", L"zmha.bin"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, filter);
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
        hw_log("save folder: file dialog unavailable");
    }
    if (SUCCEEDED(co)) CoUninitialize();
    std::lock_guard<std::mutex> l(g_m);
    g_pick = out;
    g_picked = true;
    g_picking = false;
}

#else
void pick_thread(std::string start) {
    struct Request {
        std::mutex mutex;
        std::condition_variable wake;
        std::string result;
        bool complete = false;
    } request;
    display::on_main_thread([&] {
        SDL_ShowOpenFolderDialog([](void* opaque, const char* const* files, int) {
            auto& result = *static_cast<Request*>(opaque);
            { std::lock_guard lock(result.mutex);
              if (files && files[0]) result.result = files[0];
              result.complete = true;
              result.wake.notify_one(); }
        }, &request, display::window(), start.c_str(), false);
    });
    std::unique_lock lock(request.mutex);
    request.wake.wait(lock, [&] { return request.complete; });
    std::lock_guard guard(g_m);
    g_pick = request.result; g_picked = true; g_picking = false;
}
#endif

bool writable(const std::string& dir) {
    std::error_code ec;
#ifdef _WIN32
    std::filesystem::create_directories(to_wide(dir), ec);
    HANDLE h = CreateFileW(to_wide(dir + "\\hwder-write-test.tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    if (ec) return false;
    std::string candidate = (std::filesystem::path(dir) / ".hwder-write-test-XXXXXX").string();
    const int file = mkstemp(candidate.data());
    if (file < 0) return false;
    close(file);
    return unlink(candidate.c_str()) == 0;
#endif
}

void set_status(const ImVec4& color, const std::string& text) {
    g_status_color = color;
    g_status = text;
    hw_log("save folder: %s", text.c_str());
}

// Switching to a save root asks nothing: the picked save is the one to play.
void switch_to(const std::string& dir, bool in_game, bool& changed) {
    if (!writable(dir)) {
        set_status(kError, "Can't write to " + dir + ". Copy the save somewhere you can write to and choose it there.");
        return;
    }
    if (hwfs::same_dir(dir, hwfs::chosen_save_dir())) {
        set_status(kOk, "That save is already the one in use.");
        return;
    }
    settings::Launch& l = settings::launch();
    const std::string from = hwfs::active_save_dir();
    l.save_dir = hwfs::same_dir(dir, hwfs::default_save_dir()) ? "" : dir;
    l.save_copy_from.clear();
    changed = true;
    set_status(kOk, "Save folder set to " + dir + "." + (in_game && !hwfs::same_dir(dir, from) ? " Restart the game to use it." : ""));
}

void import_from(const std::string& from, bool in_game, bool& changed) {
    const std::string to = hwfs::chosen_save_dir();
    if (!writable(to)) {
        set_status(kError, "Can't write to " + to + ". Choose Use default, then import the save again.");
        return;
    }
    if (in_game) {
        settings::launch().save_copy_from = from;
        changed = true;
        set_status(kOk, "The save in " + from + " will be imported into " + to +
                            " when you next start the game. Anything it replaces is backed up first.");
        return;
    }
    std::string report;
    set_status(hwfs::import_save(from, to, report) ? kOk : kError, report);
}

void on_pick(const std::string& file, bool in_game, bool& changed) {
    std::string root, from, report;
    if (!hwfs::resolve_save_file(file, root, from, report)) {
        set_status(kError, report + " Nothing was changed.");
        return;
    }
    if (!root.empty()) {
        switch_to(root, in_game, changed);
        return;
    }
    g_ask = from;  // the import question opens this frame
    g_ask_there = hwfs::is_save_file_folder(hwfs::chosen_save_dir() + "\\0100AE00096EA000\\48574445525F55530000000000000001");
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
        ImGui::TextColored(kPending, "Your save will be %s from %s at the next start.",
                           hwfs::is_save_file_folder(ls.save_copy_from) ? "imported" : "copied", ls.save_copy_from.c_str());

    bool picking;
    {
        std::lock_guard<std::mutex> l(g_m);
        picking = g_picking;
    }
    ImGui::BeginDisabled(picking);
    if (ImGui::Button("Choose save file...")) {
        std::lock_guard<std::mutex> l(g_m);
        g_picking = true;
        std::thread(pick_thread, chosen).detach();
    }
    ImGui::EndDisabled();
    help("Where your saves are kept. The default is the save folder next to hwder.exe.\n"
         "Choose your save file, zmha.bin (zmhasd.bin must be next to it). A save kept by HWDER, or copied from a\n"
         "console in the same layout, is used where it is; any other (an emulator export, a backup) is imported\n"
         "into the current save folder. Nothing is moved or deleted, and a save an import replaces is backed up first.");
    ImGui::SameLine();
    ImGui::BeginDisabled(ls.save_dir.empty());
    if (ImGui::Button("Use default")) switch_to(hwfs::default_save_dir(), in_game, changed);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Open save folder")) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(chosen), ec);
        display::open_path(chosen);
    }
    if (!g_status.empty()) ImGui::TextColored(g_status_color, "%s", g_status.c_str());

    if (!g_ask.empty() && !ImGui::IsPopupOpen(kPopup)) ImGui::OpenPopup(kPopup);
    const ImVec2 view = ImGui::GetMainViewport()->Size;
    const float s = ui_scale();
    ImGui::SetNextWindowPos(ImVec2(view.x * 0.5f, view.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(view.x * 0.9f, 640.0f * s), 0));
    if (ImGui::BeginPopupModal(kPopup, nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        const bool there = g_ask_there;
        ImGui::TextWrapped("Import the save in %s into %s?", g_ask.c_str(), hwfs::chosen_save_dir().c_str());
        if (there)
            ImGui::TextColored(kPending, "The save folder already has a save. Importing replaces it, after backing it up beside it.");
        ImGui::TextDisabled("Only zmha.bin and zmhasd.bin are copied. Nothing in %s is moved or deleted.", g_ask.c_str());
        if (in_game) ImGui::TextDisabled("The import happens when you next start the game.");
        ImGui::Spacing();
        const float bw = 220.0f * s;
        bool close = false;
        if (ImGui::Button("Import", ImVec2(bw, 0))) {
            import_from(g_ask, in_game, changed);
            close = true;
        }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        ImGui::SetItemDefaultFocus();
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
