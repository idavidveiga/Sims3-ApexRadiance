# Lighter Window Updates: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/lighter-window-updates.md](../features/performance/lighter-window-updates.md).

### 2026-10-05: no per-frame repaint of the game window

**Context:** commit `525ba0f`. The window message pump (0x00410890) invalidated the game's window before every pump.

**Finding:** each invalidation sends a `WM_PAINT` through every window procedure, the game's paint handler and a paint
event (0x1EE100A) nothing listens for; the picture comes from the swap chain.

**Outcome:** the short jump over that `InvalidateRect` (0x004108AE) is made unconditional; a new Performance card,
*Game and scripts*; an EA signature added.

### 2026-10-06: Experimental badge

**Context:** commit `7232c3b`.

**Outcome:** marked Experimental in the menu, still on by default. Released in 2.7.0.
