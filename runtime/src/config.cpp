// Runtime configuration (paths to the user's game data).
#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <string>

#include "hwder/runtime.h"

static std::string g_romfs;

void hw_config_load(const std::string& dir) {
    // UTF-8 like `dir`; HWDER_ROMFS is read as wide so non-ANSI paths survive.
#ifdef _WIN32
    const wchar_t* env = _wgetenv(L"HWDER_ROMFS");
    if (env) {
        int n = WideCharToMultiByte(CP_UTF8, 0, env, -1, nullptr, 0, nullptr, nullptr);
        g_romfs.assign(n > 0 ? n - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, env, -1, g_romfs.data(), n, nullptr, nullptr);
    } else {
        g_romfs = dir + "\\romfs";
    }
#else
    const char* env = std::getenv("HWDER_ROMFS");
    g_romfs = env ? env : dir + "/romfs";
#endif
    hw_log("romfs: %s", g_romfs.c_str());
}

const std::string& hw_romfs_path() { return g_romfs; }
