# Local first-run build

`play-local.bat --nsp "your-game.nsp" --keys "your-prod.keys"` runs a complete user-local
build and launches only after success. `python tools/first_run.py` with the same arguments
prepares without launching. Environment variables `HWDER_NSP` and `HWDER_KEYS` can supply
these paths. Keys are read from their original location; their contents are never copied or
stored in a manifest. The game and keys must remain available on every launch.

## Prerequisites

- Windows 10/11 x64, AVX2 CPU, Vulkan 1.2 GPU driver and Media Foundation for playback.
- Python 3.10+ x64; `python -m pip install -r tools/requirements-setup.txt` installs
  `cryptography` and `lz4`. Installation is a separate, explicit user operation.
- Visual Studio 2022 or Build Tools, Desktop development with C++, C++ Clang tools for Windows,
  C++ CMake tools for Windows (CMake 3.24+ and Ninja), Windows 10/11 SDK and MSVC x64 libraries.
- Pinned SDL, ImGui, SPIR-V Headers and Vulkan Headers sources: included by the setup-kit
  packager, or initialized by `git submodule update --init --recursive` in a checkout.
- Several minutes and several GB of free disk space; allow more for compiler objects, PDBs,
  old versions and failed attempts. Default parallelism is at most eight jobs; use `--jobs 2`
  on memory-constrained machines. Use a short writable source/cache path (Windows build tools
  have path-length limits). `--cache "C:\short\private-hwder"` overrides the default cache.

`python tools/first_run.py --check` checks Python packages, third-party headers, clang-cl,
MSVC libraries, Windows SDK headers/libraries, CMake and Ninja in the discovered VS x64
developer environment. `HW_VCVARS` can point at a custom `vcvars64.bat`. This preflight does
not replace the actual full build; compiler/SDK incompatibilities still appear in its output.

## Validation and stages

Only one program NCA, title `0100AE00096EA000`, and the exact five-module set (`rtld`, `main`,
`subsdk0`, `subsdk1`, `sdk`) are supported. `tools/supported_games.json` records SHA-256 values
from the base-game code used to validate main `1c8ee53`. No game data or keys are in that file.
Other versions and update bundles are rejected. NSO hashes verify supported code identity;
they do not verify every RomFS asset or certify a dump's provenance.

Setup reads/decrypts the user's NSP with the project's existing `nx_extract` primitives,
checks title/module hashes, hashes the complete NSP, then writes only those five modules to
the private cache. It never extracts RomFS, logos or movies. It runs the existing
`python -m recomp.gen_main`, then the full `tools/build.py` with isolated generated/build
paths. The shared host Local AppData `hwder\build.lock` serializes native builds and single-file
compiler probes across worktrees. Windows resolves this directory through Shell32 with package
redirection disabled, including inside an AppContainer. Runtime/graphics code and the original
manual developer output paths are unchanged.

Progress and subprocess errors appear in the terminal. A failed/interrupted setup exits
nonzero and never launches. Rerun the same command to reuse verified extraction/generation
and resume Ninja's partial native compilation. Partial generation starts a fresh attempt.
If publication fails because the game is open, close it and rerun. No automatic fallback
launch of a different game/source version occurs.

## Cache and recovery

Default cache: `%LOCALAPPDATA%\hwder\local-build-v1`. Separate directories store ExeFS,
generated sources and native builds. Full SHA-256 identities are recorded in manifests;
short directory labels avoid excessively long Windows object paths. OS locks serialize
coordinators sharing one cache and release automatically when a process dies.

- Extraction identity includes the complete NSP hash and module hashes.
- Generation identity includes the extraction identity, generator/NSO/extractor code,
  hooks, tracked entries, optional private `data/extra_entries.txt`, Python version and lz4.
- Native build identity includes generation identity and the exact generated file hashes,
  actual runtime/tools/CMake/third-party
  file contents (including uncommitted changes), source location and discovered toolchain
  versions/paths, including SDK/MSVC versions and a digest of compiler/linker overrides and
  include/library search paths. Normal source updates get separate build output.
- Completed extraction/generation require matching manifests and exact output file hashes.
  Native output requires a successful full build and matching executable SHA-256. A stale
  executable left by a failed compile is never accepted without rerunning the build. Uncertified
  executables are removed to force Ninja to relink, while partial compiler objects remain reusable.
- Changes to source or cached build inputs during setup cause a failure instead of publishing a
  mixed version. If cached inputs change during compilation, that build directory is retained
  with an `.invalid-*` suffix so suspect compiler objects cannot be reused on retry.

After success, setup atomically replaces `play/hwder.exe`; that stable folder retains settings,
saves, logs, private texture/shader/pipeline caches and any configured custom save path across
source updates. `last-success.json` is updated only after publication. Failed extraction,
generation, compilation or publication leaves the previous playable executable and existing
save/cache data in place. No old cache entries are deleted automatically. If updating
`last-success.json` fails after executable replacement, setup restores the previous
executable. An abrupt process/OS termination can leave a `play/hwder-previous-*.tmp` recovery
copy; rerun setup to reconcile the verified executable and success marker. If restoration itself
fails (for example because an external launcher opened the new executable), retain that recovery
copy and close the game before retrying.

Keep backups of saves;
to reclaim space, close the game/setup and remove unneeded `exefs`, `generated` or `builds`
entries, or failed `.partial-*`/`.invalid-*` directories. **Do not delete `play` to clear build
artifacts: it contains your user state.**

Launching sets only the chosen NSP/key paths and clears inherited extracted ExeFS/RomFS
overrides. The runtime continues to decrypt the user's NSP while playing. The executable is
the same static architecture as the developer build; no DLL split or runtime JIT was added.

## Source/setup packaging boundary

`python tools/make_dist.py` uses tracked project source roots and explicit top-level files,
plus pinned third-party tracked source and license notices. It verifies the ZIP file set,
rejects private paths/binary formats and checks every entry against `source-manifest.json`.
The source manifest is a local integrity record, not a publisher signature. Packaging requires
a Git checkout with initialized submodules; consuming the kit does not require Git.

Never distribute the user's NSP/NSO/ExeFS/RomFS, keys/tickets, generated C, game-linked
`hwder.exe`, PDB/static libraries/objects, saves or private shader/texture/pipeline caches.
The old executable-only `make_dist.py` behavior was removed because the static executable
contains translated game code. Its former “only our own work” claim was incorrect.

The project runtime/tooling is under the root MIT license. Third-party notices are retained
from their own source trees; see `third_party/README.md` (SDL Zlib, ImGui MIT, SPIR-V Headers
MIT, Vulkan Headers Apache-2.0/MIT). The vendored RenderDoc API header is MIT, with its
embedded notice retained; the RenderDoc DLL/programs are not bundled. No Python packages, compiler, MSVC redistributables,
Windows SDK, decryption tools from outside this repository or installer are bundled. Any
future toolchain bundling needs its own license/distribution review; this workflow does not
assert a legal guarantee about use of game dumps, keys or local generated output.

Offline tests use synthetic binary/container and source fixtures only:

```
python -m unittest discover -s tools/tests -p test_first_run.py
```
