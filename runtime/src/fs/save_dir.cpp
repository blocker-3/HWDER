// Save folder location and the copy offered when it changes (save_dir.h).
#ifdef _WIN32
#include <windows.h>
#endif

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

#ifdef _WIN32
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
#else
HostPath normalize(const std::string& dir) {
    std::error_code error;
    const auto path = fsys::absolute(fsys::u8path(dir), error);
    if (error) return fsys::u8path(dir).lexically_normal();
    const auto resolved = fsys::weakly_canonical(path, error);
    return error ? path.lexically_normal() : resolved;
}
HostPath long_path(const std::string& dir) { return normalize(dir); }
bool inside(const std::string& child, const std::string& parent) {
    const auto relative = normalize(child).lexically_relative(normalize(parent));
    return !relative.empty() && relative != "." && *relative.begin() != "..";
}
#endif
std::string display_path(const HostPath& path) {
#ifdef _WIN32
    return wide_to_utf8(strip_prefix(path.native()));
#else
    return path_utf8(path);
#endif
}

std::mutex g_m;
HostPath g_active;  // set when the game first opens a save

}  // namespace

std::string default_save_dir() { return display_path(exe_dir() / "save"); }

std::string chosen_save_dir() {
    const std::string& s = settings::launch().save_dir;
    if (s.empty()) return default_save_dir();
    std::string root, report;
    // Also repair the interpretation of a leaf already saved by an older picker.
    // Do not rewrite preferences or move/copy any save files here.
    return resolve_save_folder(s, root, report) ? root : display_path(HostPath(normalize(s)));
}

bool resolve_save_folder(const std::string& selected, std::string& root, std::string& report) {
    const auto result = inspect_save_folder(fsys::path(normalize(selected)));
    root = result.error ? "" : display_path(result.root);
    report = result.error ? result.error : result.account_leaf
        ? "Recognized the account save folder. The save root used by the game is " + root + "." : "";
    return !result.error;
}

bool resolve_save_file(const std::string& file, std::string& root, std::string& import_from, std::string& report) {
    const auto result = inspect_save_file(fsys::path(normalize(file)));
    root = result.root.empty() ? "" : display_path(result.root);
    import_from = result.import_from.empty() ? "" : display_path(result.import_from);
    report = result.error ? result.error : "";
    return !result.error;
}

bool is_save_file_folder(const std::string& dir) {
    std::error_code ec;
    return fsys::is_regular_file(fsys::path(long_path(dir)) / "zmha.bin", ec);
}

HostPath active_save_root() {
    std::lock_guard<std::mutex> l(g_m);
    if (g_active.empty()) {
        g_active = long_path(chosen_save_dir());
        hw_log("fs: save folder %s", display_path(g_active).c_str());
    }
    return g_active;
}

std::string active_save_dir() {
    std::lock_guard<std::mutex> l(g_m);
    return g_active.empty() ? chosen_save_dir() : display_path(g_active);
}

bool same_dir(const std::string& a, const std::string& b) {
#ifdef _WIN32
    const auto x = normalize(a), y = normalize(b);
    return CompareStringOrdinal(x.c_str(), (int)x.size(), y.c_str(), (int)y.size(), TRUE) == CSTR_EQUAL;
#else
    return normalize(a) == normalize(b);
#endif
}

bool has_files(const std::string& dir) {
    std::error_code ec;
    for (fsys::recursive_directory_iterator it(long_path(dir), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) return true;
    return false;
}

namespace {

// copy_saves and import_save: copy `files` (relative to from, or every file under it when empty) into to/sub,
// backing up what they replace first into <to>.bak-<date>-<time>/sub.
bool copy_files(const std::string& from, const std::string& to, const fsys::path& sub, std::vector<fsys::path> files,
                std::string& report) {
    if (same_dir(from, to)) {
        report = "The old and new save folders are the same; nothing to copy.";
        return true;
    }
    if (inside(to, from)) {
        report = "Can't copy: the new folder is inside the old save folder.";
        return false;
    }
    const fsys::path src = long_path(from), root = long_path(to), dst = root / sub;
    std::error_code ec;
    if (files.empty()) {  // relative to src
        for (fsys::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
#ifndef _WIN32
            if (it->is_symlink(ec)) {
                report = "Can't copy: the save folder contains a symbolic link.";
                return false;
            }
#endif
            if (it->is_regular_file(ec)) files.push_back(it->path().lexically_relative(src));
        }
    }
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
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    strftime(stamp, sizeof(stamp), ".bak-%Y%m%d-%H%M%S", &lt);
    fsys::path backup_root = root;
    if (root == root.root_path()) backup_root /= std::string("hwder-save") + stamp;  // drive root
    else backup_root += fsys::u8path(stamp);                                        // beside it
    const fsys::path backup = backup_root / sub;
#ifndef _WIN32
    // Validate all targets before any copy, including paths through existing
    // directories. A static symlink must not redirect a save write outside to.
    const auto destination_root = fsys::weakly_canonical(root, ec);
    if (ec) { report = "Can't resolve the new save folder: " + ec.message(); return false; }
    for (const auto& rel : files) {
        const auto resolved = fsys::weakly_canonical(dst / rel, ec);
        const auto relative = resolved.lexically_relative(destination_root);
        if (ec || relative.empty() || *relative.begin() == ".." || fsys::is_symlink(dst / rel, ec)) {
            report = "Can't copy: a destination save path resolves outside the save folder or is a symbolic link.";
            return false;
        }
        // A missing destination is expected and may set ENOENT in is_symlink.
        ec.clear();
    }
#endif
    size_t backed_up = 0;
    for (const fsys::path& rel : files) {
        if (!fsys::exists(dst / rel, ec)) continue;
        fsys::create_directories((backup / rel).parent_path(), ec);
        if (!ec) fsys::copy_file(dst / rel, backup / rel, fsys::copy_options::overwrite_existing, ec);
        if (ec) {
            report = "Nothing was copied: backing up " + display_path(dst / rel) + " failed (" + ec.message() + ").";
            return false;
        }
        backed_up++;
    }
    for (const fsys::path& rel : files) {
        fsys::create_directories((dst / rel).parent_path(), ec);
        if (!ec) fsys::copy_file(src / rel, dst / rel, fsys::copy_options::overwrite_existing, ec);
        if (ec) {
            report = "Copying " + path_utf8(rel) + " failed (" + ec.message() + ")." +
                     (backed_up ? " The files it replaced are in " + display_path(backup) + "." : "");
            return false;
        }
    }
    report = "Copied " + std::to_string(files.size()) + (files.size() == 1 ? " file" : " files") + " from " + from + ".";
    if (backed_up)
        report += " The " + std::to_string(backed_up) + " it replaced " + (backed_up == 1 ? "is" : "are") + " backed up in " +
                  display_path(backup) + ".";
    return true;
}

}  // namespace

bool copy_saves(const std::string& from, const std::string& to, std::string& report) {
    return copy_files(from, to, {}, {}, report);
}

bool import_save(const std::string& from, const std::string& root, std::string& report) {
    const fsys::path sub = fsys::path("0100AE00096EA000") / "48574445525F55530000000000000001";
    const std::string leaf = display_path(HostPath(normalize(root)) / sub);
    if (inside(leaf, from) || inside(from, leaf) || same_dir(from, leaf)) {
        report = "That save is already in the save folder.";
        return false;
    }
    return copy_files(from, root, sub, {"zmha.bin", "zmhasd.bin"}, report);
}

void run_pending_save_copy() {
    settings::Launch& l = settings::launch();
    if (l.save_copy_from.empty()) return;
    const std::string from = l.save_copy_from, to = chosen_save_dir();
    std::string report;
    const bool ok = is_save_file_folder(from) ? import_save(from, to, report) : copy_saves(from, to, report);
    hw_log("fs: save copy to %s %s: %s", to.c_str(), ok ? "done" : "FAILED", report.c_str());
    l.save_copy_from.clear();
    // On failure keep playing from the old folder rather than a new one with no (or half-copied) saves.
    // (An import never changed the folder.)
    if (!ok && !is_save_file_folder(from)) l.save_dir = same_dir(from, default_save_dir()) ? "" : from;
    settings::save();
#ifdef _WIN32
    if (!ok && !debug::flag(debug::Mute))
        MessageBoxW(nullptr, utf8_to_wide("Your saves could not be copied to the new save folder.\n\n" + report +
                                          "\n\nThe game keeps using " + from + ". Choose the folder again in the menu to retry.")
                                 .c_str(),
                    L"HWDER", MB_ICONWARNING | MB_OK);
#endif
}

}  // namespace hwfs
