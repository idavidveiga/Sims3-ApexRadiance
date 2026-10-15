# Smooth light maps: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/room-map-smoothing.md](../features/night-lighting/room-map-smoothing.md).

### 2026-10-13: why interior shadows look blocky

**Context:** shadows of walls and furniture on room floors and walls showed texel steps even with *Light detail* High.

**Finding:** three separate causes. The room-lit shaders read the map with the bilinear filter (no InteriorFloor or
InteriorWall shader was patched). The batch solve `0x006A31D0` packs each texel with `0x0060CD10`: x 255 and a 1.5 x 2^23
add (round to nearest), so a slow falloff steps by whole levels. `LightPointWithAllLights` drops a light under
`room+0x63C` at `0x0069FE40` before its wall and occluder tests, a step of 3/255 or 6/255 at the edge of each lamp's reach.

**Outcome:** B-spline reads in copies of the room-lit shaders, a position-keyed dither before the pack, and a smoothstep
fade over the threshold's last step in the wrapped solves.

### 2026-10-13: the room-lit shader family and the wall atlas

**Finding:** over the Steam precomp, one plain `texld` of TEXCOORD1 followed by `mad rD.xyz, rL.w, cA, rL` (the ambient
term) matches 186 pixel shaders, all of techniques that read the same room map, none of ExteriorWall (which reads the
atlas with a lamp scale and no ambient term). Their vertex shaders scale the uv by 1/4096 (walls) or 1/32767 (floors). The
wall atlas is 256n x 128n texels with blocks of 3n + 1 rows; a block's row k is drawn at texel `y0 + N - 1 - k`.

**Outcome:** a pattern match (not shader ids); the band clamp from the atlas width; a runtime check of each block's `y0`.

### 2026-10-13: rejected

- **16 texels per metre:** four times the memory and the solve time of *Light detail* High and over the game's 2048 map
  size on large lots; the B-spline gives smooth edges at the existing density.
- **Screen-space shadows:** the per-frame relighting they need was removed earlier for its cost and flicker.

### 2026-10-14: ported to 2.12.3 + Real-time outdoor lamps

**Context:** the light through an arch into the next room and the lamp light around wall openings outdoors looked blocky.

**Outcome:** the B-spline reads, the dither and the soft light cut ported with the penumbra from the lamp's size; the GPU
relight of real-time outdoor lamps and the quick outdoor lamp edits take the same soft cut and dither. Each part has a
developer Tester switch. Offline: `tools/room_map_smooth_check` 957 checks, `tools/rt_lamps_test` 22, `tools/outdoor_delta_test`
11, 0 failures. Not yet checked in game.
