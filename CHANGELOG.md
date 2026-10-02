# Changelog

## 2.5.4 — 2026-10-02

- Prioritize known visible-lamp colour, intensity and activation changes, including small intensity adjustments and repeated switches.
- Refresh newly visible lots through bounded local terrain updates and clear disabled lot-owned lamp contributions without waiting for fade completion.
- Correct the verified summer multi-pass terrain-light path so lighting remains consistent across lot/world boundaries.
- Coalesce rapid edits and retain bounded terrain scheduling, coordinated floor lighting and Rooms at Night recovery.
- Provide page/whole-mod defaults with confirmation and Undo, preserving saved captures, reports and profiles.
- End object-point capture after one click, return to the Report panel after completion and align routine notices at the top center.
- Translate the updated shared controls into English, Portuguese, Spanish and French. Developer binaries remain private.

The maintainer accepted the latest private lighting-response build and reported improved perceived performance. This report does not provide instrumented latency/FPS figures or validate unverified shader variants. See the feature documentation for scope and testing.
