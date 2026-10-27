// Mods (LayeredFS): folders of game files laid over the game's RomFS when the game starts.
//
// Each folder inside the mods folder is one mod. It holds the RomFS tree itself (mods/<name>/...),
// a romfs/ folder, or a Switch mod's Atmosphere layout (atmosphere/contents/0100AE00096EA000/romfs),
// so mods made for the console work unchanged. Mods apply in name order, later ones winning; a file
// replaces the game file at the same path (a case-only difference still matches) or is added.
//
// The mods folder is <exe dir>\mods on Windows and $XDG_DATA_HOME/HWDER/mods (else
// ~/.local/share/HWDER/mods) on Linux, where Setup rebuilds the exe folder under ~/.cache.
// settings::launch().mods_dir overrides it and HWDER_MODS overrides both; HWDER_NO_MODS=1 loads none.
// A mod is off when it is switched off in the in-game menu (settings::launch().mods_off) or its
// folder name starts with '_' or '.'. Changes take effect at the next launch. Paths are UTF-8.
#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace hwfs::mods {

struct Mod {
    std::string name;            // folder name inside the mods folder
    std::filesystem::path root;  // the RomFS tree inside it (native host path)
    std::string layout;          // where the tree was found: "", "romfs" or "atmosphere"
    bool off_by_name = false;    // '_' or '.' prefix
    bool off_in_menu = false;    // listed in settings::launch().mods_off
    bool enabled() const { return !off_by_name && !off_in_menu; }
};

std::string default_folder();
std::string folder();  // the folder in use: HWDER_MODS, else the setting, else the default
bool disabled_by_env();  // HWDER_NO_MODS

std::vector<Mod> scan();  // what the folder holds now, in load order

// The enabled mods this run lays over the RomFS. Fixed on first use (the game's first RomFS access).
const std::vector<Mod>& loaded();

// What each loaded mod changed, filled in once the RomFS image is built.
struct Stats {
    bool built = false;
    size_t replaced = 0, added = 0;
};
void set_stats(size_t loaded_index, size_t replaced, size_t added);
Stats stats(size_t loaded_index);

bool ensure_folder();  // creates the mods folder if missing, so "Open mods folder" has somewhere to go

}  // namespace hwfs::mods
