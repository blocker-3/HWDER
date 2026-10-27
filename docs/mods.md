# Mods (LayeredFS)

HWDER lays mod files over the game's RomFS when the game starts, the way Atmosphere's LayeredFS does
on a Switch. The game file (`.nsp`) and the unpacked update are never changed. Windows and Linux share
the same code (`runtime/src/fs/mods.cpp`, `runtime/src/fs/romfs.cpp`).

## Where mods go

| | Default mods folder |
|---|---|
| Windows | `mods\` beside `hwder.exe` (Setup: `%LOCALAPPDATA%\HWDER\play\mods`) |
| Linux | `$XDG_DATA_HOME/HWDER/mods`, else `~/.local/share/HWDER/mods` (Setup rebuilds the exe folder under `~/.cache`, so user data stays out of it, like the saves) |

`mods_dir=` in `hwder_settings.ini` picks another folder; `HWDER_MODS` overrides both, and
`HWDER_NO_MODS=1` loads none.

Each folder inside the mods folder is one mod. HWDER accepts, inside a mod folder:

- the RomFS tree itself (`mods/<mod>/data/...`, `mods/<mod>/movie/...`);
- a `romfs/` folder (`mods/<mod>/romfs/data/...`);
- the Atmosphere layout (`mods/<mod>/atmosphere/contents/0100AE00096EA000/romfs/...`), also without the
  leading `atmosphere/`, or just `0100AE00096EA000/romfs/`.

So a Switch mod zip can be extracted into its own folder as it is.

## Rules

- Mods apply in folder name order; where two change the same file, the later one wins.
- A mod file replaces the game file at the same path or is added if the game has none. A name that
  differs only in upper/lower case still replaces the game's file (Windows tools often change case).
- A mod is off when its folder name starts with `_` or `.`, or when it is switched off in the menu
  (saved as `mods_off=` in the settings, names separated by `/`).
- Changes take effect at the next start. `hwder.log` lists each mod with how many files it replaced and
  added (`mods:` lines).
- Only RomFS (game data) mods. ExeFS patches (`exefs/`, `.ips`) are not supported: the game code is
  recompiled to C ahead of time, so code patches would have to be applied before recompiling.

## In-game menu

F1 (or holding View/Select) > **Mods**: the mods folder with **Open mods folder** (created if missing) and
**Copy path**, and every mod found with a checkbox, whether it is loaded in this run and how many files it
replaced and added. The listing refreshes every two seconds while the tab is open.

## How it works

Without mods (or with `HWDER_NO_MODS`), the NSP's RomFS (or the update's) is passed through untouched,
exactly as before. With mods, `romfs.cpp` builds a virtual RomFS image:

1. the base tree comes from the NSP RomFS's own metadata (with game update 1.0.1 that is the updated
   RomFS, because `nsp::romfs_read` serves the update's);
2. each enabled mod is laid over it;
3. the base files keep their original data offsets and are served from the NSP as one region; only mod
   files are appended after it, so the image differs from the NSP only in its metadata and the appended
   files.

`HWDER_FS_SELFTEST=1` resolves every file through the built image's hash tables, as the SDK does, and
compares it with its source (NSP or mod file). `HWDER_FS_TRACE=1` names the file each read hits
(`[nsp]` for unchanged game data).

The pipeline catalog prewarm (`gpu/vk/catalog_game_sources.cpp`) still reads shader sources from the
game's own RomFS. A mod that replaces shaders works, but its pipelines are compiled on first use rather
than prewarmed.

## Credits

The loader follows the LayeredFS mod support in Paulo Deyvid's fork
[PDEYVID/HWDER---mod-pt-br](https://github.com/PDEYVID/HWDER---mod-pt-br) (MIT), written for a Brazilian
Portuguese translation mod. HWDER does not ship any mod; mods are modified game data and belong to their
authors and the game's rights holders.
