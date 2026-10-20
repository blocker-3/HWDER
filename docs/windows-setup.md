# Private Windows setup prototype

IMPLEMENTED / PENDING REVIEW OR MERGE. LOCAL WINDOWS ONLY; unsigned and unpublished. The native window opens before Python exists. It contains no game code. Real game preparation and clean-machine gameplay remain unverified.

Extract the complete kit and open **HWDER Setup.exe**, keeping its source folder beside it. Choose the **Data folder** before installing tools. It contains private tools, generated/build stages and `play/` with the executable, settings and saves. The default is Local AppData/HWDER. Existing CLI users may select their older Local AppData/hwder/local-build-v1 root to reuse its data; the wizard does not migrate folders automatically.

1. **Install private tools** displays explicit network/size/ownership consent. It downloads eight fixed SHA-256 artifacts from python.org, GitHub and PyPI: CPython3.13.16 embedded x64, LLVM-MinGW20260908 UCRT x64, CMake3.31.12, Ninja1.13.2 and four pinned Python wheels. No administrator access, registry/global PATH change, pip, venv or system developer installation is required. Full component notices remain with downloaded tools.
2. **Check requirements** verifies installed file certificates and private tools. Every tool file is read; verification can take time. The window remains responsive and Cancel owns only its current process job. Missing or changed tools require the explicit install action; Check never downloads.
3. Browse to your own supported NSP and prod.keys files. The wizard records paths only, never copies keys, and never includes game inputs in a release.
4. **Prepare game** uses the established first_run backend to validate the supported module hashes, generate code and build locally. Stages and detailed errors are visible, with no invented percentage. Verified work is reused. Private GNU objects have a distinct ABI/tool/source identity and are never mixed with MSVC objects.
5. Before updating the playable executable, the UI acknowledges the publication boundary. Cancel/Close waits during that short transaction. Failures leave Play disabled and Retry available. This handles normal cancellation, not a promise of crash/power-loss atomicity.
6. **Play** explicitly verifies the playable certificate and original input paths. Managed output must have only Windows system DLL imports and the expected stack/ASLR contract; gameplay gets Windows-only PATH. Preparation never auto-launches. A shortcut reopens this setup bound to the chosen Data folder.

Windows10/11 x64, an AVX2 CPU, Vulkan1.2 driver and Windows Media Foundation are still game requirements. Windows N editions may need the Media Feature Pack; this tool does not install OS features. Network access is required only for consented tool downloads. Existing compiler installations are neither modified nor required by the private path.

## Isolation and packaging

The system PowerShell/.NET helper verifies/extracts the first interpreter. The isolated interpreter installs a separately certified complete stack and wheels without pip. Strict ZIP names, case collisions, links/reparse points, file counts/byte budgets, owned staging and per-file hashes are checked; incomplete output never becomes an active toolset. No Windows SDK redistribution is assumed: LLVM-MinGW supplies its own Windows headers/import libraries under its retained notices.

The release kit contains public source, pinned third-party source/notices and exactly one certified game-free setup executable. It includes applicable notices for statically linked bootstrap runtimes. No Python/compiler binaries, wheels, game-linked executable, generated guest C, keys, saves or private shader caches are bundled. Downloaded tools and generated code remain private in the chosen Data folder.

The original installed-Python/Visual Studio CLI remains unchanged unless explicitly given a managed toolchain by the bridge. [Legacy developer flow](local-build.md).

## Evidence and limits

Eight pinned download/extract transactions and private AES/LZ4 imports passed. All17,292 installed files match the certificate and reference traversal; no measured hashing speedup is claimed. Selected production units and four public generated-C/Windows-ABI/cache tests passed. The native setup was compiled using the private compiler; actual normal Check passed with Windows-only PATH, missing tools refused without payload creation, and synthetic preparation tested Job cancellation, nonce/spoof rejection, retry, input invalidation, publication ACK/deferred close and rendered controls. Preview never prepares or launches a game.

The full runtime/static-SDL synthetic-guest link probe is a separate NOT-A-GAME artifact, never executed or packaged; its result is tracked in the workspace owner note. It cannot certify a real game. Full clean-machine/real NSP preparation, gameplay, signing and distribution remain pending. Tests/evidence are retained under runs/managed-checks and runs/setup-preview-* in the unmerged worktree.
