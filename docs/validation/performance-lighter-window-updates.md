# Lighter Window Updates: validation

The feature is described in
[features/performance/lighter-window-updates.md](../features/performance/lighter-window-updates.md). It must:

- Change only the short jump over the per-frame `InvalidateRect`, after checking the bytes around it.
- Leave the paints Windows sends by itself (uncovering, resizing) to the game's handler, and write `75` back on Stop.

## Automated tests

None offline.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-05 | `525ba0f` | Disassembly of Steam TS3W.exe | No compare or push of the paint event id 0x1EE100A anywhere in the executable | None |

## In-game test plan

1. Start a world. **Expected log:** `[WindowRepaint] On: the game no longer invalidates its window every frame`.
2. Windowed mode: cover the game window with another window, then uncover it; resize it. **Expected:** the picture is
   redrawn normally.
3. Several monitors in borderless mode. **Expected:** the other monitors look as before.

## Confirmed in game

- Nothing recorded at publication.

## Open checks

- The measured effect on frame time.
- Overlays or capture tools that rely on `WM_PAINT`.
