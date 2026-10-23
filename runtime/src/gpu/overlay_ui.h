// The in-game menu's look and some of its settings widgets, shared with other screens drawn through the
// overlay (the prelaunch screen). Only valid inside an overlay frame.
#pragma once
#include "gpu/vk_loader.h"
#include "hid/host_input.h"

#include "imgui.h"

namespace overlay {

extern const ImVec4 kGold;
ImFont* bold_font();  // null = the regular font
float ui_scale();     // style scale of the current frame (window height and the menu size setting)

void section(const char* label);                     // bold gold heading on a rule
void key_hint(const char* key, const char* action);  // footer hint: key cap, then what it does
void help(const char* text);                         // "(?)" tooltip for the widget just drawn

// Host buttons that confirm and go back in the menus: the ones bound to Switch A and B (see overlay.cpp).
struct MenuButtons {
    int accept, back;  // hid::HostButton
};
MenuButtons menu_buttons(const hid::RawPad& pad);
const char* accept_button_name();  // as printed on the controller in hand

// Settings widgets, bound to settings::; `changed` is set when the user changes one (the caller saves).
void opt_interpolation(bool& changed);
void opt_borderless(bool& changed);
void opt_window_size(VkExtent2D extent, bool& changed);
void opt_internal_res(bool& changed);
void opt_volume(bool& changed);
// Save folder (save_folder_ui.cpp). `in_game`: the game has started, so a new folder applies on restart.
void opt_save_folder(bool in_game, bool& changed);

}  // namespace overlay
