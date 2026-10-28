# Playing HWDER

Everything about installing and playing on Windows that the [README](../README.md) only summarises. Linux players: see [Linux release](linux-release.md).

## Setup in detail

You do not need to clone this repository or install any developer tools. Windows 10/11 x64 only.

1. **Download** the latest `HWDER-setup-<version>.zip` from this repository's **Releases** page (not the
   green "Code > Download ZIP" button, which is source only).
2. **Extract** the whole zip into a folder you will keep (for example `C:\Games\HWDER-setup`) and open
   **HWDER Setup.exe**. The exe is not code-signed yet, so Windows may show "Windows protected your PC":
   choose **More info > Run anyway**.
3. **Next**: pick your game file (`.nsp`) and your `prod.keys`. Both must come from your own Switch and
   your own copy of *Hyrule Warriors: Definitive Edition*: the game dumped from your cartridge or eShop
   purchase (the base game, without an update merged in), and the keys dumped from the same console
   with homebrew tools. HWDER ships neither and
   cannot help you obtain them. The files are used where they are; keys are never copied. The optional
   **Game update** field takes the 1.0.1 update (`.nsp` or `.nsz`); the game is then built from the
   updated code.
4. **Install**: after you confirm, Setup downloads pinned build tools (Python, LLVM-MinGW, CMake, Ninja
   and five Python packages, each checked against a fixed SHA-256) from python.org, GitHub and PyPI into
   its Data folder (default `%LOCALAPPDATA%\HWDER`). No administrator rights, system installs or PATH
   changes.
5. **Prepare**: Setup checks that your dump is the supported version, extracts the game code, translates
   it to C and compiles `hwder.exe` on your PC. It shows the step (k of 6) and the time remaining; expect
   several minutes and several GB of disk space.
6. **Play** starts the game. The first launch shows a startup screen that checks your game and keys.
   Once it is ready, **Create desktop shortcut** and **Add to Steam** (a non-Steam game; Setup closes
   and reopens Steam itself) let you start the game from the shortcut, from Steam (or `%LOCALAPPDATA%\HWDER\play\hwder.exe`) without opening Setup. Keep your `.nsp` and
   `prod.keys` where they are: the game reads them every time it starts.

**Advanced** also holds the Data folder choice, Check requirements, Open game folder and the Details log.
Next time, Setup opens straight on **Play**. More detail: [Windows setup](windows-setup.md).
Developers who want to build from a checkout: [local-build.md](local-build.md) and [Building from source](building.md).

**No game code or assets are in this repository or the release zip.** The game-linked `hwder.exe`,
PDB and generated C are made on your machine from your own dump and are never packaged. `tools/make_dist.py`
and `tools/make_setup_dist.py` create the source and setup kits, with no game dump, keys, generated
program, saves or private shader cache.

### The first minutes on a fresh install can hitch

Each graphics pipeline is compiled the first time the game needs it, so the opening minutes (and each new stage the first time) can show short hitches or
an object appearing a moment late. Graphics > Shaders > "Compile shaders in the background" (on by
default) picks late objects over hitches. Compiled pipelines are saved beside `hwder.exe`
(`pipeline_cache.bin`, `pipeline_warmup.bin`) and prebuilt before the startup screen on later
launches, so this fades as you play. A new HWDER version, GPU or graphics driver starts the warmup list
over (the driver cache may still help). Deleting both files is safe. Internals: [pipeline cache](pipeline-cache.md).

## Requirements, controls and files

Requirements: Windows 10/11 x64, a Vulkan 1.2 driver (NVIDIA, AMD, Intel), an AVX2 CPU, and
Media Foundation (built into Windows; "N" editions need the Media Feature Pack) for the movies.
Without an AVX2 CPU or a Vulkan 1.2 driver the game shows a message saying which is missing and
closes; Setup's Check and Prepare also refuse a CPU without AVX2.

In game: **F1** opens the options overlay (internal resolution scale, unlocked resolution for
ultrawide, vsync, borderless, anisotropic filtering, FPS counter, opt-in 60 Hz monitor switch, and
a save-file cheats tab). Settings > Display > Frame rate > "120 fps (experimental)" draws an extra in-between frame per
battle tick (off by default, switchable while playing; menus stay at 60; see [high-framerate.md](high-framerate.md)). **F11** toggles borderless fullscreen. SDL3 gamepads and XInput controllers work; two-player local co-op (the game's 2P mode): controllers take player slots in the order they connect, and the keyboard joins the first free slot by default (Settings > Input > Players chooses a fixed player or turns it off). Pressing the left stick on the warrior select answers the game's controller screen with the connected players;
keyboard: WASD move, arrows camera, K/Enter = A, J/Backspace = B, I = X, L = Y, Q/E = L/R,
U/O = ZL/ZR, Space = +, Tab = -, 1-4 = D-pad, Z/C = stick clicks.

The wizard keeps its playable result in the selected Data folder, initially
`%LOCALAPPDATA%\HWDER\play\hwder.exe`. The legacy CLI defaults to
`%LOCALAPPDATA%\hwder\local-build-v1`; select that existing folder in the wizard to reuse it.
There is no automatic data migration. Files created beside the exe: `hwder_settings.ini`, `hwder.log` (attach to bug reports),
`save\` (console save layout), `cache\` and `pipeline_cache.bin` (safe to delete). To use another save, choose
its `zmha.bin` on the startup screen or in the menu's Gameplay tab (Choose save file). A save in the console
layout is used where it is; any other (an emulator export, a backup) is imported into the current save folder
after asking, with the save it replaces backed up first. Nothing is moved or deleted.

**Mods** (translations, texture or data mods): put each mod in its own folder inside `mods\` beside
`hwder.exe` (on Linux `~/.local/share/HWDER/mods`). Mods made for the Switch work as they are: the
folder may hold the game's file tree itself, a `romfs` folder or the `atmosphere/contents/0100AE00096EA000/romfs`
layout. They are laid over the game's files at startup in name order (the later one wins), and your game
file is never changed. The menu's **Mods** tab lists them, turns each on or off for the next start and opens
the folder; a folder whose name starts with `_` is off. More: [mods](mods.md).

## Field of view

Custom field-of-view adjustment is temporarily hidden because wider FOV can misplace officer nameplates. The game uses its default FOV (100%); older saved `fov=` overrides are ignored. This does not disable the separate ultrawide aspect-ratio option.

## Reporting a bug

`hwder.log` is written beside `hwder.exe` (or wherever `HWDER_LOG` points) and is rewritten on
every launch. After a crash, a minidump `hwder-crash-YYYYMMDD-HHMMSS.dmp` is written next to it
(the newest 5 are kept; `HWDER_NO_DUMP=1` turns them off; they never leave your machine). Open an
issue with the [bug report form](https://github.com/sonsegajp/HWDER/issues/new?template=bug_report.yml),
attach the log, and zip the .dmp before dragging it in. The version string it asks for is at the
bottom of the F1 menu and on the first line of the log. Never attach game files or keys.
