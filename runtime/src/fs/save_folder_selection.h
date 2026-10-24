// Read-only recognition of HWDER's account-save folder. No save contents are opened.
#pragma once
#include <filesystem>
#include <string>

namespace hwfs {

struct SaveFolderSelection {
    std::filesystem::path root;
    const char* error = nullptr;
    bool account_leaf = false;
};

inline SaveFolderSelection inspect_save_folder(std::filesystem::path selected) {
    namespace fs = std::filesystem;
    constexpr const char* title = "0100AE00096EA000";
    constexpr const char* user = "48574445525F55530000000000000001";
    selected = selected.lexically_normal();
    while (selected.filename().empty() && selected.parent_path() != selected) selected = selected.parent_path();
    auto named = [](const fs::path& path, const char* expected) {
        auto name = path.filename().u8string();
        size_t i = 0;
        for (; i < name.size() && expected[i]; ++i) {
            char c = static_cast<char>(name[i]);
            if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
            if (c != expected[i]) return false;
        }
        return i == name.size() && expected[i] == 0;
    };
    auto regular = [](const fs::path& path) {
        std::error_code ec;
        return fs::is_regular_file(path, ec);
    };
    const bool main = regular(selected / "zmha.bin"), side = regular(selected / "zmhasd.bin");
    const bool canonical_leaf = named(selected, user) && named(selected.parent_path(), title);
    if (canonical_leaf && main && side) return {selected.parent_path().parent_path(), nullptr, true};
    if (main || side || canonical_leaf)
        return {{}, "Choose the save root containing 0100AE00096EA000, or its account folder containing both "
                    "zmha.bin and zmhasd.bin. A different title/user or partial flat save folder cannot be used."};
    // Selecting the title folder would otherwise add a second title level.
    if (named(selected, title))
        return {{}, "This is the title folder. Choose its parent save folder, or the account folder inside it."};
    const auto account = selected / title / user;
    const bool root_main = regular(account / "zmha.bin"), root_side = regular(account / "zmhasd.bin");
    if (root_main != root_side)
        return {{}, "The selected save root contains an incomplete account save. Both zmha.bin and zmhasd.bin are required."};
    return {selected};  // Existing root or a new empty root: preserve standard title/user routing.
}

// The save picker asks for the save file itself (zmha.bin), so there is no folder level to get wrong.
// In an HWDER/console layout (<root>/0100AE00096EA000/<user>/zmha.bin + zmhasd.bin) the game switches to that
// root and uses the save where it is. Anywhere else (a save exported from an emulator or a backup) the pair
// is imported: copied into the current save folder, which keeps its layout and gets no new folders.
struct SaveFileSelection {
    std::filesystem::path root;         // use this save root (console layout)
    std::filesystem::path import_from;  // or import zmha.bin + zmhasd.bin from this folder
    const char* error = nullptr;
};

inline SaveFileSelection inspect_save_file(std::filesystem::path file) {
    namespace fs = std::filesystem;
    file = file.lexically_normal();
    auto name = file.filename().u8string();
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (name != u8"zmha.bin") return {{}, {}, "Choose the save file zmha.bin."};
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return {{}, {}, "That save file does not exist."};
    const fs::path folder = file.parent_path();
    if (!fs::is_regular_file(folder / "zmhasd.bin", ec))
        return {{}, {}, "zmhasd.bin is missing next to that zmha.bin. A save needs both files."};
    const SaveFolderSelection leaf = inspect_save_folder(folder);
    if (!leaf.error && leaf.account_leaf) return {leaf.root, {}, nullptr};
    return {{}, folder, nullptr};
}

}  // namespace hwfs
