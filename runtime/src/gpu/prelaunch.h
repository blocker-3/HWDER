// Prelaunch screen: drawn in the game window before the game boots. Checks the game file and the keys,
// lets the user pick them, and offers a few quick settings.
#pragma once
#include <string>

namespace prelaunch {

// Shows the screen until Play (returns true; the NSP is then initialized) or Quit (false). Needs display::init().
// HWDER_PRELAUNCH_FRAMES=<n> quits after n frames (screenshots in automated runs).
bool run(const std::string& exe_dir);

}  // namespace prelaunch
