// Mods folder discovery and on/off state (mods.h). The overlay itself is in romfs.cpp.
#ifdef _WIN32
#include <windows.h>
#endif

#include "fs/mods.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include "debug_flags.h"
#include "fs_internal.h"
#include "gpu/settings.h"

namespace hwfs::mods {

namespace {

namespace fsys = std::filesystem;

constexpr const char* kTitleRomfs = "0100AE00096EA000";

// Native host path for a UTF-8 folder: absolute, no trailing separator, long-path form on Windows.
HostPath host_dir(const std::string& dir) {
#ifdef _WIN32
    std::wstring p = utf8_to_wide(dir);
    std::wstring full(32768, L'\0');
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)full.size(), full.data(), nullptr);
    if (n && n < full.size()) {
        full.resize(n);
        p = full;
    }
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    if (p.rfind(L"\\\\", 0) != 0) p = L"\\\\?\\" + p;
    return p;
#else
    std::error_code error;
    const auto absolute = fsys::absolute(fsys::u8path(dir), error);
    return error ? fsys::u8path(dir).lexically_normal() : absolute.lexically_normal();
#endif
}

std::string display(const HostPath& path) {
    std::string s = path_utf8(path);
#ifdef _WIN32
    if (s.rfind("\\\\?\\UNC\\", 0) == 0) return "\\\\" + s.substr(8);
    if (s.rfind("\\\\?\\", 0) == 0) return s.substr(4);
#endif
    return s;
}

bool is_dir(const HostPath& p) {
    std::error_code error;
    return fsys::is_directory(p, error);
}

std::mutex g_m;
std::vector<Stats> g_stats;

}  // namespace

std::string default_folder() {
#ifndef _WIN32
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && data[0] == '/')
        return display(fsys::u8path(data) / "HWDER" / "mods");
    if (const char* home = std::getenv("HOME"); home && home[0])
        return display(fsys::u8path(home) / ".local" / "share" / "HWDER" / "mods");
#endif
    return display(exe_dir() / "mods");
}

std::string folder() {
#ifdef _WIN32
    if (const wchar_t* e = _wgetenv(L"HWDER_MODS"); e && *e) return wide_to_utf8(e);  // debug::Mods, read as a wide path
#else
    if (const char* e = debug::str(debug::Mods); e && *e) return e;
#endif
    const std::string& s = settings::launch().mods_dir;
    return s.empty() ? default_folder() : s;
}

bool disabled_by_env() {
    return debug::flag(debug::NoMods);
}

std::vector<Mod> scan() {
    std::vector<Mod> mods;
    const HostPath base = host_dir(folder());
    std::error_code error;
    for (fsys::directory_iterator it(base, error), end; !error && it != end; it.increment(error)) {
#ifndef _WIN32
        if (it->is_symlink(error)) continue;
#endif
        if (!it->is_directory(error)) continue;
        Mod m;
        m.name = path_utf8(it->path().filename());
        m.off_by_name = m.name.empty() || m.name[0] == '_' || m.name[0] == '.';
        m.off_in_menu = settings::launch().mods_off.count(m.name) != 0;
        m.root = it->path();
        // A romfs folder, or a console mod's layout, inside the mod folder.
        const HostPath atmosphere = it->path() / "atmosphere" / "contents" / kTitleRomfs / "romfs";
        const HostPath contents = it->path() / "contents" / kTitleRomfs / "romfs";
        const HostPath title = it->path() / kTitleRomfs / "romfs";
        if (is_dir(it->path() / "romfs")) m.root = it->path() / "romfs", m.layout = "romfs";
        else if (is_dir(atmosphere)) m.root = atmosphere, m.layout = "atmosphere";
        else if (is_dir(contents)) m.root = contents, m.layout = "atmosphere";
        else if (is_dir(title)) m.root = title, m.layout = "atmosphere";
        mods.push_back(std::move(m));
    }
    std::sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) { return a.name < b.name; });
    return mods;
}

const std::vector<Mod>& loaded() {
    static const std::vector<Mod> mods = [] {
        std::vector<Mod> on;
        if (disabled_by_env()) {
            hw_log("mods: all off (HWDER_NO_MODS)");
            return on;
        }
        for (Mod& m : scan()) {
            if (!m.enabled()) {
                hw_log("mods: '%s' off (%s)", m.name.c_str(), m.off_by_name ? "folder name" : "menu");
                continue;
            }
            hw_log("mods: '%s' -> %s", m.name.c_str(), display(m.root).c_str());
            on.push_back(std::move(m));
        }
        if (on.empty()) hw_log("mods: none in %s", folder().c_str());
        return on;
    }();
    return mods;
}

void set_stats(size_t i, size_t replaced, size_t added) {
    std::lock_guard<std::mutex> lock(g_m);
    if (g_stats.size() <= i) g_stats.resize(i + 1);
    g_stats[i] = {true, replaced, added};
}

Stats stats(size_t i) {
    std::lock_guard<std::mutex> lock(g_m);
    return i < g_stats.size() ? g_stats[i] : Stats{};
}

bool ensure_folder() {
    std::error_code error;
    const HostPath dir = host_dir(folder());
    fsys::create_directories(dir, error);
    return is_dir(dir);
}

}  // namespace hwfs::mods
