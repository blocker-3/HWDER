# Changelog

## v0.3.2 (2026-10-30, pre-release)

- **Japanese release recognised** (issue #3). Setup accepts the Japanese game (title 01002AB007FD2000). It builds if its code matches a version HWDER knows; otherwise Setup names the build so it can be reported. The game asks for Japanese text with it.
- **Try anyway (may crash).** A Setup option (Windows: under Advanced; Linux: next to Prepare) that builds a game version HWDER does not recognise with the closest known version's layout. It works only if the code is laid out the same; otherwise the build or the game fails.

## v0.3.1 (2026-10-30)

- **Fix: the game no longer fails to start on PCs with Microsoft's Vulkan compatibility layer** (issue #2). Windows' OpenCL/OpenGL/Vulkan Compatibility Pack adds a Vulkan-on-Direct3D 12 device that HWDER cannot use; it could be picked over the real GPU driver. HWDER now skips devices without the Vulkan extensions it needs and prefers native drivers, and the error message no longer says Vulkan is missing when a device was only unusable.
- **Fix: Linux Setup's build no longer fails at the final link** when the system has `ld.lld` on its PATH (issue #1).
- **Version in the menus.** The startup menu and the F1 menu header show the HWDER version.
- **Antivirus warnings.** Some players saw a virus warning when downloading v0.3.0. An audit of the v0.3.0 release found nothing malicious: both packages hold exactly the tagged source, `HWDER Setup.exe` is byte-identical to an independent build of that source except for its link timestamp, the tools Setup downloads match the official files, and Microsoft Defender reports no threats. Setup is still unsigned and starts PowerShell and downloads compilers, which heuristic scanners can dislike. [Antivirus warnings](docs/antivirus.md) explains what is flagged and why, and how to check your download against the SHA-256 GitHub lists next to each file. If your antivirus flags HWDER, please report the product and the detection name.

## v0.3.0

- **Linux release.** HWDER now runs natively on Linux (tested on a ROG Ally with Bazzite, Game Mode included). Download `HWDER-linux-x86_64-<version>.tar.gz`, extract it and run `hwder-setup`: a Setup window (or `hwder-setup --cli` in a terminal) picks your game, keys and update 1.0.1, downloads the pinned build tools and builds the game on your machine. No container or extra packages; it adds menu entries and a Steam shortcut. Saves live in `~/.local/share/HWDER/save`.
- **Mods.** A `mods` folder (LayeredFS) is laid over the game's files on Windows and Linux; see the README.
- **Setup offers Update.** When you open a newer Setup over an existing install, the main button says Update (the line above names the installed and the new version) instead of starting the old game.
- **Faster Setup.** The private build tools are checked once against their published SHA-256 when downloaded, then only by a quick stamp, and one copy is shared by every Setup you unpack, so an Update no longer installs or re-checks them. *Repair private tools* re-checks everything on request.
- **Linux fixes:** battles render on newer Mesa drivers (depth format fallback); steady audio on PipeWire; A/B follow the button-label setting.

## v0.2.2

- **Fix: Setup's Prepare builds the game again.** The new AVX2 startup check in v0.2.1 did not compile with the compiler Setup installs, so Prepare stopped with an error. Everything else is the same as v0.2.1.

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
