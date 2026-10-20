// Synthetic metadata only. Never reads or copies actual user save contents.
#include "fs/save_folder_selection.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const fs::path base = fs::absolute(argv[1]) / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(base);
    const fs::path title = "0100AE00096EA000", user = "48574445525F55530000000000000001";
    int checks = 0;
    auto require = [&](bool value, const char* name) {
        if (!value) throw std::runtime_error(name);
        ++checks;
    };
    auto files = [](const fs::path& dir, bool pair = true) {
        fs::create_directories(dir);
        std::ofstream(dir / "zmha.bin") << "synthetic-main";
        if (pair) std::ofstream(dir / "zmhasd.bin") << "synthetic-side";
    };
    try {
        const auto root = base / "save", leaf = root / title / user;
        files(leaf);
        auto r = hwfs::inspect_save_folder(root);
        require(!r.error && !r.account_leaf && r.root == root, "existing root preserved");
        r = hwfs::inspect_save_folder(leaf);
        require(!r.error && r.account_leaf && r.root == root, "canonical leaf normalized to root");
        // The production fs.cpp appends this suffix. This reproduces the actual older-picker bug.
        require(leaf / title / user != leaf, "old verbatim selection nests save hierarchy");
        require(r.root / title / user == leaf, "resolved runtime account path reaches original leaf");
        require(!fs::exists(leaf / title), "inspection created no nested save directories");
        files(leaf / title / user);  // Fresh wrongly nested saves from a previous run.
        require(hwfs::inspect_save_folder(leaf).root == root, "original leaf wins over accidental nested fresh save");
        require(hwfs::inspect_save_folder(leaf / "").root == root, "trailing separator handled");
        require(hwfs::inspect_save_folder(root / title).error, "title-only choice refused");
        const auto partial = base / "partial" / title / user;
        files(partial, false);
        require(hwfs::inspect_save_folder(partial).error, "partial account leaf refused");
        require(hwfs::inspect_save_folder(base / "partial").error, "partial save root refused");
        const auto wrong_title = base / "wrong-title" / "0100000000000000" / user;
        files(wrong_title);
        require(hwfs::inspect_save_folder(wrong_title).error, "wrong title leaf refused");
        const auto wrong_user = base / "wrong-user" / title / "00000000000000000000000000000001";
        files(wrong_user);
        require(hwfs::inspect_save_folder(wrong_user).error, "wrong user leaf refused");
        const auto flat = base / "arbitrary-flat";
        files(flat);
        require(hwfs::inspect_save_folder(flat).error, "unsupported flat folder refused");
        files(flat / title / user);
        require(hwfs::inspect_save_folder(flat).error, "ambiguous flat and nested saves refused");
        const auto empty = base / "new-root";
        require(!hwfs::inspect_save_folder(empty).error && !fs::exists(empty), "new root accepted without writes");
        const auto other = base / "other-save-types";
        fs::create_directories(other / "system" / "1234");
        require(hwfs::inspect_save_folder(other).root == other, "system/device hierarchy unchanged");
        const auto lowercase = base / "lower" / "0100ae00096ea000" / "48574445525f55530000000000000001";
        files(lowercase);
        require(hwfs::inspect_save_folder(lowercase).root == base / "lower", "Windows hex folder case accepted");
        require(fs::file_size(leaf / "zmha.bin") == 14 && fs::file_size(leaf / "zmhasd.bin") == 14,
                "source synthetic files unchanged");
        std::cout << "save folder selection: " << checks << " checks PASS; synthetic metadata only\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "save folder selection FAILED: " << e.what() << '\n';
        return 1;
    }
}
