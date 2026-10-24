# Changelog

## v0.2.1

- **Steady 120 fps in big battles.** On busy stages the GPU command thread was the limit: 120 fps mode fell to about 73 fps on a ~1500-draw battle. It now holds 120 there, and native mode holds an exact 60.
- **Performance cores first.** On hybrid CPUs (Intel P-cores and E-cores) the game prefers the performance cores. Set `HWDER_ALL_CORES=1` to let it use every core.
- **Game update 1.0.1.** Setup has a Game update field for the 1.0.1 update (`.nsp` or `.nsz`); the game is then built from the updated code and reads the update's data. The base game alone still works.
- **Game memory option.** Settings > Gameplay > Game memory offers the Switch's 3.2 GB, 4 GB or 6 GB (applies at the next start). The game is also told a Switch-like amount of used memory.
- **Setup: desktop shortcut and Add to Steam** on the main screen. Add to Steam closes and reopens Steam itself, names the entry *Hyrule Warriors: Definitive Edition* and downloads box art (the game's key art from Nintendo's store; with your own free SteamGridDB API key under Advanced, community art and a logo). No artwork ships with HWDER.
- **Save picker.** The save option now asks for the save file (`zmha.bin`) instead of a folder; a save from elsewhere is imported after a question, with a backup.
- **Clear startup errors.** Without an AVX2 CPU or a Vulkan 1.2 driver the game now says so in a message box instead of closing silently; Setup checks AVX2 too.
- **Ultrawide.** Menus and loading screens no longer spill stretched parchment into the side bands.
- **Fix: pressing + at startup no longer crashes.** + opens the game's electronic manual, which HWDER does not show; it now closes at once instead of crashing on the title screen.

## v0.2.0 (2026-10-23)

First release of this fork: guided Windows setup, local two-player co-op, the in-game HWDER menu, the optional 120 fps mode, controller and graphics options, and many rendering fixes. See the v0.2.0 release page for the full notes.
