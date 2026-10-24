# Windows setup window: one screen, one button (R5)

Design note for the R5 setup UX, 2026-10-21. It replaces the R4 button row
(Check requirements / Install private tools / Prepare game / Play / shortcut /
folder) with a single main button whose label follows the state of the
installation. Everything the R4 window could do is still reachable under
**Advanced**. Backend contracts (private tools, publication acknowledgement,
Play lifecycle, settings merge) are unchanged.

## Screen

```
Let's get HWDER ready / Ready to play
Use your own game dump and keys. They stay where they are; nothing is copied.

Game file (.nsp)      [ path                                   ] [Browse…]
Your prod.keys file   [ path                                   ] [Browse…]

Status sentence (what is happening, or what to do next)
Step 4 of 6 · Building the game · 312 of 1480 files · about 6 min left
[###########################                         ]

[ Primary ]  [Cancel]  [Create desktop shortcut] [Add to Steam]  [Advanced ▾]
             (the two shortcut buttons appear once the game is ready to play)
--------------------------------------------------- (Advanced, collapsed)
Data folder           [ path                                   ] [Choose…]
Installed game: <Data>\play\hwder.exe
[Check requirements] [Install private tools] [Open game folder]
[Open data folder] [About tools] [x] Details
[ details log                                                      ]
```

## States and the primary button

The window computes one `Step` from the inputs and the last backend result.
The label, the default button and the title all derive from it.

| Step | When | Label | Click runs |
|---|---|---|---|
| Inputs | NSP or keys box is not an existing file (normal mode) | **Next** | Opens the file picker for the first missing file. |
| Install | Data folder has no `managed-tools\*\complete.json`, or the last failure said the tools are missing | **Install** | Consent dialog, then the private tool download/verify (`tools`). |
| Prepare | Inputs and tools present, no verified prepared game | **Prepare** | `prepare`: verifies tools, checks requirements, extracts, generates, builds, publishes. |
| Retry | Prepare failed with a recoverable error | **Retry** | Same as Prepare. |
| Play | `last-success.json` verified for this Data folder | **Play** | `launch`. Disabled while the verified game process runs. |
| Busy | Any helper running | previous label, disabled | Cancel is enabled except during publication and launch. |

Rules kept from R4: Play is disabled while the game runs and comes back after
exit; closing Setup never terminates the game; only `nsp_path` and `keys_path`
are merged into the installed `hwder_settings.ini`; keys and game files are
never copied; no Python or Visual Studio is needed; the kit contains no
game-derived bytes and exactly one code-free EXE.

Startup: if the Data folder has `last-success.json` the window runs the quick
`status` verification and lands on **Play**. NSP and keys come from
`<Data>\setup-inputs.json` (written only by a successful Prepare, so the paths
bound to the prepared game always win) or else `<Data>\setup-prefs.json`
(written when Install or Prepare starts, or a file is picked). A Data folder
chosen through Advanced › Choose… is remembered in
`%LOCALAPPDATA%\HWDER\setup-prefs.json`; `--data` on the command line still
overrides it, and the test harness never writes outside its own Data folder.

Prepare performs its own checks. The R4 gate "Check requirements before
Prepare" is gone; **Check requirements** under Advanced is an optional
diagnostic that reports the same checks without preparing anything.

## Progress and time estimate

Backend `stage` events carry `step`/`steps` (1..6: Checking requirements,
Checking your game, Preparing game files, Preparing game code, Building the
game, Finishing setup). The window shows "Step k of 6 · <stage>". While
Ninja builds, its `[n/m]` lines drive a real progress bar and a remaining-time
estimate from the measured rate (shown once at least 20 files have compiled).
Other stages use an indeterminate bar with elapsed time. No estimate is shown
when there is nothing to measure it from.

## Error texts

The bridge's `failed` event carries `next` (`inputs`, `install`, `prepare`,
`close`) and a short message; the window shows the message and moves the
primary button to the matching step.

| Condition | Message | next |
|---|---|---|
| NSP is an update/bundle or wrong title | This NSP has an update or is not the base game. Use the base game only, then Prepare again. | inputs |
| Unsupported module set / version | This dump is not the supported game version. Use the unmodified base game. | inputs |
| Keys cannot decrypt | Your prod.keys cannot open this dump. Use the keys from the same console. | inputs |
| Original NSP/keys moved | Your game or key file moved. Pick it again, then Prepare. | inputs |
| Private tools missing or changed for this kit | Private tools are not installed for this kit yet. Choose Install (about 260 MB, no admin). | install |
| Private interpreter not ready (PowerShell stage) | Private tools are not installed yet. Choose Install. | install |
| Game running during Prepare/Play | Your game is running. Close it, then try again. | close |
| Build/compiler failure | The build did not finish. Open Advanced › Details for the error, then Retry. | prepare |
| Cancelled | Cancelled. Nothing was changed; choose Prepare to continue. | prepare |
| Anything else | Setup did not finish: <one line>. Fix that, then Retry. | prepare |

Saves and settings are never touched by a failure.

## The kit tracks main

`tools/kit_baseline.txt` holds the full SHA of the `main` commit whose runtime the
kit is meant to build. `tools/make_setup_dist.py` refuses to package when that
commit is not an ancestor of the source checkout's HEAD, and records it as
`baseline` in `setup-manifest.json`. Base-kit project files (`runtime/`, `tools/`,
`docs/`, top-level files) that the current source no longer tracks are dropped
from the kit. Bump the baseline whenever the kit branch merges a newer main.
R5 (`1cd3c11`) was packaged from a branch that predated main's whole-block GI
blending fix (`171dbef`, `9f0dd02`), so a fresh Prepare rebuilt the jagged
120 fps flashes; R6 merges main `f9e061a` and adds this guard.
