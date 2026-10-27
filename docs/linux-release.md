# Linux release (player install)

The Linux download is `HWDER-linux-x86_64-<version>.tar.gz`. Like the Windows zip it holds no game code: HWDER
Setup builds the game on the player's machine from their own dump. Target: Bazzite and SteamOS (immutable, nothing
installed on the system, no container); Fedora, Ubuntu 22.04+ and Arch should work (glibc 2.35 or newer, Python 3.8+,
Vulkan driver).

## What is in the tarball

| Path | What |
|---|---|
| `hwder-setup` | Starts the Setup window; `--cli` (or no display) runs the same steps as terminal questions. Other arguments go to `tools/linux_setup.py` (`status`, `prepare`, `steam`, `desktop`, `play`, `uninstall`). |
| `setup/hwder-setup-ui` | The Setup window (SDL3 + Dear ImGui, `tools/linux/setup_ui`). Needs only glibc; SDL loads X11/Wayland/OpenGL itself. Code-free: it runs `tools/linux_setup.py --json`. |
| `linux-deps/` | Static SDL3, libavcodec/libavutil (FFmpeg 8.0.3, H.264 decoder only, LGPL, its source in `linux-deps/sources/`) and OpenSSL 3.5.9 libcrypto, built on Ubuntu 22.04 by `tools/linux/build_deps.sh` with hidden visibility. Licences: `linux-deps/licenses/`. |
| source | The project source and pinned third-party source (make_dist's file list), `version.txt` for the version stamp. |
| `release.json` | Version, commit and SHA-256 of every file; Setup copies only verified files. |

## What Setup does

All in `$XDG_DATA_HOME/HWDER` (else `~/.local/share/HWDER`):

1. Copies the release to `app/` (so the download can be deleted; the menu entry "HWDER Setup" starts it from there).
2. Downloads the pinned toolchain once into `toolchain/<manifest hash>/` (`tools/linux/toolchain.json`): Zig 0.15.2
   (its Clang 20, lld, libc++ and glibc 2.35 stubs), CMake 3.31, Ninja 1.13, python-build-standalone 3.13.16 and the
   cryptography/cffi/pycparser/lz4/zstandard wheels. Each download is checked against its size and SHA-256; archives
   are unpacked with path checks. Older toolchains and the downloads are removed afterwards.
3. Checks the chosen files (NSP header, key names, update), then runs `tools/first_run.py` on the pinned Python with
   `CC`/`CXX` = Zig wrappers targeting `x86_64-linux-gnu.2.35`, `HWDER_LINUX_DEPS` = `app/linux-deps` and
   `--cache build/`. The game is `build/play/hwder`: it needs only `libc.so.6`, `libm.so.6` and the Vulkan loader
   (dlopened). first_run keeps only the build it just published (`--keep-old-builds` keeps the rest).
4. Writes `nsp_path`/`keys_path` into `build/play/hwder_settings.ini` (the files are read in place, never copied),
   the launcher `build/play/hwder-steam.sh`, the menu entries `hwder.desktop` and `hwder-setup.desktop`, and creates
   `mods/`. Saves go to `save/` (the game's default).
5. Optional: a desktop icon, Add to Steam (`tools/steam_shortcut.py`: Steam has to close; outside Game Mode Setup
   restarts it, in Game Mode Steam restarts by itself), Play.

`uninstall` removes `app/`, `toolchain/`, `build/` and the launchers; saves, mods and `setup.json` stay.

## Building the tarball

On Ubuntu 22.04 with `tools/linux/build-packages.txt` installed (the GitHub runner, or a podman container
`ubuntu:22.04`), from a clean checkout with submodules:

    python3 tools/linux/package_release.py [--allow-untagged]

It builds the deps (reused while `build_deps.sh` and the SDL commit are unchanged), the Setup window, checks that the
window needs only glibc (no symbol newer than 2.35), assembles the tarball and checks its members (no game files,
keys, generated C, saves or binaries of the game). `.github/workflows/release.yml` runs it on `ubuntu-22.04` for
`v*` tags next to the Windows zip and attaches both to the draft release.

## Notes for developers

- The Zig C++ runtime is LLVM's libc++/libunwind, linked statically. Guest threads therefore exit by jumping back to
  their entry (as `ExitThread` on Windows) instead of `pthread_exit`, whose forced unwind would run glibc's
  `libgcc_s` through libc++abi's personality routine and crash.
- `hwder` is linked with `-rdynamic` (named crash backtraces). The Vulkan function pointers are hidden and the
  static deps are built with hidden visibility, so Vulkan layers in the game process (gamescope's WSI layer,
  MangoHud, the Steam overlay) cannot bind to them.
- `HWDER_REQUIRE_WRITE_WATCH` makes the release build fail if its Linux headers lack `PAGEMAP_SCAN` (Zig 0.15.2
  ships 6.13 headers).
- Developers can still build against system packages (`docs/linux-native.md`, distrobox); without
  `HWDER_LINUX_DEPS` CMake uses pkg-config as before.
- `tools/linux_autoplay.py` runs native builds by default (`--container NAME` for old dev builds); test root
  `--root` or `$HWDER_TEST_ROOT`, play build `--play` (default: the installed release's `build/play`).
