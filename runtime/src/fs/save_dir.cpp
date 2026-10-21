// Save folder location and the copy offered when it changes (save_dir.h).
#include <windows.h>

#include "fs/save_dir.h"
#include "fs/save_folder_selection.h"

#include <ctime>
#include <filesystem>
#include <mutex>
#include <vector>

#include "fs_internal.h"
#include "gpu/settings.h"
#include "debug_flags.h"

namespace hwfs {

namespace {

namespace fsys = std::filesystem;

std::wstring strip_prefix(std::wstring w) {
    if (w.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + w.substr(8);
    if (w.rfind(L"\\\\?\\", 0) == 0) return w.substr(4);
    return w;
}

// Absolute, backslashes, no trailing slash (except a drive root such as C:\), no \\?\ prefix.
std::wstring normalize(const std::string& dir) {
    std::wstring w = strip_prefix(utf8_to_wide(dir));
    std::wstring full(32768, L'\0');
    DWORD n = GetFullPathNameW(w.c_str(), (DWORD)full.size(), full.data(), nullptr);
    if (n && n < full.size()) {
        full.resize(n);
        w = full;
    }
    while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
    return w;
}

std::wstring long_path(const std::string& dir) {
    std::wstring w = normalize(dir);
    if (w.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + w.substr(2);
    return L"\\\\?\\" + w;
}

bool inside(const std::string& child, const std::string& parent) {
    std::wstring c = normalize(child), p = normalize(parent);
    if (p.back() != L'\\') p += L'\\';
    return c.size() > p.size() && CompareStringOrdinal(c.c_str(), (int)p.size(), p.c_str(), (int)p.size(), TRUE) == CSTR_EQUAL;
}

std::mutex g_m;
std::wstring g_active;  // set when the game first opens a save

}  // namespace

std::string default_save_dir() { return wide_to_utf8(strip_prefix(exe_dir()) + L"\\save"); }

std::string chosen_save_dir() {
    const std::string& s = settings::launch().save_dir;
    if (s.empty()) return default_save_dir();
    std::string root, report;
    // Also repair the interpretation of a leaf already saved by an older picker.
    // Do not rewrite preferences or move/copy any save files here.
    return resolve_save_folder(s, root, report) ? root : wide_to_utf8(normalize(s));
}

bool resolve_save_folder(const std::string& selected, std::string& root, std::string& report) {
    const auto result = inspect_save_folder(fsys::path(normalize(selected)));
    root = result.error ? "" : wide_to_utf8(result.root.wstring());
    report = result.error ? result.error : result.account_leaf
        ? "Recognized the account save folder. The save root used by the game is " + root + "." : "";
    return !result.error;
}

std::wstring active_save_root() {
    std::lock_guard<std::mutex> l(g_m);
    if (g_active.empty()) {
        g_active = long_path(chosen_save_dir());
        hw_log("fs: save folder %s", wide_to_utf8(strip_prefix(g_active)).c_str());
    }
    return g_active;
}

std::string active_save_dir() {
    std::lock_guard<std::mutex> l(g_m);
    return g_active.empty() ? chosen_save_dir() : wide_to_utf8(strip_prefix(g_active));
}

bool same_dir(const std::string& a, const std::string& b) {
    std::wstring x = normalize(a), y = normalize(b);
    return CompareStringOrdinal(x.c_str(), (int)x.size(), y.c_str(), (int)y.size(), TRUE) == CSTR_EQUAL;
}

bool has_files(const std::string& dir) {
    std::error_code ec;
    for (fsys::recursive_directory_iterator it(long_path(dir), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) return true;
    return false;
}

bool copy_saves(const std::string& from, const std::string& to, std::string& report) {
    if (same_dir(from, to)) {
        report = "The old and new save folders are the same; nothing to copy.";
        return true;
    }
    if (inside(to, from)) {
        report = "Can't copy: the new folder is inside the old save folder.";
        return false;
    }
    const fsys::path src = long_path(from), dst = long_path(to);
    std::error_code ec;
    std::vector<fsys::path> files;  // relative to src
    for (fsys::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(fsys::relative(it->path(), src, ec));
    if (ec) {
        report = "Can't read " + from + ": " + ec.message();
        return false;
    }
    if (files.empty()) {
        report = "No saves in " + from + "; nothing to copy.";
        return true;
    }
    // Back up whatever the copy would overwrite, before writing anything.
    char stamp[32];
    time_t now = time(nullptr);
    tm lt;
    localtime_s(&lt, &now);
    strftime(stamp, sizeof(stamp), ".bak-%Y%m%d-%H%M%S", &lt);
    const std::wstring dw = normalize(to);
    fsys::path backup = dw.size() > 3 ? fsys::path(long_path(to) + utf8_to_wide(stamp))       // beside it
                                      : fsys::path(long_path(to) + L"\\hwder-save" + utf8_to_wide(stamp));  // drive root
    size_t backed_up = 0;
    for (const fsys::path& rel : files) {
        if (!fsys::exists(dst / rel, ec)) continue;
        fsys::create_directories((backup / rel).parent_path(), ec);
        if (!ec) fsys::copy_file(dst / rel, backup / rel, fsys::copy_options::overwrite_existing, ec);
        if (ec) {
            report = "Nothing was copied: backing up " + wide_to_utf8(strip_prefix((dst / rel).wstring())) + " failed (" +
                     ec.message() + ").";
            return false;
        }
        backed_up++;
    }
    for (const fsys::path& rel : files) {
        fsys::create_directories((dst / rel).parent_path(), ec);
        if (!ec) fsys::copy_file(src / rel, dst / rel, fsys::copy_options::overwrite_existing, ec);
        if (ec) {
            report = "Copying " + wide_to_utf8(rel.wstring()) + " failed (" + ec.message() + ")." +
                     (backed_up ? " The files it replaced are in " + wide_to_utf8(strip_prefix(backup.wstring())) + "." : "");
            return false;
        }
    }
    report = "Copied " + std::to_string(files.size()) + (files.size() == 1 ? " file" : " files") + " from " + from + ".";
    if (backed_up)
        report += " The " + std::to_string(backed_up) + " it replaced " + (backed_up == 1 ? "is" : "are") + " backed up in " +
                  wide_to_utf8(strip_prefix(backup.wstring())) + ".";
    return true;
}

void run_pending_save_copy() {
    settings::Launch& l = settings::launch();
    if (l.save_copy_from.empty()) return;
    const std::string from = l.save_copy_from, to = chosen_save_dir();
    std::string report;
    const bool ok = copy_saves(from, to, report);
    hw_log("fs: save copy to %s %s: %s", to.c_str(), ok ? "done" : "FAILED", report.c_str());
    l.save_copy_from.clear();
    // On failure keep playing from the old folder rather than a new one with no (or half-copied) saves.
    if (!ok) l.save_dir = same_dir(from, default_save_dir()) ? "" : from;
    settings::save();
    if (!ok && !debug::flag(debug::Mute))
        MessageBoxW(nullptr, utf8_to_wide("Your saves could not be copied to the new save folder.\n\n" + report +
                                          "\n\nThe game keeps using " + from + ". Choose the folder again in the menu to retry.")
                                 .c_str(),
                    L"HWDER", MB_ICONWARNING | MB_OK);
}

}  // namespace hwfs
