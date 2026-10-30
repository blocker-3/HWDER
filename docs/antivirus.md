# Antivirus warnings and verifying a download

Some players reported a "virus detection" when downloading `HWDER-setup-v0.3.0.zip`. This page
says what is in the download, what an antivirus may react to, how to check your copy, and what
the v0.3.0 audit found.

## Check your download

1. Get the zip only from the [Releases](https://github.com/blocker-3/HWDER/releases) page.
2. Compare its SHA-256 with the one GitHub lists next to the asset. In PowerShell:

       Get-FileHash .\HWDER-setup-v0.3.0.zip -Algorithm SHA256

   v0.3.0: `96fe3f83fcedcd20013cd52558338f50199e08091b97aa98252c53686c0712a5`
   (Linux tarball: `c84a851e038d6135d70de4ebead08c36f6cd011630f000312b41450a942e6bcb`).
3. The zip holds exactly one program, `HWDER Setup.exe`. Everything else is source code, licence
   files and two text files. `setup-manifest.json` lists the SHA-256 of every file, including
   `HWDER Setup.exe` (v0.3.0: `4ae09c4fffd29a5ec824360b3778fd1ad1ef4d0fe0885ff2f1b368083db11b00`).

If your antivirus flags it, please report the product and the exact detection name in a bug
report. A name such as `Trojan:Win32/Wacatac.*!ml` or `Gen:Variant` ending in `!ml`, `.ml`,
`Heur` or `Generic` is a machine-learning or heuristic guess, not a match against known malware.

## Why an antivirus may react

Nothing in the kit is packed, encrypted or self-extracting, and Setup is not a PyInstaller or
py2exe bundle: it is a small C++ program compiled with clang (LLVM-MinGW). What can look
suspicious to a heuristic scanner is real behaviour, all of it visible in
`source/tools/setup_ui/bootstrap.cpp`:

- **Unsigned and new.** `HWDER Setup.exe` has no code signature, no version information and no
  icon, and each release is a file no scanner has seen before. These alone raise heuristic scores.
- **It starts PowerShell hidden with `-ExecutionPolicy Bypass`.** Install, Prepare, Check and Play
  run `install_private_python.ps1` this way (no console window). This is the same command line
  many malware droppers use, and is the most likely single trigger.
- **It downloads and runs programs.** Install fetches Python, LLVM-MinGW, CMake, Ninja and four
  Python packages from python.org, GitHub and PyPI (about 260 MB, after you confirm), checks each
  against a fixed SHA-256 in `tools/managed_toolchain.json`, and runs them.
- **It builds a new, unsigned `hwder.exe` on your PC.** Prepare compiles the game from your own
  dump. A freshly compiled program is unknown to every scanner; some flag it when it is written
  or first started.
- **Add to Steam** closes Steam (`steam.exe -shutdown`), edits Steam's `shortcuts.vdf` (after a
  backup) and downloads box art from Nintendo's store image service, or from SteamGridDB when you
  give your own key. **Create desktop shortcut** writes `HWDER.lnk` to your Desktop.

Setup does not install a service, scheduled task, autostart or registry entry, does not ask for
administrator rights, does not change Defender settings or add exclusions, and does not send
anything about you or your files anywhere. It reads the registry only to find Python and Steam.

## Audit of the v0.3.0 release (2026-10-30)

Checked from the public release assets:

- **The source in both packages is the tagged source.** Every project file in the Windows zip and
  the Linux tarball equals tag `v0.3.0` (commit `8e994a8`), apart from Windows line endings in the
  zip. The 2,042 third-party files (SDL, Dear ImGui, Vulkan-Headers, SPIRV-Headers) equal the
  upstream commits the tag pins. Every per-file SHA-256 in both manifests matches.
- **`HWDER Setup.exe` is what the source compiles to.** It is byte-identical to the maintainer's
  own local build of the same source (`79713b0`; the tag only adds the changelog) except for
  the 6 bytes of the two link timestamps. Two machines building independently got the same
  program. It imports only Windows system DLLs, and its link input is the one Setup source file.
- **The downloaded tools are the official ones.** Re-downloading LLVM-MinGW, CMake, Ninja and the
  five Python wheels today gives exactly the SHA-256 pinned in `tools/managed_toolchain.json`.
  (python.org could not be reached from the audit machine; its pin is fixed in two places.)
- **The release workflow pins its actions.** `actions/checkout`, `setup-python`, `upload-artifact`
  and `download-artifact` are pinned to full commit SHAs, which match the named release tags. The
  workflow uses only `GITHUB_TOKEN`, and asks for write access only in the job that drafts the release.
- **Microsoft Defender finds nothing.** On 2026-10-30 (engine 4.18.26100.4, signatures
  1.463.31.0, real-time and tamper protection on), a custom scan of the downloaded zip, the
  extracted kit, `HWDER Setup.exe` and `install_private_python.ps1` each reported no threats
  (exit code 0), with no earlier detection in the PC's history. Other vendors were not tested:
  the files were not uploaded to any online scanner.
- **The game program behaves like a game.** `hwder.exe` (built on your PC) reads the keyboard only
  while its own window has focus, installs a crash handler that writes `hwder.log` and minidumps
  beside the exe, and has no network code (the runtime links no network library).

What this does not prove: an audit cannot rule out a compromise of GitHub itself or of the
upstream projects, or a malicious change by someone with write access to this repository that
also regenerated the manifests. The reproducible `HWDER Setup.exe` narrows the last case: a
tampered program would not match an independent build. The prebuilt Linux pieces (the Setup
window and the static SDL3, FFmpeg and libcrypto libraries) were not rebuilt for this audit.

## For maintainers: reducing false positives

In rough order of effect per effort:

1. **Report each detection as a false positive.** Microsoft:
   <https://www.microsoft.com/wdsi/filesubmission> (Software developer, attach the zip or exe).
   Other vendors have similar forms; ask the reporter which product it was. Do not upload game
   files or keys.
2. **Stop starting PowerShell with `-ExecutionPolicy Bypass`.** Setup already contains a WinHTTP
   client. Fetching and checking the embedded Python in C++ and starting `python.exe` directly
   would remove PowerShell from the normal flow.
3. **Give `HWDER Setup.exe` a version resource, an application manifest and an icon,** and strip
   the DWARF debug sections (about 500 KB of the 2.1 MB). Unsigned files with no publisher or
   version information score worse.
4. **Sign the program.** Microsoft's comparison
   (<https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options>):
   Azure Artifact Signing costs about $9.99 a month but is open to individual developers only in
   the USA and Canada; an OV certificate costs about $150-300 a year and needs a hardware token or
   cloud HSM; EV no longer skips SmartScreen. The SignPath Foundation signs open-source projects
   for free from CI builds (<https://signpath.org/>), subject to its terms. Signing does not give
   instant trust: reputation builds across releases signed by the same publisher.
5. **Publish the SHA-256 in the release notes** and keep this page linked from them.
