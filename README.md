# HWDER - Hyrule Warriors: Definitive Edition Recomp

*Hyrule Warriors: Definitive Edition* running natively on Windows and Linux PCs, recompiled from your own
Switch copy. No emulator, and no game files in this repository.

## What HWDER adds over the Switch version

- **120 fps battles.** An optional 120 fps mode draws an extra in-between frame for every game
  tick, and it holds a steady 120 even in the busiest battles (where it used to drop to about
  73-85 fps). Menus stay at 60. The normal mode holds an exact 60.
- **Higher resolution.** Render 3D at 0.5x to 3x of 1080p (up to 5760x3240), or turn on
  Unlocked resolution to render at your window's full size.
- **Ultrawide.** On 21:9 and wider screens the camera widens to fill the screen while the HUD
  stays in a centred 16:9 area.
- **Native Linux release.** A Setup window (or `hwder-setup --cli`) builds the game on your machine,
  with menu entries and Add to Steam for Game Mode. No container; tested on a ROG Ally running Bazzite.
- **Picture options.** Anisotropic texture filtering up to 16x, FXAA, sharpening, bloom and
  contrast controls, VSync, borderless fullscreen (F11), and an option that keeps fences, rubble
  and other small scenery from vanishing in big fights.
- **Any controller, remappable.** Xbox, PlayStation, Switch Pro and other gamepads, plus keyboard.
  Remap buttons per controller type, choose Switch-position or printed-label face buttons, and set
  deadzones, stick inversion, rumble strength and gyro motion.
- **Two-player co-op on one PC.** The game's local 2P mode with two controllers, or a controller
  and the keyboard.
- **Mods.** Drop translation, texture or data mods (Switch LayeredFS layouts work as they are)
  into a `mods` folder; the menu's Mods tab turns each on or off. Your game file is never changed.
- **More game memory.** Choose the Switch's 3.2 GB, 4 GB or 6 GB.
- **Game update 1.0.1** is supported (optional; the base game works on its own).
- **Easy saves.** Pick any `zmha.bin`, including an emulator export or backup; it is imported
  after asking, with the replaced save backed up first.
- **Launch from your desktop or Steam.** Setup creates a desktop shortcut or adds the game to
  Steam with its name and box art.
- **In-game HWDER menu** (F1, or hold View/Select on a controller) for all of the above, most of
  it switchable while you play.
- **Crash reports that help.** A log and crash dump are written beside the game, and a
  [bug report form](https://github.com/sonsegajp/HWDER/issues/new?template=bug_report.yml)
  asks for what we need.

## Quickstart

You need your own game dump (`.nsp`) and `prod.keys` from your own Switch, and optionally update
1.0.1; HWDER ships none of them. Keep them where they are: the game reads them every time it starts,
and keys are never copied.

### Windows

Windows 10/11 x64 with a Vulkan 1.2 graphics driver and an AVX2 CPU.

1. Download the latest `HWDER-setup-<version>.zip` from the
   [Releases](https://github.com/sonsegajp/HWDER/releases) page.
2. Extract the whole zip into a folder you will keep and open **HWDER Setup.exe**. If Windows
   says "Windows protected your PC", choose **More info > Run anyway**.
3. Choose your game file (`.nsp`), your `prod.keys` and, if you have it, the 1.0.1 game update.
   Click **Next**.
4. Click **Install** and confirm. Setup downloads its own private build tools (about 260 MB, no
   admin rights).
5. Click **Prepare**. Setup builds the game on your PC; this takes several minutes and several GB
   of disk space.
6. Click **Play**.
7. Optional: **Create desktop shortcut** or **Add to Steam** to start the game without Setup next
   time.
8. In game, press **F1** and turn on Display > Frame rate > **120 fps (experimental)**.

On an existing install a newer Setup shows **Update**, which rebuilds the game and keeps your saves
and settings. Every step is explained in [Playing HWDER](docs/playing.md).

### Linux

x86-64 with glibc 2.35 or newer, Vulkan 1.3, an AVX2 CPU and Python 3 (Bazzite, SteamOS, Fedora,
Ubuntu and Arch include it).

1. Download the latest `HWDER-linux-x86_64-<version>.tar.gz` from the
   [Releases](https://github.com/sonsegajp/HWDER/releases) page and extract it.
2. Run `hwder-setup` (double-click it, or run `hwder-setup --cli` in a terminal), choose your game,
   keys and optional update, and let it build. A first install takes about 15 minutes on a ROG Ally.
3. Start the game from the menu entry, or from Steam after **Add to Steam**.

Saves live in `~/.local/share/HWDER/save`, mods in `~/.local/share/HWDER/mods`. Details:
[Linux release](docs/linux-release.md).

## Documentation

| Doc | What is in it |
|---|---|
| [Playing HWDER](docs/playing.md) | Setup step by step, first-run shader hitches, requirements, controls, files beside the exe, saves, mods, reporting a bug |
| [Mods](docs/mods.md) | Mods folder layouts and load order |
| [120 fps mode](docs/high-framerate.md) | How the extra frame is drawn, pacing and known limits |
| [Windows setup](docs/windows-setup.md) | What Setup.exe does, isolation and packaging |
| [Linux release](docs/linux-release.md) and [Linux native port](docs/linux-native.md) | Linux Setup and tarball, native port build and validation |
| [Building from source](docs/building.md) | Developer build, the recompile loop, synthetic tests |
| [How HWDER works](docs/how-it-works.md) | The recompiler, kernel, services, GPU and shader translator; repository layout |
| [Environment variables](docs/environment-variables.md) | Every `HWDER_*` debugging switch |
| [Pipeline cache](docs/pipeline-cache.md) | Pipeline warmup and the metadata pipeline catalog |

## Reporting a bug

Open an issue with the [bug report form](https://github.com/sonsegajp/HWDER/issues/new?template=bug_report.yml)
and attach `hwder.log` from beside `hwder.exe` (and a zipped crash dump, if there is one). Never attach
game files or keys. More: [Reporting a bug](docs/playing.md#reporting-a-bug).

## Status

Boots, menus, movies, Adventure/Legend mode battles, saves and audio work; gameplay runs at the
game's native 60 Hz (30 Hz where the game itself drops to half rate), or at 120 fps with the
optional 120 fps mode, at native or scaled resolution. Known issues are tracked in the issue tracker.

## License

MIT for everything in this repository (see `LICENSE`). Hyrule Warriors: Definitive Edition is
the property of Nintendo and Koei Tecmo; you need your own copy of the game.
