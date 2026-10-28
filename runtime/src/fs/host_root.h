// Root of a host-backed guest filesystem (save data, RomFS folder, sdmc).
#pragma once
#include <filesystem>

namespace hwfs {

// Windows save roots are long paths (\\?\C:\...). Never lexically_normal() them: LLVM's libc++ (the Setup
// toolchain) turns \\?\C:\Users\... into the drive-relative C:Users\..., which Windows resolves against the
// game's folder - the game then found no save and started a new one there (Max, 2026-10-28). Windows only drops
// trailing separators, as v0.2.2 did; elsewhere the path is normalized.
inline std::filesystem::path host_root(const std::filesystem::path& root) {
#ifdef _WIN32
    auto text = root.native();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return std::filesystem::path(text);
#else
    return root.lexically_normal();
#endif
}

}  // namespace hwfs
