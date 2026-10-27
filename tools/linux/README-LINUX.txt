HWDER for Linux - Hyrule Warriors: Definitive Edition, native PC version
=========================================================================

HWDER is built on your own PC from your own game. You need:
  - your dump of the game (Hyrule Warriors: Definitive Edition, .nsp)
  - your own prod.keys
  - optional: your dump of game update 1.0.1 (.nsp or .nsz)
  - an x86-64 CPU with AVX2 and a Vulkan 1.3 GPU driver (Steam Deck, ROG Ally, most PCs since 2015)
  - about 8 GB of free space and an internet connection the first time (Setup downloads its build tools,
    about 200 MB, and checks each file against its SHA-256)

Made for Bazzite and SteamOS; Fedora, Ubuntu (22.04 or newer) and Arch should work too. Nothing has to be
installed on the system and no container is used.

Install
-------
1. Unpack this folder anywhere (for example in Downloads).
2. Double-click "hwder-setup" (in a file manager choose "Execute" / "Run"), or run ./hwder-setup in a terminal.
   The HWDER Setup window opens. It works with a mouse, touch screen or controller.
3. Choose your game .nsp, prod.keys and, if you have it, the update. Then "Prepare the game".
   This takes about 15 minutes on a ROG Ally (the game's code is compiled for your PC).
4. "Add to Steam" puts the game in your Steam library (and Game Mode). Steam has to close for that;
   Setup closes it and starts it again (in Game Mode Steam restarts by itself).
   "Play" starts the game; it is also in your application menu.

Setup copies itself to ~/.local/share/HWDER/app, so you can delete this folder afterwards; "HWDER Setup" in the
application menu opens it again (to update, choose other files or rebuild).

Without a display, or with --cli, the same steps run as questions in the terminal:  ./hwder-setup --cli
Other commands: ./hwder-setup status | steam | desktop --desktop-icon | play | uninstall   (--help for details)

Where things are
----------------
  ~/.local/share/HWDER/save        your saves
  ~/.local/share/HWDER/build/play  the game (hwder), its settings (hwder_settings.ini), shader caches, hwder.log
  ~/.local/share/HWDER/toolchain   the build tools
Your game, keys and update files stay where they are and are never copied. Uninstall (keeps saves):
  ~/.local/share/HWDER/app/hwder-setup uninstall

Licences
--------
The project source and third-party notices are in this folder (LICENSE, third_party/). The game is linked on your PC
with prebuilt SDL3, FFmpeg (H.264 decoder only, LGPL; its source is in linux-deps/sources) and OpenSSL libcrypto;
see linux-deps/licenses/README.txt. Setup's build tools (Zig/Clang, CMake, Ninja, Python and its modules) are
downloaded from their official sites; tools/linux/toolchain.json lists them with their licences.
