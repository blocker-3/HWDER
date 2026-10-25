# Releasing the setup zip

The player download is `HWDER-setup-<version>.zip`: the project source, the pinned third-party
source with its licence files, and the code-free `HWDER Setup.exe`. It contains no game code,
keys or game files; Prepare builds the game on the player's PC from their own dump.

## One command

From a clean checkout with submodules initialised and the release tag on HEAD:

    python tools/release_kit.py

It builds the source kit (`make_dist.py`), installs the pinned toolset from
`tools/managed_toolchain.json` (public downloads checked by SHA-256), compiles `HWDER Setup.exe`
(`build.py --setup-ui`), writes its bootstrap certificate, assembles the zip
(`make_setup_dist.py`) and checks it again: one exe, no generated C, dumps, keys, saves or
images. Output in `dist/`: the zip, `.sha256` and a `.json` summary. `--allow-untagged` packages
a test build. Keep `--work` short on Windows (the toolset nests deep paths).

## On GitHub

`.github/workflows/release.yml` runs the same command on `windows-2022`:

- Pushing a `v*` tag runs the synthetic tests, builds the zip and attaches it with its `.sha256`
  to a **draft** release. Publishing (and pasting the release notes) stays manual.
- Actions > Release setup zip > Run workflow builds an untagged test zip, kept as a workflow
  artifact only.

It uses only `GITHUB_TOKEN`. Settings > Actions > General > Workflow permissions can stay
read-only, since the job asks for `contents: write` itself.

The CI `HWDER Setup.exe` is not byte-identical to a local build, but the
source and third-party files in the zip are the same as a local package of the same commit.
