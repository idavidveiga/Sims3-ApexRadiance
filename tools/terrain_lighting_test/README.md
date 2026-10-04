# Terrain lighting regression checks

`-CompositionCapture` optionally accepts the 2026-10-04 11-52-03 F7 directory.
The captured single and multipass shaders run with controlled equal material,
normal, sun and lamp inputs. The fixture reproduces the native pre-material
UNORM range mismatch and compares the daylight candidate against multipass RGB,
including transition weights, no-lamp pixels and original night pixels. This
is not an exact replay of scene vertices, textures, depth or all material layers.

The harness exercises the production policy, shader patch and sampler guard. It
does not attach to TS3, execute game patch writes, change captures or write files.
The runner compiles into `OutDir`; executables print results to stdout only.

```powershell
./tools/terrain_lighting_test/run.ps1 -Captures 'absolute session folder' -OutDir 'absolute test output folder' -ReferenceSource 'previous lot_light_bridge.cpp'
```

`ReferenceSource` is an unchanged source snapshot from the parent commit. Supplying
it enables pixel comparison of the old and current **actual** replacement HLSL.
Without it the policy, native-alpha GPU and captured-shader tests still run, but
night lot-pass equivalence is not tested. The current captured fixture names are
`PS_2BA33118.bin` (world s7) and `PS_2BA62940.bin` (multi-pass s2) from the 2026-10-04
02-05-41 session. Missing fixtures or unavailable native D3D9 cause a failure.

Coverage includes:

- Native zero lamp factors at day; single-pass and squared multi-pass factors,
  101 night levels, 120 gains, 21 native scales, previous night rounding, invalid
  values and all edit-priority combinations.
- Day/night endpoint debounce, reversal cancellation, world-load merging,
  disabled scheduling and 200,000 deterministic stress frames.
- Build preview zero phase delay, preserved Live delay, captured rapid-switch
  timeline replay and a maximum of four nearest priority chunks with a known eye.
- Queue checks extract the actual production `Attach`, `QueueSweep`, `OnPresent`,
  completion/refusal helpers, types and budget/timeout limits. Fake native memory
  and timing cover 3,000 randomized phase handoffs, an old completed bake awaiting
  consumption, nearest-first order, preserved batch ownership, non-mutating sweep
  refusals, native rebuild/render gates, rolling caps and terrain-owner changes.
- Exact sampler-state restoration after every injected read/set failure; no
  redundant sampler writes when states match. Texture getter/setter failures
  restore partial state changes; GPU checks also restore a real nonempty previous
  texture binding on the spare sampler.
- SM2 and SM3 bytecode creation, malformed/truncated/ambiguous/unsupported
  layouts, GPU pixel readback: smoothed RGB retained, solar alpha identical to the
  original native map at every tested pixel.
- Actual captured shader patching and D3D9 shader creation, exclusive c7 lamp
  constant identification, and world-lamp eligibility/streaming policy tests.
- With the reference: 60 GPU scenarios of the production lot HLSL at night,
  compared within one 8-bit LSB, plus day continuity across forced lot-edge weights
  with native window/lot scaling left at zero.
- Extracted wall and regular lot draw paths against D3D9 fixtures: exact wall gain
  and no-op writes, atlas/chunk fallback, failed original-constant reads, texture
  reference balance and restoration of all touched state. This is not a wall
  daylight shader/pixel validation.
- Extracted edit-state functions: native terrain completion before debounce,
  one independent rig request after debounce or completion, coalesced edits,
  later separate edits and exclusion of ordinary lot/automatic requests. An
  optional `EditReferenceSource` enables reproduction of the old lost-request
  behavior from an unchanged `night_terrain_relight_patch.cpp` snapshot.
- Extracted world reset, memo/scan selection, enumeration/tracking and refresh
  scheduling: no old-world lamp rows, real memo invalidation, forced first read,
  failed enumerations, 512 world changes without false edit events and successful
  new-world selection. Optional `WorldReferenceSource` reproduces the old cache
  behavior from an unchanged `lot_light_bridge.cpp` snapshot.

The reported sampler-guard cost is an isolated driver CPU benchmark, not game FPS
or end-to-end lighting latency. A supported smooth terrain draw adds one native
texture read and one alpha move. No smoothing means no native-alpha alias.

Gameplay validation remains necessary: repeated Build mode day/night switching,
move/place/remove/recolour/intensity/on/off edits inside and outside lot borders,
moving camera, lot streaming, unsupported variants, seasons, native D3D9 and DXVK.
Preserved chunk render gates, caps and timeout fallbacks mean instant visual
updates cannot be promised for every scene or driver.

The queue suite can also run separately:

```powershell
./tools/terrain_lighting_test/run_queue_checks.ps1 -OutDir 'absolute test output folder'
```

It executes extracted queue logic against memory/timing fixtures, not the TS3
engine or driver. The printed source hash identifies the queue source tested.
