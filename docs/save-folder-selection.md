# Save folder selection

The runtime stores account saves at `<save root>/0100AE00096EA000/48574445525F55530000000000000001`. Older folder selection stored an account leaf verbatim, so the filesystem appended the title/user again and created a fresh nested save instead of opening the selected originals.

The picker now recognizes this exact title/user leaf when both `zmha.bin` and `zmhasd.bin` are regular files, and stores its ancestor save root. Existing saved canonical leaf preferences are interpreted the same way at startup without rewriting settings or moving/copying saves. A leaf containing both originals and a mistaken nested fresh copy still resolves to the original root. Other filesystem save-data type routing is unchanged.

## Picking the save file (2026-10-23)

Picking a folder confused players: the title folder, the account folder and the root above it all look like "the save folder", and one level off used to start a new nested save. The startup screen and the Gameplay tab now ask for the save file itself, `zmha.bin` (`hwfs::inspect_save_file`):

- `zmha.bin` + `zmhasd.bin` in the console layout (`<root>/0100AE00096EA000/48574445525F55530000000000000001/`): the save folder switches to `<root>` and the save is used where it is. No copy question: the picked save is the one to play.
- the pair anywhere else (an emulator export, a backup): after a question, only those two files are imported into the current save folder's account leaf; a save they replace is first backed up beside it (`hwfs::import_save`). On the startup screen this happens at once; in the game at the next start (`save_copy_from` then names the source folder), so the running game cannot save over it.
- no `zmhasd.bin` next to it, or another file name: refused, nothing changes.

"Use default" switches back to the default save folder: next to hwder.exe on Windows, `$XDG_DATA_HOME/HWDER/save` (else `~/.local/share/HWDER/save`) on Linux. Saved folder preferences from the old picker keep working (below).

## Folder preferences (before 2026-10-23)

Wrong title/user leaves, partial pairs, arbitrary flat exports and ambiguous flat/nested choices are refused by the picker before its writable probe. Select their actual supported save root or a complete recognized account folder; this change does not import arbitrary exports. Existing unsupported preferences are not migrated automatically. An already running game's active root stays fixed, so restart after changing the folder.

Validation: the synthetic `save_folder_selection` CTest covers 24 cases (6 for the file picker) (roots, account leaves, title folders, flat and partial folders) against fixtures created under the build directory; it reads no real save.
