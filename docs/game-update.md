# Game update 1.0.1

HWDER plays the base game (1.0.0) or the base game plus update 1.0.1 (title 0100AE00096EA800, v65536). The
update is optional: Setup's **Game update** field takes it as `.nsp` or `.nsz`, next to the base game `.nsp`.

## What the update changes

- `main` is rebuilt: 25359 of its 25366 functions are identical apart from their address (moved by a few
  hundred bytes), 7 functions changed (10652 bytes). `rtld`, `sdk`, `subsdk0`, `subsdk1` are unchanged.
- The RomFS patch replaces 5 files, `data/voice/EXCL_025.bin` to `EXCL_029.bin`.

The function-level analysis behind this port is kept outside the repository.

## How it is built

- `tools/supported_games.json` has a profile per version. The 1.0.1 profile names its own symbol and
  entry files, `tools/recomp/v1.0.1/symbols.txt` and `extra_entries.txt`. Prepare picks the profile from the
  module hashes and passes those files to the generator.
- `tools/recomp/remap_symbols.py` made them from the base files: it matches every base function to 1.0.1 by its
  body with PC-relative fields and ADRP offsets masked, checks the call graph, maps data through ADRP
  references (or, failing that, a unique byte match nearby), and stops on any named address in changed code.
  Rerun it when a name is added to `symbols.txt`:

      python tools/recomp/remap_symbols.py --base-main <base exefs>/main --new-main <1.0.1 exefs>/main \
          --functable <base generated>/functable.c --out tools/recomp/v1.0.1

- The runtime has no literal guest addresses left: everything goes through `symbols.h` (`HW_ADDR_*`, `f_*`),
  including the Battle_Tick return addresses (offsets from its start) and the `HWDER_PATCH_DT` constants.
- `module_info.c` records each module's build id; the loader refuses a different module with a message to
  Prepare again, so a 1.0.1 build cannot run base files or the reverse.

## How it is read

`tools/game_update.py` unpacks the update once (Prepare does this into the Data folder's cache):
`exefs/main`, `exefs/main.npdm`, `romfs.patch` (the update's RomFS section, decrypted: the patch data and the
BKTR tables) and `update.json`. An `.nsp` update is decrypted with AES-CTR-Ex (the counter's generation changes
per region); an `.nsz` stores the same bytes inside one zstd stream (zstandard 0.25.0, BSD-3-Clause, a pinned
private Setup package). Both give byte-identical output.

The game gets the folder through `update_path` in `hwder_settings.ini` (Setup writes it; empty switches it off)
or `HWDER_UPDATE`. `fs/nsp.cpp` then serves the update's `main` and `main.npdm` instead of the base game's,
and reads the RomFS through the indirect table: each range comes either from the base game's RomFS section or
from `romfs.patch`. The startup screen and the pipeline catalog see version 65536.
