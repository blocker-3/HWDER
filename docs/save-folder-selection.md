# Save folder selection

The runtime stores account saves at `<save root>/0100AE00096EA000/48574445525F55530000000000000001`. Older folder selection stored an account leaf verbatim, so the filesystem appended the title/user again and created a fresh nested save instead of opening the selected originals.

The picker now recognizes this exact title/user leaf when both `zmha.bin` and `zmhasd.bin` are regular files, and stores its ancestor save root. Existing saved canonical leaf preferences are interpreted the same way at startup without rewriting settings or moving/copying saves. A leaf containing both originals and a mistaken nested fresh copy still resolves to the original root. Other filesystem save-data type routing is unchanged.

Wrong title/user leaves, partial pairs, arbitrary flat exports and ambiguous flat/nested choices are refused by the picker before its writable probe. Select their actual supported save root or a complete recognized account folder; this change does not import arbitrary exports. Existing unsupported preferences are not migrated automatically. An already running game's active root stays fixed, so restart after changing the folder.

Validation: the synthetic `save_folder_selection` CTest covers 18 cases (roots, account leaves, title folders, flat and partial folders) against fixtures created under the build directory; it reads no real save.
