# Native Windows setup prototype

IMPLEMENTED / PENDING REVIEW OR MERGE. This local prototype opens a native setup window without Python, a compiler, Vulkan or any game-linked code loaded into the bootstrap. Preparing the game still needs installed prerequisites and the player's own supported NSP/keys. Nothing has been published.

Extract the entire setup kit into a permanent, short writable application folder and open **HWDER Setup.exe**. Keep the source folder beside it. The window shows the game/key paths and a separate **Data folder**, initially Local AppData/HWDER. Saves, settings and the prepared executable stay in that folder's play directory; private generated files and build stages also stay under the chosen data folder. Application sources stay where the kit was extracted. No administrative application installation or registry configuration is performed.

Existing command-line users: the old default is Local AppData/hwder/local-build-v1. Choose that existing root as Data folder to reuse its play saves/settings and verified stages. The new default does not automatically migrate or copy them.

## Player flow

1. **Check requirements** detects installed standard x64 CPython 3.11–3.14 and the existing Microsoft C++ toolchain. Store aliases/install-manager launchers are not used for discovery. The check does not install anything. ARM64, 32-bit, free-threaded and future Python versions are rejected by this prototype.
2. **Get Python** and **C++ Build Tools** open official web pages after explaining the required choices. They do not download or run installers. The full Visual Studio IDE is not required: use Build Tools 2022 with Desktop development with C++, MSVC x64/x86, C++ Clang, CMake tools and a Windows 10/11 SDK. Finish those user-controlled installers, then recheck.
3. **Install setup components** requires visible network consent. It creates an owned setup-python virtual environment in the data folder and installs four pinned binary packages from PyPI. It never installs into system Python or bundles a compiler. Installation failures can be retried; another setup using the same data folder is serialized by the existing setup lock.
4. Browse the original NSP and prod.keys paths, then choose **Prepare game**. The established first_run.py backend checks the exact supported module profile, reuses verified stages and builds locally under the actual host build lock. Progress names stages; there is no invented percentage or time estimate. **Show details** exposes the bounded private log.
5. **Cancel** stops only the owned preparation job. Before stable publication, a nonce-bound request must receive the UI's commit permission. After permission, cancellation/close waits for commit or rollback; it does not hard-kill that critical transaction. This protects normal UI cancellation, not abrupt OS/power failure. Retry verifies reusable stages and the playable certificate.
6. **Play** is an explicit separate action after successful verification. A desktop shortcut opens setup with that data folder and requests a fresh certificate/path check before launch. No game starts on setup success, failure or cancellation.

The game's existing AVX2 CPU, Windows x64, Vulkan driver and supported-game requirements remain. The toolchain check is not a GPU or gameplay compatibility test.

## Package and dependency boundary

The only executable allowed in this kit is the reviewed code-free native bootstrap, identified by a source/object/link/import certificate and exact SHA256. The existing source-kit private-file exclusions are unchanged. The setup packager verifies the complete base source manifest/notices, overlays current committed public project sources, reopens the final ZIP and hashes every member. The manifest is a local integrity record, not a publisher signature; this prototype is unsigned.

No game-linked executable, generated C, NSP/NSO/ExeFS/RomFS, keys, saves, private caches, compiler, SDK, Python runtime or Python wheel is shipped. Project and pinned third-party source/license notices remain. Dependency downloads happen only if the user later consents to component installation:

| Component | Version | Upstream source/license |
|---|---|---|
| cryptography | 47.0.0 | [PyPI](https://pypi.org/project/cryptography/47.0.0/), Apache-2.0 OR BSD-3-Clause |
| lz4 | 4.4.5 | [PyPI](https://pypi.org/project/lz4/4.4.5/), [upstream notices](https://github.com/python-lz4/python-lz4/blob/v4.4.5/LICENSE) |
| cffi | 2.0.0 | [PyPI](https://pypi.org/project/cffi/2.0.0/), MIT |
| pycparser | 3.0 | [PyPI](https://pypi.org/project/pycparser/3.0/), BSD-3-Clause |

These versions match the locally exercised backend environment. Binary-only/no-dependency resolution installs exactly these pins; wheels are not hash-locked in this prototype. End-to-end component downloading/installation on a clean machine remains untested.

Official prerequisite sources: [Python Windows releases](https://www.python.org/downloads/windows/), [Microsoft downloads](https://visualstudio.microsoft.com/downloads/), [Build Tools components](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022). No installer or toolchain redistribution is asserted.

## Developer checks and limits

Build only the code-free window with tools/build.py --setup-ui. This CMake mode returns before any generated-game or runtime/SDL target. The bootstrap links one public C++ translation unit and Windows system libraries with the static CRT.

Offline tests cover the real backend publication boundary/cancel/rollback, fake component installation/ownership lock, certificate/path-gated launch and package exclusions. Controlled native preview tests exercise actual HWNDs, own process identity, owned JobObject cancellation, nonce/spoof rejection, retry/data invalidation, publication ACK/deferred close and rendered windows. Preview mode cannot launch a game or install packages. Test evidence remains under runs/release-setup-checks and runs/setup-preview-*.

No actual package installation/download, fresh real NSP extraction/generation/build or gameplay has been run for this UI prototype. Clean-machine installation, signing, accessibility/localization and broader installer recovery remain release gates. The existing proven CLI remains available and unchanged without the optional publication callback.
