// Runs steam::add from tools/setup_ui/steam_shortcut.h on files, so test_steam_shortcut.py can compare the
// Python port byte for byte. Synthetic files only.
// Usage: steam_shortcut_driver <in.vdf or -> <out.vdf> <name> <exe> <start_dir> <icon>
#include "../setup_ui/steam_shortcut.h"

#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc != 7) return 2;
    std::string existing;
    if (std::string(argv[1]) != "-") {
        std::ifstream in(argv[1], std::ios::binary);
        existing.assign(std::istreambuf_iterator<char>(in), {});
    }
    const steam::Shortcut s{argv[3], argv[4], argv[5], argv[6]};
    std::string out;
    uint32_t id = 0;
    const auto added = steam::add(existing, s, out, &id);
    const char* names[] = {"yes", "updated", "already-there", "unreadable"};
    std::ofstream(argv[2], std::ios::binary).write(out.data(), (std::streamsize)out.size());
    std::cout << names[(int)added] << ' ' << id << '\n';
    return 0;
}
