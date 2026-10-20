// Where save data lives on the host: <exe dir>\save by default, or the folder picked on the startup screen or in
// the in-game menu (settings::launch().save_dir). Paths here are UTF-8 without the \\?\ prefix.
#pragma once
#include <string>

namespace hwfs {

std::string default_save_dir();  // <exe dir>\save
std::string chosen_save_dir();   // the setting, else the default
// Accept a save root or this game's recognized account leaf; reads file metadata only.
// On failure root is empty and report explains why the picker must keep the previous setting.
bool resolve_save_folder(const std::string& selected, std::string& root, std::string& report);
// The folder the running game uses. Fixed the first time the game opens its save; until then it follows the setting.
std::string active_save_dir();
std::wstring active_save_root();  // the same, with the \\?\ prefix, for the host filesystem
bool same_dir(const std::string& a, const std::string& b);
bool has_files(const std::string& dir);  // any file anywhere under dir

// Copies every file under `from` into `to`, keeping the layout. Files in `to` that would be overwritten are first
// copied to a backup folder beside it (<to>.bak-<date>-<time>). Nothing in `from` is changed. `report` says what
// happened in plain words, on failure too.
bool copy_saves(const std::string& from, const std::string& to, std::string& report);

// A copy asked for in the in-game menu runs at the next launch, before the game opens its save, so it picks up
// everything saved until then (settings::launch().save_copy_from). Call once at startup, after settings::load().
void run_pending_save_copy();

}  // namespace hwfs
