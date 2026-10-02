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

## Revision (30/09, after the first test)

- **Crash fixed:** Open / Open the captures folder called ShellExecuteW inside the menu frame; it pumped the game window's
  messages, the overlay's window procedure ran again inside the frame and its std::mutex threw (resource deadlock,
  crash report 22:10:22, `Overlay::ApexWndProc` -> `std::_Throw_Cpp_error`). Explorer is now opened on a short-lived
  thread with COM (`ShowInExplorer`), like the Profiles folder button. Never call ShellExecute from the menu frame.
- **Layout (user: sessions higher, clearer, a nicer look):** the page is now Session (a violet-edged card of its own:
  big icon, three numbered steps and "Start a session"; while open, a pulsing dot with its time and count, the captures
  so far with check marks, "End and save the session" and "Open its folder") -> Save a capture -> Your captures -> How
  to report a problem.
- **Screenshots:** every capture also gets `Screenshot.png` (switch "Include a screenshot", `[ui] capture_screenshot`,
  default on). Taken on the next frame: menu closed = at Present (the picture as shown, Color filters included; the
  capture notes are not drawn that frame), menu open = at `endSceneBeforeOverlay` (before the Apex menu; the Color pass
  comes after the menu, so it is not in that one). Back buffer -> SYSTEMMEM (GetRenderTargetData, a resolve first if it
  were multisampled), BGR 24-bit, encoded with WIC on a short-lived thread. 8-bit back buffers only.

## Guided capture (local preview, 2026-10-02)
The public and private Report page now starts with four plain-language choices: lighting, an object's appearance, a crash, or another/unknown problem. Selecting one starts or reuses a capture session. The guide presents only the relevant action; all existing capture tools, session controls, help and the capture library remain available in collapsed sections. Finishing explicitly ends the session and opens its folder. ZIP creation remains manual; no automatic upload is performed.
The appearance action arms a one-shot light probe and closes the menu. A Violet target follows the mouse and the top-left capture note displays the actual configured probe chord and Esc cancellation. Opening the menu also cancels selection. The marker is visual only, does not identify an object, and does not run scene searches. A guided capture does not schedule automatic floor-change follow-ups; the direct shortcut retains that diagnostic behavior. No target is drawn in capture screenshots. Selection is transient and not saved to TOML. The guide, hint and controls are translated into EN/PT/ES/FR. Gameplay validation of pointer alignment and capture completion remains necessary.

## One-click selection and centered notices (private, 2026-10-02)
This supersedes the guided selection instructions above. A left click while the target is active confirms that client-area pixel, hides the target and instruction immediately, and consumes both mouse down and mouse up so the game does not also select or place an object. The clicked coordinates are queued atomically; only the render thread starts the GPU probe. The existing probe shortcut also confirms selection. Esc, losing focus or reopening the menu cancels selection. This selects a screen pixel, not an object identity.
After a confirmed selection, the Report page reopens once the probe and pending screenshot are complete. It does not open over the captured frame. An idle open capture session no longer keeps a permanent on-screen chip; the session remains available on the Report page. Capture/save acknowledgments remain transient.
Startup, capture, recording and comparison pills share one top-center anchor, 20 scaled pixels below the viewport top, with consistent padding, rounding, background and border. Only one routine pill is displayed there at a time: capture/recording takes precedence over comparison, which takes precedence over the startup hint. Compatibility warnings and first-start interactive notes use the same anchor and suppress routine pills. Captures suppress routine notices and the target in their screenshot. Updated location and click instructions are translated into EN/PT/ES/FR. Build checks do not replace an in-game click/cancel and scaling test.

### 2.5.4 public hotfix scope

The shared Report changes above, page defaults and centered notices are included in the public build. Developer-only diagnostics and profiler controls are excluded. The one-click capture regression checks and English/Portuguese/Spanish/French translation checks passed. The separate simpler recording/saving design remains a prototype; no automatic ZIP generation or upload was added.
