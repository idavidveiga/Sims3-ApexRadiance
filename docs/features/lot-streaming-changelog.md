# Lot Streaming research and refinement changelog

**Branch:** `feature/lot-lod-streaming`  
**Research window:** 2026-10-02 through 2026-10-04  
**Current validated baseline:** Lot LOD distance **300** + Max Active Lots **16**  
**Game build used for the controlled probes:** EA 1.69.47.024017

This document records the Lot Streaming work separately from the general Apex Radiance changelog. It describes what was
measured in the game, which native fields were changed, how the current implementation works, which parts are independent
Apex research, and which parts share lineage with Sims3SettingsSetter (S3SS).

## 1. Goal

The original problem was not simply "increase draw distance". The Sims 3 has several distinct decisions in the lot-detail
pipeline:

- whether a lot is eligible for Detailed View by distance/score;
- how many lots may remain detailed at once;
- how aggressively multiple LoD transitions are allowed to start;
- whether camera motion / viewing angle affects those transitions;
- how a lot's objects are built after the lot is promoted.

The work therefore separated **eligibility**, **capacity**, **transition throttling**, **visibility stability** and
**per-object loading** instead of treating them as one setting.

## 2. Native fields and functions verified

The controlled EA 1.69 probes resolved and exercised the following native path:

| Item | EA 1.69 address / field | Meaning in the current research |
|---|---:|---|
| LotLodScoring | `0x00C6B610` | Native lot scoring path used before Detailed View decisions. |
| Lot metric function | `0x00C62110` | Produces the lot visibility/distance metric observed by the probe. |
| LotLodScoring metric CALL | `0x00C6B957` | Verified single CALL used to bind the metric safely. |
| Camera-bias branch | `0x00C623A5` | Short JZ used by the lot-visibility camera bias. |
| LotDetailRequest | `0x00AC1830` | Promotion/demotion request path logged by the probe. |
| WorldManager global | `0x01246C54` | Live WorldManager pointer. |
| WorldManager + `0xDC` | live field | Lot LOD distance. Native observed baseline: 70. |
| WorldManager + `0xE0` | live field | Active Lot Bias. Observed value: 8.0. |
| WorldManager + `0xE4` | live field | Max Active Lots. Observed/default test baseline: 8. |
| WorldManager + `0xE8` | live field | Terrain-height threshold. Observed value: 600. |
| WorldManager + `0xEC` | live field | Camera speed threshold. Native observed value: 32. |

The probe identifies the lot pointer from the metric's arguments at runtime and refuses to guess when the expected relation
cannot be proven.

## 3. Lot LOD distance: 70 -> 100 -> 200 -> 300

### What was changed

The test probe temporarily overrides only `WorldManager+0xDC`, after first proving the expected baseline. The write is
guarded, maintained only while Apex still owns the value, and restored on a clean unload.

### What the tests proved

The native metric behaves as a squared-distance eligibility test for the controlled cases:

- distance 200 produced a cutoff around **40,000 = 200^2**;
- distance 300 produced a cutoff around **90,000 = 300^2**;
- at 300, a lot with metric **87,798.671875** remained Detailed View ON (~296.31 distance);
- values clearly above 90,000 remained outside the distance eligibility range in the controlled sparse-area test.

OFF states below the cutoff are not evidence of a different distance formula: the independent active-lot capacity and
priority rules can keep an otherwise eligible lot out of Detailed View.

### Current decision

**300 is the current validated Lot LOD distance baseline.** No larger value is being adopted at this stage.

## 4. Max Active Lots: 8 -> 16

### What was changed

A second guarded test override was added for `WorldManager+0xE4`.

This is the native **Max Active Lots** capacity field. It is separate from the S3SS setting named
`Throttle Lot LoD Transitions Max Active Lot Threshold`.

### What the tests proved

With distance 300 and the native capacity still at 8, dense areas repeatedly saturated at eight Detailed View lots.

With only the capacity changed from 8 to 16:

- WorldManager read back **Max Active Lots=16**;
- the same dense reference camera position that had previously saturated at exactly eight lots produced **16 consecutive
  Detailed View ON** lots;
- the 17th and later candidates could remain OFF even while inside distance 300, proving the capacity/priority stage is
  independent of distance eligibility.

### Current decision

**16 is the current validated Max Active Lots baseline.** We are deliberately not testing 24/32 now. The goal is
refinement and stability, not maximising the number indefinitely.

## 5. Memory and stability observation

The 300 + 16 run showed no crash, fatal error or runaway allocation pattern in the recorded test.

During a heavier part of the run:

- private committed memory reached about **1566 MB**;
- roughly **1994-2007 MB** of address space remained free;
- the largest free block remained about **1790 MB**;
- later the private value returned to roughly **1285-1286 MB**.

This is not a universal memory guarantee for every world/save, but it is enough to keep 300 + 16 as the current practical
baseline for further refinement.

## 6. How the current Lot Streaming pieces work

### 6.1 Smooth Lot Streaming

Apex can enable the game's native **Throttle Lot LoD Transitions** mechanism and maintain a camera-speed threshold. It does
not replace the game's lot loader.

The production feature uses camera threshold **5.0**, while the native value observed by the probe is 32.0. The controlled
300 + 16 A/B validated 5.0 together with the native transition throttle: transition rate fell from 149.3 to 99.2 per minute,
same-lot reversals within 5 seconds fell from 87 to 18, and reversals within 2 seconds fell from 48 to 3. The 5.0 value is
therefore the current validated production baseline.

### 6.2 Transition Max Active Lot Threshold = 12

The feature `LotActiveThreshold` resolves the live setting
`Throttle Lot LoD Transitions Max Active Lot Threshold` and currently applies 12.

This value controls the native **transition throttle policy**. It is **not** the same field as
`WorldManager+0xE4 Max Active Lots`.

The value 12 comes from the S3SS LotStreamingOptimizations implementation. It remains implemented for development/reference,
but it is **not exposed in the main Performance menu** and it is not part of the validated 300 + 16 production baseline.
The user-facing capacity control is WorldManager+0xE4 (**Maximum detailed lots**, 8-16).

### 6.3 Keep Lot Visibility Stable

The game's metric contains a camera-view bias controlled by a short conditional branch. The S3SS behavior changes the
branch from JZ (`0x74`) to JMP (`0xEB`), so lots are not promoted/demoted purely because the viewing angle changes.

Apex implements the same behavior with its own address resolution, ownership detection and safe restoration rules.

### 6.4 Pause Lot Streaming in Map View

S3SS implements a map-view blocker around the same native `WorldManager+0x258` "skip lot streaming" gate.

Apex shares the feature goal but the standalone implementation is different: it does not detour
`WorldManager::Update`; it maintains the live gate from Apex's existing pump while map view is open and for a short
exit grace period, then restores the previous value safely.

### 6.5 Spread Lot Objects While Loading

This is **not** a Lot LOD distance/capacity setting. It runs later, when a promoted lot builds scene objects.

The feature has direct S3SS lineage and is explicitly a port/adaptation of
`LotStreamingOptimizations.objectThrottle` into Apex's framework:

- intercept `Lot::AddLotObjectsToScene`;
- build regular objects in small continuation windows;
- use the game's remote-method marshal for continuations;
- keep large objects / outdoor flora synchronous in the first window because one-shot lot activation fixups require
  them to exist immediately.

## 7. S3SS provenance: what is shared and what is Apex work

The public S3SS source was compared against the current standalone Apex implementation so the provenance is recorded
explicitly.

| Area | Relationship to S3SS | Current authorship/provenance statement |
|---|---|---|
| Native TS3 Lot LOD metric / fields | Same game code is available to both mods | **EA game code**, not owned by either project. |
| Distance 300 research and cutoff proof | Not part of the current public S3SS LotStreamingOptimizations settings | **Apex research/probe work.** |
| Max Active Lots 8 -> 16 at WorldManager+0xE4 | Distinct from S3SS's threshold 12 | **Apex research/probe work.** |
| Metric argument identification, 200^2/300^2 validation, diagnostic logging | No equivalent used for these controlled tests in the compared S3SS patch | **Apex diagnostic/research work.** |
| Transition throttle + threshold 12 | S3SS implements the same native live settings and uses 12 | **Behavior/value lineage from S3SS; Apex standalone wrapper/resolver/ownership code is its own implementation. The threshold-12 control is retained only for development/reference and is not in the main menu.** |
| Camera speed threshold 5.0 | S3SS exposes the same setting with default 5.0 | **Behavior/default lineage from S3SS; validated by Apex's final 300 + 16 A/B and used by the production Smooth Lot Streaming feature.** |
| Visibility JZ -> JMP | Same patch behavior is present in S3SS | **Behavior/patch lineage from S3SS; Apex adds its own validation/ownership/restoration layer.** |
| Map-view blocker | Same feature goal and native skip gate | **S3SS lineage for the feature; Apex standalone implementation differs and avoids the S3SS WorldManager::Update detour.** |
| Object throttle | S3SS public implementation is the source lineage | **Port/adaptation from S3SS into Apex EntryChain and safety framework.** |

### Does standalone Apex contain S3SS code?

The accurate answer is **not "none at all" and not "the Lot Streaming code is just S3SS"**.

- The standalone Apex framework was rewritten and does not carry wholesale S3SS framework files or S3SS file headers.
- Several Lot Streaming features deliberately reproduce S3SS behavior because S3SS already identified useful native
  controls.
- **Object throttle is explicitly a port/adaptation from the S3SS feature and therefore has direct source lineage.**
- The visibility override and the 12 / 5.0 streaming-setting choices also have direct S3SS behavior lineage, while Apex
  supplies its own integration, validation, ownership and restoration machinery.
- The new **Distance 300**, **Max Active Lots 16**, metric probe, squared-distance validation and the controlled EA 1.69
  tests are Apex-side research and are not presented as S3SS work.

This distinction should be preserved in future public documentation and credits.

## 8. Current production baseline

The validated research values are now in the normal production path, without the metric probe:

- **Extended Lot Detail**: production-capable on EA 1.69, but in the current v2.6.0-integrated build it is **opt-in** (`enabledByDefault = false`).
  - **Lot detail distance**: range **70-300**, control default **300**, snapped in 10-unit steps.
  - **Maximum detailed lots**: range **8-16**, control default **16**, one-lot steps.
  - Apex captures the native values per live WorldManager, writes only guarded expected values, stops maintaining a field
    if another owner changes it, and restores only fields it still owns.
- **Smooth Lot Streaming**: validated/recommended companion behavior, but current registration is **opt-in** (`enabledByDefault = false`); when enabled it uses native transition throttle ON + camera threshold **5.0**.
- **Keep Lot Visibility Stable**: optional/experimental; camera-angle bias JZ -> JMP with ownership-safe restoration.
- **Pause Lot Streaming in Map View**: optional/experimental; uses WorldManager+0x258 while the map is open plus a 1 s exit grace.
- **Object streaming** remains separate from lot eligibility/capacity:
  - **Spread Lot Objects While Loading**: optional, default **2 regular objects per window** with **16 ms** between windows;
  - **Spread New Objects Over Frames**: optional Scene Node Budget applied later in the scene pipeline.

Active Lot Bias remains **8.0** and the terrain-height threshold remains **600**; neither is changed by Extended Lot Detail.

## 9. Deferred / separate research

The validated Lot Streaming pass is complete. Items intentionally outside this pass are:

1. **Objects/flora visual distance:** scene/object LOD research, separate from lot Detailed View eligibility.
2. **Threshold 12 experiments:** the S3SS-derived internal transition threshold remains available only for development/reference;
   it is not a normal user-facing control.
3. **Higher-than-validated ranges:** values above distance 300 or more than 16 detailed lots are deliberately not exposed until
   separately tested for stability and memory behavior.

## 10. Production integration after validation

After the final controlled OFF vs Smooth comparison, the research baseline was promoted into normal Apex code:

- `WorldManager+0xDC` is now managed by the production **Extended Lot Detail** feature, default **300**.
- `WorldManager+0xE4` is managed by the same feature, default **16**.
- Both settings are persisted in `[patches.LotDetailRange]` and can be changed from the Performance menu within the validated
  ranges: distance **70-300** (10-unit steps) and capacity **8-16**.
- The production implementation does **not** install the metric/scoring/detail-request diagnostic hooks.
- Smooth Lot Streaming is validated as the normal companion behavior: native transition throttle ON + camera threshold 5. The current v2.6.0-integrated registration remains opt-in.
- The misleading threshold-12 switch is no longer shown in the main menu. The underlying diagnostic feature was kept for development/reference.
- Lot-object throttling and Scene Node Budget are presented separately under Object streaming.

Final A/B in one game session (300 + 16 held constant):
- everything OFF: 268 transitions over ~107.7 s = **149.3 transitions/min**; 87 same-lot reversals <=5 s; 48 <=2 s.
- Smooth ON: 190 transitions over ~114.9 s = **99.2 transitions/min**; 18 same-lot reversals <=5 s; 3 <=2 s.
- approximate reduction: **34%** transitions/min, **79%** <=5 s reversals, **94%** <=2 s reversals.

## 11. Final upstream 2.6.0 integration state — 2026-10-06

The fork was rebased conceptually onto the **published final Apex Radiance 2.6.0** while preserving the validated fork-only Lot Streaming work.

- Upstream final tag: `v2.6.0`, commit `9ca0b102d4ee0f90f5f4fe406d97ab0f3f5dca9a`.
- Post-release experimental `feature/color-filters` work was **not** imported.
- Where the fork and upstream both carried intermediate EA 1.69 implementations of the same lighting subsystem, the final upstream implementation won; `features/level_light_share.cpp` is intentionally the official 2.6.0 version.
- Fork-only Lot Streaming modules remain present: `lot_detail_range`, `lot_lod_streaming`, `lot_object_throttle`, `lot_visibility_override`, `lot_active_threshold` and the diagnostic probes.
- x86 Release validation after the integration passed in workflow run `37414470615`.

### S3SS comparison snapshot

The detailed provenance review was refreshed against public Sims3SettingsSetter main `5eb2c65bb11e21dac423731c9726627f1fb118ac` and `patches/lot_streaming_optimizations_patch.cpp` blob `def668c514230dd83855c4b2c7e7af6e6e3b7add`.

The durable classification is:

- **Apex research:** distance 300, `WorldManager+0xE4 = 16`, metric argument/cutoff probes, 200²/300² validation, controlled A/B, ownership of the new distance/capacity fields.
- **S3SS direct source/algorithm lineage:** per-lot Object Throttle.
- **S3SS behavior lineage, Apex reimplementation:** transition throttle settings, camera threshold 5, visibility JZ->JMP and map-view blocking.
- **Not copied from S3SS:** its general patch framework, LiveSetting framework, UI/config system, and its `WorldManager::Update` map-view detour.

For future ports, use `docs/Lot_Streaming_Implementation_Guide.md` as the implementation handoff and this file as the research/validation history.
