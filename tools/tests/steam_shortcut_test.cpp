// Synthetic bytes only: never reads or writes a real Steam folder.
#include "../setup_ui/steam_shortcut.h"

#include <iostream>
#include <stdexcept>

int main() {
    int checks = 0;
    auto require = [&](bool value, const char* name) {
        if (!value) throw std::runtime_error(name);
        ++checks;
    };
    try {
        const steam::Shortcut game{"HWDER", "C:\\Games\\HWDER\\play\\hwder.exe", "C:\\Games\\HWDER\\play", "C:\\Games\\HWDER\\play\\hwder.exe"};
        std::string first, second, again;
        require(steam::add("", game, first) == steam::Added::Yes, "new file created");
        require(first.compare(0, 11, std::string("\0shortcuts\0", 11)) == 0, "file starts with the shortcuts map");
        require(first.substr(first.size() - 10) == std::string("\0tags\0\x08\x08\x08\x08", 10), "tags, entry, list and file closed");
        require(first.find(std::string("\0" "0" "\0", 3)) == 11, "first entry is named 0");
        require(first.find("\"C:\\Games\\HWDER\\play\\hwder.exe\"") != std::string::npos, "exe is quoted");
        require(steam::add(first, game, again) == steam::Added::AlreadyThere, "same exe not added twice");
        steam::Shortcut upper = game;
        upper.exe = "c:\\games\\hwder\\PLAY\\HWDER.EXE";
        require(steam::add(first, upper, again) == steam::Added::AlreadyThere, "exe match ignores case");
        steam::Shortcut other = game;
        other.name = "Other";
        other.exe = "D:\\Other\\other.exe";
        require(steam::add(first, other, second) == steam::Added::Yes, "second shortcut added");
        require(second.find(std::string("\0" "1" "\0", 3)) != std::string::npos, "second entry is named 1");
        require(second.compare(0, first.size() - 2, first, 0, first.size() - 2) == 0, "existing entries kept byte for byte");
        require(steam::add(second, game, again) == steam::Added::AlreadyThere, "first still found after the second");
        require(steam::add("not a vdf", game, again) == steam::Added::Unreadable, "foreign bytes refused");
        require(steam::add(first.substr(0, first.size() - 5), game, again) == steam::Added::Unreadable, "truncated file refused");
        // Known value: CRC-32 of "\"C:\\a.exe\"" + "A" with the top bit set, computed independently (zlib.crc32).
        require(steam::shortcut_appid("\"C:\\a.exe\"", "A") == 0xFE27FA28u, "appid matches zlib crc32");
        std::cout << "steam shortcut: " << checks << " checks PASS; synthetic bytes only\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "steam shortcut FAILED: " << e.what() << '\n';
        return 1;
    }
}
