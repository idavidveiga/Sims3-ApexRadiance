# EA App 1.69 — CAS native diagnostic smoke test

**Diagnostic only, not a finished CAS speedup.** This procedure checks whether the native Apex module loads and whether the EA game's *loaded* executable has readable machine code. It does not install a CAS hook or modify UI.dll.

## Build source

Workflow: [Apex Win32 CAS diagnostic build](https://github.com/idavidveiga/Sims3-ApexRadiance/actions/workflows/apex-cas-win32-diagnostic.yml)

- Build Release Win32 with the project's regular ABI safety gate.
- The two experimental Mono-related switches are disabled in ordinary builds and are excluded from the Overview bulk switch.
- The read-only developer inspection remains available.
- Artifact: `ApexRadiance-CAS-Diagnostic-Win32` (download from a **successful** workflow run; verify the artifact's commit SHA before testing).
- Build success checks only compilation. No executable of The Sims 3 runs in GitHub Actions.

## Local diagnostic procedure

1. Close The Sims 3 and EA App game processes; back up the existing `Game/Bin/ApexRadiance.asi` if present. Do not replace the game's TS3.exe or UI.dll.
2. Extract the diagnostic `ApexRadiance.asi` from the workflow artifact. Install it the same way as the current Apex module, replacing **only that ASI**.
3. Enable `Settings → Menu → Settings and maintenance → Enable developer mode`, confirm, **restart the game**, then load a test world. The Apex menu default shortcut is `Ctrl+Shift+F11`.
4. Open `Developer → Performance → CAS native runtime inspection`.
5. Click `Inspect loaded TS3.exe (read-only)` once and note the full result, including entropy and page protection.
6. Close the game normally. Collect `Documents/Electronic Arts/The Sims 3/Apex Radiance/ApexRadiance_LOG.txt`. Look for `[TS3 Mono Runtime Probe]`.
7. Restore the previous ASI if you do not plan further controlled tests. Developer mode can be turned off and requires restarting to take effect.

## What constitutes a valid result

- The inspector reports the recognized EA build and a readable image section, or a clearly stated read failure.
- It **never** reports a verified JIT method entry point; this first probe cannot establish one.
- If the memory still appears packed/high-entropy, stop native hook research for that executable until its runtime state can be legitimately characterized.
- If the memory differs from the disk image, treat any historical byte signature as *an observation only*. Verify method identity and the ABI separately.
- No CAS part loading speedup should be expected in this build. The scheduler C++ core remains unconnected to the game's UI.dll.

## Compatibility boundaries

No external method-patching framework, no second script mod, no injection into another application, no altered executables. Work only on the player's own unmodified game and normal Apex ASI installation.

**Do not enable the ABI-unverified compilation flag on a game installation.** It is for independent developer research after native calling conventions have been proven, not an end-user setting.
