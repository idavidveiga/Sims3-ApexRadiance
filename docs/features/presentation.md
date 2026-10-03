> Current RC, 2026-10-02: window modes, monitor selection, Apex FPS/V-Sync control and driver VRR detection were removed at the user’s request. This document is historical. Apex now forwards the game’s original CreateDevice/Reset/Present requests without changing synchronization or window state. G-SYNC/FreeSync remains controlled by the driver/monitor; active VRR in this game is not asserted.

> Private RC update, 2026-10-02: approved proposal 3 is implemented as Display and fluency, with window/monitor selection, read-only VRR reports and conditional conflicts. See [display-fluency.md](display-fluency.md). No installation/publication or universal multi-monitor FPS fix is implied.

# Presentation and frame pacing — private RC

Introduced in 2.5.4-rc-presentation-test. One unified binary. This is a presentation policy and optional FPS limiter, not a renderer replacement or a driver VRR activation API. Gameplay/monitor validation is pending.

## Settings (Display > Window, below Borderless)

| UI | TOML `[display]` key | Default | Range | Application |
|---|---|---|---|---|
| V-Sync | vsync_policy | 0: Keep current | 0 preserve, 1 On, 2 Off | next successful device creation/reset; restart recommended |
| Limit FPS with Apex | frame_pacing | false | boolean | live, subject to S3SS conflict gate |
| Target frame rate | target_fps | 120 | 30–240, integer | live |

The existing Window profile category includes these fields. Legacy profiles without them preserve the current synchronization values. Page reset and Restore synchronization restore policy 0, pacing false, target 120. No configuration of another mod is edited automatically.

## Implementation

`features/presentation.cpp` applies the requested D3D9 PresentationInterval independently of borderless ownership. Bootstrap fallback retains the game's parameters if device creation/reset fails. The UI compares its request with the successful device parameters and shows restart pending when they disagree. This reflects D3D9 requests only; DXVK and driver overrides can supersede them. Keep current preserves each game device request.

The device Present hook invokes the optional scheduler after the existing callback chain and before original Present, outside the registry lock. Disabled pacing returns immediately. Enabled pacing uses QPC deadlines and a render-thread-owned high-resolution waitable timer, with ordinary waitable timer/Sleep fallback. No spin loop, system timer-resolution change, worker thread, per-frame disk writes or frame catch-up bursts. Wait chunks are bounded to 4 ms and failures stop the current schedule. Off/focus loss/minimization, configuration changes, successful device creation/reset and failed Present restart scheduling. Long stalls resynchronize rather than replaying deadlines. The target is a ceiling, not a promise of exact frame times or sustainable FPS. The limiter also affects foreground loading frames if configured; background limiting remains external.

Visible Apex wait and Present values are last-call CPU durations, not GPU scanout measurements, VRR status or percentile statistics. GPU/shader/lighting behavior is unchanged.

## Compatibility

At device creation/reset and first installed window procedure, Apex caches whether loaded S3SS has SmoothPatchPrecise enabled with positive frameRateLimit in its configuration. When detected, Apex's pacing control is disabled and no pacing waits execute, including when enabled by a profile. The gate is conservative configuration evidence, not introspection of S3SS runtime state. Restart after changing the S3SS FPS setting. Its inactive FPS limit and TPS/tick controls are retained. Unknown driver/RTSS/DXVK limiters cannot be reliably inferred from this check: use only one limiter.

Local investigation found S3SS active FPS 237, inactive FPS 60, TPS 960 and DXVK d3d9.presentInterval=1. These files were not changed by this feature. The previously authorized window handoff changed only the two borderless mode keys. DXVK can force V-Sync regardless of the Apex request. G-SYNC/FreeSync must remain enabled through the supported driver/monitor path. This RC does not guarantee OLED flicker elimination.

## Verification

Required in-game A/B: existing RC versus this RC with defaults, then one limiter at a sustainable target. Test original D3D9 and DXVK, focus/Alt+Tab, load screens, reset/device loss, menu open/closed, profile import and reset, V-Sync requests with/without renderer overrides. Measure frame-time percentiles and visible flicker, not average FPS alone. Do not label compilation as gameplay validation.
