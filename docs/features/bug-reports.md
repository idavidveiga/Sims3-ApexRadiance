# Report a problem: captures for bug reports

> Since 30/09 (after 2.4.0) the lighting recorder (F6), the light probe (F7) and the light diagnostics (F8), which were
> development-build tools, are in both builds, renamed for players, and gathered on one menu page, **System › Report a
> problem**, with plain explanations. Every capture goes into its own dated folder that is never overwritten; capture
> sessions gather several captures in one folder; the page lists the saved captures with Open and Delete. Code:
> `features/captures.{h,cpp}` (folders, notes, sessions, list), `apex_gui.cpp` (`ReportPage`, `CaptureNote`).

## What players see

- **Sidebar › Report a problem** (icon: bug), three cards:
  1. **How to report a problem**: three numbered steps (make it happen, save a capture or press its key, zip the folder
     and send it on Nexus Mods' Bugs tab or GitHub), a tip to press Compare with the game to tell whether it is the mod,
     and a warning when `ApexRadiance_Crash.txt` was written in the last 7 days.
  2. **Save a capture**: one row each, with what it is good for and its key:
     - **Save a report**: the log and settings only (any problem, crashes);
     - **Record a few seconds** (Start / Stop; key = the shortcut set's recorder key, F6 / X / 6);
     - **Capture the light at a spot** (key only: the mouse must point at the spot with the menu closed; F7 / V / 4);
     - **Lighting snapshot** (Save; F8 / B / 5);
     - **Capture session**: Start session / End session.
  3. **Your captures**: newest first, "date · time · kind" (sessions: "Session (N captures) · open"), size, **Open** and
     **Delete** (a second click within 4 s confirms); **Open the captures folder** and **Delete all** (same confirm).
- **On-screen notes** (top-left, the start note's pill style, below it when both show, never taking input): while
  recording, a pulsing red dot with the seconds and the key to stop; "Capturing the light under the mouse…"; "Saved in
  Captures › <folder>" / "Added to the session: <folder>" / "Session saved…" for 6 s; while a session is open and
  nothing else shows, "Capture session open · N captures · end it on the Report a problem page".
- **Settings › Menu › Start note** (`[ui] start_note`, default on) turns the start pill off.
- **Settings › Shortcuts** lists the three capture keys under REPORT A PROBLEM (they follow the shortcut set).

## Files

`Documents\Electronic Arts\The Sims 3\Apex Radiance\Captures\`:

| Capture | Folder | Main file |
|---|---|---|
| Report | `YYYY-MM-DD HH-MM-SS Report\` | (only the copies below) |
| Recording | `... Recording\` | `Recording.txt` (as before: the lines sorted by clock time, the toml at the start) |
| Light capture | `... Light capture\` (the automatic follow-ups after a floor change: `... Light capture (automatic)\`) | `Light capture.txt` + the textures (BMP) and shaders |
| Lighting snapshot | `... Lighting snapshot\` | `Lighting snapshot.txt` |
| Session | `... Session\` holding `HH-MM-SS <kind>\` folders | `About this session.txt` (the list) |

Every capture folder also gets, when it is complete (`Captures::Finish`): a copy of `ApexRadiance_LOG.txt`,
`ApexRadiance.toml`, `ApexRadiance_Crash.txt` when present, and `About this capture.txt` (version, game build, time, what
it is, how to zip and send it). A session gets the same copies when it ends. A name already taken gets " (2)", " (3)"...
Nothing is deleted or overwritten by itself (the light probe's old 20-capture pruning and the "latest" copies
`ApexRadiance_LightProbe.txt` / `ApexRadiance_LightDiag.txt` are gone). Delete removes only direct children of
`Captures\`, never the open session.

## Detail in the public build

The recording reads detailed log lines that the lighting modules used to write in the development build only (the solve
journal of `level_light_share.cpp`, the lot lamp change details of `lot_light_bridge.cpp`, the lamp change decisions of
`night_terrain_relight_patch.cpp`, the furniture tracer). They now test `Recorder::Verbose()` (= development build, or a
recording running), so a player's recording has them too and the public log stays quiet otherwise. Only log lines changed:
no lighting behaviour depends on them.

## Shortcuts

`hotkeys.cpp`: the public build now takes Recorder, Probe and Diagnostics (Frame Capture stays development-only). Action
names for players: "Recording", "Light capture", "Lighting snapshot".
