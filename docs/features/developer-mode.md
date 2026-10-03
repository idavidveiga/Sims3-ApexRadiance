# Optional developer mode — unified build

## User flow

Settings > Menu contains Enable developer mode. Default false; stored as `[ui] developer_mode` in ApexRadiance.toml. Enabling opens a confirmation with Cancel on the left and Enable on the right. It explains temporary diagnostic visuals, performance costs of measurements/extra checks, local paths and session details in reports, and that nothing is uploaded automatically. Cancel leaves the setting unchanged. Both enabling and disabling require restarting the game; a note distinguishes requested mode from the current session.

All code ships in one `Release/ApexRadiance.asi`. The old ApexPublic build property has no effect. Developer mode is loaded before constructing features and before starting developer instruments. The legacy kPublicBuild is an atomic runtime flag (true = normal mode). The Developer sidebar item, feature developer UIs, additional checks/logs, address-space monitor, profiler activation and Frame Capture follow it. The ordinary Report a problem tools remain available to all users. Debug views of edge smoothing/depth blur are ignored in normal mode even when an old configuration retains their values.

## Profiles

Development is a new optional profile part (bit 8; previous bits unchanged). The save checkbox is hidden unless developer mode is requested/active. Profiles containing it expose it when selecting what to load, even in normal mode. It is unchecked by default on import. Loading Development that requests activation opens the same confirmation before any profile part is applied. Cancelling applies nothing.

The section stores mode choice, feature preference snapshots (without feature enabled flags), lighting diagnostic views/sample preferences, compression/cache/index verification frequencies, texture-worker settings, scene-node budgets, wall-shading wait and profiler tuning/sampling preferences. Imported preferences are deferred until startup if required. Mode activation never follows an ordinary profile that lacks this section.

Recordings, frame captures, running profilers, one-shot census/rebuild operations, temporary verify-all timers and accumulated measurements are actions/session results and are not restarted by profiles. Profiler enabled is explicitly false in the exported profile. A diagnostic view may be restored in developer mode; the confirmation warns about temporary image changes.

## Navigation

FXAA (recommended) is the first method, followed by SMAA. Reordering does not change stored enum values or the chosen method. Anti-aliasing is the first Display tab, Window the second.

## Validation

Compile the unified configuration, check that the legacy ApexPublic property selects the same output and definitions, test profile part filtering/old masks, default-off startup gating, cancellation and confirmation policy, and developer preference round trips. Offline checks do not validate actual game performance or UI layout; verify these in game before releasing. No installation or publication is part of this change.

Offline profile checks passed: default-off UI setting/runtime gate, unchanged old bit positions, developer-only/normal/empty filtering and old profiles without the new part. The unified Release build passed. In-game confirmation layout, restart behavior, profile round trips and performance remain to be verified.
