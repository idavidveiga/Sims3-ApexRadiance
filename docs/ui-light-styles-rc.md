# Night Lighting: ready balance layout — current RC

The approved first proposal is implemented in the Lighting Overview tab: a separate Night Lights master card, three intensity choices (Subtle / Soft / Natural), a custom/current status, expandable scope explanation, explicit Undo choice, and a short pointer to the surface tabs. Main controls remain visible. Violet spacing and Lucide icons are retained; EN/PT/ES/FR text is supplied. The duplicate whole-Night-Lights reset button and its unused API were removed; the existing page-defaults control remains.

Soft retains exactly the approved ten-value reference: ground .75, roads 1, street lamps .8, lot lamps .8, objects .75, pieces .75, fences .75, walls 1.5, roofs .45, water .3. Subtle retains the previous -10% variation; Natural retains original defaults. Soft Plus and Balanced selector entries were removed. Existing numeric profiles are not rewritten or coerced: unmatched values display Custom. No choice is applied on load. Color/moonlight legacy registered parameters still serve existing profiles and runtime lighting; they are not dead code and were preserved.

A selection changes the same ten surface intensities only. Room background light, colors, moonlight, switches, reflection settings and performance stay unchanged. Explicit choice undo restores all ten values present before the selection, including custom values, and uses the existing live-update/save path. Lighting solvers and terrain-edge corrections were not edited.

## Retired display runtime

The former Display/fluency page and all three modules (`borderless`, `presentation`, `display_monitor`) were deleted, together with their project entries, unused vendor SDK headers and dedicated fixture. S3SS FPS/window-specific detection helpers were removed; general compatibility and split-floor detection remain. Bootstrap now forwards original game presentation parameters, and Present has no Apex FPS wait. Old `[display]` config values are retained inert on general TOML save but are neither applied nor exported in new profiles. Reserved profile bit 4 stays unused so Performance/Shortcuts/AO/Development do not shift. Edge Smoothing remains as its own sidebar/search/defaults page; confirmed MSAA conflict warnings remain.

Removing an Apex controller does not enable VRR in the driver or certify actual game scanout. It removes Apex’s policy changes and extra FPS waits, preserving the game/renderer/driver presentation path. Nothing is installed or published automatically; gameplay validation remains necessary.

Validation (2026-10-02): Release x86 compilation, translation-table checks and native legacy profile filtering checks passed. Gameplay validation remains pending. Candidate binary SHA-256: 75EFF7A869E709D9D1C9698FF00CEC5987D693F69FF2E43DA36BF2D20F8B203B.
