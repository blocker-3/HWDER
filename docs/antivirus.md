# Antivirus warnings and verifying a download

Some players get a "virus detection" for `HWDER Setup.exe` from the setup zip. Microsoft Defender
names it `Trojan:Win32/Wacatac.B!ml`. This page explains what that name means, how to check your
copy and get Setup running again, and what is in the download.

## What `Wacatac.B!ml` means

The `!ml` suffix means the file was judged by a machine-learning model, not matched against known
malware. Defender's cloud model looks at features of a new, unsigned program (no publisher, how it
starts other programs, that it downloads files, how it was compiled) and guesses. `Wacatac` is the
generic family name that model gives such files. Names ending in `.ml`, `Heur`, `Generic` or
`Gen:Variant` from other products are the same kind of guess.

The verdict belongs to one exact file (its SHA-256 hash), not to HWDER's code. On 2026-11-01
Defender flagged the v0.3.3 `HWDER Setup.exe` as `Wacatac.B!ml` as soon as it was extracted,
before it was run. A local rebuild of the same v0.3.3 source, which differs from the release
only in the 6 bytes of its two link timestamps, scanned clean. Every new release is therefore a
new file that the model judges again.

## If Defender blocked or removed Setup

1. Check your download first (next section). Only continue if the SHA-256 matches.
2. Open **Windows Security > Virus & threat protection > Protection history**, select the
   `Trojan:Win32/Wacatac.B!ml` entry for `HWDER Setup.exe`, then **Actions > Restore** (if it was
   quarantined) or **Allow on device**. Restore puts the file back. Allow stops Defender from
   flagging that exact file again.
3. If it is flagged again, you can exclude your HWDER setup folder: **Virus & threat protection
   settings > Manage settings > Exclusions > Add or remove exclusions > Add an exclusion >
   Folder**. Exclude only the folder you extracted the zip into, never a whole drive or your
   Downloads folder. Remove the exclusion once the game is prepared.
4. You can tell Microsoft it is a false positive at <https://www.microsoft.com/wdsi/filesubmission>.
   Choose **Home customer**, select Microsoft Defender Antivirus, upload `HWDER Setup.exe` (not the
   game, keys or saves), choose **Incorrectly detected as malware/malicious**, and give the
   detection name `Trojan:Win32/Wacatac.B!ml`. Microsoft usually answers within a few days. A
   cleared verdict then reaches every PC through Defender's updates.

## Check your download

1. Get the zip only from the [Releases](https://github.com/blocker-3/HWDER/releases) page.
2. Compare its SHA-256 with the one GitHub lists next to the asset. Releases after v0.3.3 also
   attach a `SHA256SUMS` file and list the hashes in the release notes. In PowerShell:

       Get-FileHash .\HWDER-setup-v0.3.3.zip -Algorithm SHA256

   v0.3.3: `acd0dfbc243db9919be3d97abf6c224d010040a94ce9e09b4ae7e4a2b73fd522`
   (its `HWDER Setup.exe`: `1c7a8b1eaa14490fa5abc1a57791ffd504b38b95ed22265832f7b8066952dffd`).
   v0.3.0: `96fe3f83fcedcd20013cd52558338f50199e08091b97aa98252c53686c0712a5`.
3. The zip holds exactly one program, `HWDER Setup.exe`. Everything else is source code, licence
   files and two text files. `setup-manifest.json` lists the SHA-256 of every file, including
   `HWDER Setup.exe`.

If your antivirus flags it, please report the product and the exact detection name in a bug
report.

## What Setup does

Nothing in the kit is packed, encrypted or self-extracting, and Setup is not a PyInstaller or
py2exe bundle. It is a small C++ program compiled with clang (LLVM-MinGW). It imports only Windows
system DLLs (user32, gdi32, comctl32, shell32, ole32, advapi32, winhttp, kernel32 and the
Universal CRT). Its behaviour is all in `source/tools/setup_ui/bootstrap.cpp`:

- **It starts Windows PowerShell** with `-ExecutionPolicy Bypass -File install_private_python.ps1`
  for Install, Prepare, Check and Play. The bypass is needed because Windows does not run scripts
  by default, and a script extracted from a downloaded zip carries the download mark. Its console
  window is visible, minimized, with the title "HWDER Setup - Windows PowerShell". Its output
  shows in Setup under Advanced > Details. (v0.3.3 and earlier started it hidden.)
- **It downloads and runs programs.** Install fetches Python, LLVM-MinGW, CMake, Ninja and five
  Python packages from python.org, GitHub and PyPI (about 260 MB, after you confirm), checks each
  against a fixed SHA-256 in `tools/managed_toolchain.json`, and runs them.
- **It builds a new, unsigned `hwder.exe` on your PC.** Prepare compiles the game from your own
  dump. A freshly compiled program is unknown to every scanner. Some flag it when it is written or
  first started.
- **Add to Steam** closes Steam (`steam.exe -shutdown`), edits Steam's `shortcuts.vdf` (after a
  backup) and downloads box art from Nintendo's store image service, or from SteamGridDB when you
  give your own key. **Create desktop shortcut** writes `HWDER.lnk` to your Desktop.

Setup does not install a service, scheduled task, autostart or registry entry, does not ask for
administrator rights (its manifest says `asInvoker`), does not change Defender settings or add
exclusions, and does not send anything about you or your files anywhere. It reads the registry
only to find Python and Steam.

## What the project does to avoid false positives

Done for releases after v0.3.3 (branch `defender-mitigations`):

- `HWDER Setup.exe` has an icon, an application manifest (`asInvoker`, Windows 10/11
  `supportedOS`, common controls v6, DPI-unaware with GDI scaling) and Windows version information
  (CompanyName "HWDER project", ProductName "HWDER", FileDescription "HWDER Setup",
  OriginalFilename "HWDER Setup.exe", version from `git describe`). See
  `tools/setup_ui/bootstrap.rc`, `bootstrap.manifest` and `hwder.ico` (made by `make_icon.py` from
  uncompressed bitmaps, so it adds no compressed data).
- It is linked with `-s`: no symbol table or DWARF debug sections (2.1 MB down to 1.2 MB).
- PowerShell is no longer started hidden. Setup no longer creates it suspended and then resumes
  it (a pattern malware uses to tamper with a child process). The process starts directly inside
  Setup's kill-on-close job (`PROC_THREAD_ATTRIBUTE_JOB_LIST`). `ResumeThread` is gone from its
  imports.
- `tools/release_kit.py` refuses to package a Setup that lacks the icon, manifest, version
  strings or stripping (`pe_dependencies.setup_identity_problems`). The bootstrap certificate pins
  the new resource sources too.
- The release workflow attaches `SHA256SUMS` and lists the hashes in the draft release notes.

Not done: code signing (the project does not plan to sign) and an MSVC build of Setup (no Visual
Studio on the maintainer PC, and the release certificate pins the LLVM-MinGW toolchain).

## Audit of the v0.3.0 release (2026-10-30)

Checked from the public release assets:

- **The source in both packages is the tagged source.** Every project file in the Windows zip and
  the Linux tarball equals tag `v0.3.0` (commit `8e994a8`), apart from Windows line endings in the
  zip. The 2,042 third-party files (SDL, Dear ImGui, Vulkan-Headers, SPIRV-Headers) equal the
  upstream commits the tag pins. Every per-file SHA-256 in both manifests matches.
- **`HWDER Setup.exe` is what the source compiles to.** It is byte-identical to the maintainer's
  own local build of the same source (`79713b0`; the tag only adds the changelog) except for
  the 6 bytes of the two link timestamps. Two machines building independently got the same
  program. Its link input is the one Setup source file.
- **The downloaded tools are the official ones.** Re-downloading LLVM-MinGW, CMake, Ninja and the
  five Python wheels gave exactly the SHA-256 pinned in `tools/managed_toolchain.json`.
  (python.org could not be reached from the audit machine; its pin is fixed in two places.)
- **The release workflow pins its actions.** `actions/checkout`, `setup-python`, `upload-artifact`
  and `download-artifact` are pinned to full commit SHAs, which match the named release tags. The
  workflow uses only `GITHUB_TOKEN`, and asks for write access only in the job that drafts the release.
- **Microsoft Defender found nothing on 2026-10-30** (engine 4.18.26100.4, signatures 1.463.31.0)
  in the zip, the extracted kit, `HWDER Setup.exe` or `install_private_python.ps1`. The files
  were not uploaded to any online scanner.
- **The game program behaves like a game.** `hwder.exe` (built on your PC) reads the keyboard only
  while its own window has focus, installs a crash handler that writes `hwder.log` and minidumps
  beside the exe, and has no network code (the runtime links no network library).

What this does not prove: an audit cannot rule out a compromise of GitHub itself or of the
upstream projects, or a malicious change by someone with write access to this repository that
also regenerated the manifests. The reproducible `HWDER Setup.exe` narrows the last case: a
tampered program would not match an independent build. The prebuilt Linux pieces (the Setup
window and the static SDL3, FFmpeg and libcrypto libraries) were not rebuilt for this audit.

## Defender check of v0.3.3 and this branch (2026-11-01)

Engine 4.18.26100.4 / 1.1.26100.4, signatures 1.463.72.0, real-time, cloud (MAPS advanced) and
tamper protection on. Custom scans (`MpCmdRun -Scan -ScanType 3 -File ... -DisableRemediation`):

| File | Result |
| --- | --- |
| v0.3.3 release `HWDER Setup.exe` (1c7a8b1e...) | `Trojan:Win32/Wacatac.B!ml`, exit 2 |
| local rebuild of v0.3.3 source (differs only in link timestamps) | no threats, exit 0 |
| v0.3.3 + `-s` only | no threats |
| v0.3.3 + icon/manifest/version only | no threats |
| v0.3.3 + visible PowerShell / job-list launch only | no threats |
| this branch (all of the above) | no threats, exit 0 |

The local scans cannot show which change helps. The flagged release and the clean rebuild are the
same program, so the `!ml` verdict was most likely made in Defender's cloud for that one hash when
it first arrived as a download, and a never-seen local build gets no such verdict. The changes
remove features that such models weigh. Whether the next release passes will only show once it
is published and downloaded.
