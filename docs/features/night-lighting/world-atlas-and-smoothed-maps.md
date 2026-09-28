# World light atlas and smoothed terrain light maps

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) as described; `lightmap_smooth.cpp` changed after v0.1.0
> only in its status text (Portuguese in v0.1.0).

> `lightmap_smooth.cpp` decodes every terrain chunk light map the world draws (256x256 DXT5), removes the RGB565
> colour noise, enlarges it 4x with a cubic B-spline that reads across neighbouring chunks, and uploads the result as a
> 1024x1024 A8R8G8B8 texture with a full mip chain. Mip 1 of every smoothed chunk is also copied into one **world light
> atlas** (2 texels per metre) that any surface knowing its world xz can sample. Status: **working**. Both flavours.
> Setting `mapaDeLuzSuavizado` ("Smooth light on the ground"). Part of [Night Lighting](README.md).

## Purpose

Notes "Mapa de luz suavizado": two visible defects come from the chunk light map itself.
- Blocky "low resolution" lamp circles: 1 texel per metre stretched with bilinear filtering, plus the 4x4 DXT blocks.
- Purple / green specks at night: DXT5 stores colour as RGB565 endpoints (green 6 bits, red and blue 5), so dim light is
  rounded to tinted values. Very visible on white snow.

And one structural need: many surfaces (lot grass beyond its home chunk, snowy floors, fences, doors, snow on objects)
only know their world position, not a chunk. They need the terrain light "anywhere, across chunk borders": the atlas.

## User-facing settings

| UI label | TOML key | Type | Default | Notes |
|---|---|---|---|---|
| Smooth light on the ground | `mapaDeLuzSuavizado` | bool | true | Adv / Ground and snow. Pushed every frame (`LightmapSmooth::SetEnabled`); turning it off calls `Clear()` (everything released). The UI greys out the ground-light object and fence options without it, because no atlas exists then |

## How it works

### Registration (render thread, per draw)

This branch of `OnDrawInner` sits after the `luzDoPosteNaGramaDoLote` gate, so with "Street lamps light inside lots"
off no chunk is registered and neither smoothed maps nor the atlas exist (this is the real reason the UI requires both
options for the atlas consumers).

`OnDrawInner` -> `RecordWorldChunk` (lot_light_bridge.cpp) finds the chunk light map of each world terrain draw
(see [lot-light-pass.md](lot-light-pass.md)) and calls `LightmapSmooth::Get(key, gameTexture)`:
- creates or updates the `Entry` for `key` = chunk centre (x, z) (256-unit steps; centre = 256*i + 128);
- stores the game texture AddRef'd; when the game's texture pointer changes, resets `hash = 0` (re-read);
- sets `lastUse = g_frame` (chunks in view are processed first);
- returns the smoothed texture or nullptr; the world draw then swaps it into the sampler for that draw only.

### Change detection and scheduling (`OnPresentBody`, every frame)

1. Sort entries by `lastUse` (most recent first).
2. Up to **4 new** maps per frame (`hash == 0`): `CheckEntry` -> `ReadSource` (`LockRect(0, READONLY)`; requires
   256x256, `D3DFMT_DXT5`, pool != DEFAULT) -> copies the 64 KB of level 0 -> 64-bit FNV-1a -> if the hash changed,
   keeps the raw DXT5 (`shared_ptr`) and marks the 3x3 neighbourhood dirty (`MarkDirtyAround`), because each map reads
   its neighbours' borders.
   - Not readable: `hash = 1` (never retried until the pointer changes), and if a smoothed map of an older source
     exists it is **dropped**, so the draw uses the game's current map instead of a stale smoothed one. Log:
     `[LightmapSmooth] Mapa (x, z) ilegivel: WxH formato F pool P niveis L (suavizado antigo descartado)`. Counter
     "unreadable" in the status.
3. When no new map was read this frame: one round-robin re-check (`g_checkCursor`) of an existing entry. This is how a
   game rebuild (dusk, lamp change) is detected: one chunk hash per frame.
4. `EnsureAtlas(dev)`.
5. Queue a `Job` for each dirty, not-in-flight entry that has raw data: the job holds the 3x3 raw neighbours and a
   generation number (`g_genCounter`).
6. Upload **one** finished result per frame (`Upload`).

### Processing (`Process`, detached worker thread `WorkerMain`)

| Step | Detail |
|---|---|
| Extended source | 268x268 (256 + 2x6 border, `kBorder` = 6). Border texels come from the neighbour chunk when it is registered, else clamped to the centre chunk. Each neighbour is decoded one at a time (`DecodeDxt5`) |
| Planes | luma `Y = 0.299 R + 0.587 G + 0.114 B`, alpha A (the game uses the light map alpha for sun visibility; kept) |
| Chroma cleanup | separable 7-tap Gaussian, sigma 1.5 (weights 0.0366, 0.1112, 0.2167, 0.2710, ...) over RGB and Y; colour ratio = blurred RGB / blurred Y, clamped 0..4, faded to neutral (ratio 1) when blurred Y < 0.03 ("very dim: the noise is all there is") |
| Enlarge 4x | output texel j at source coordinate (j + 0.5)/4 - 0.5; cubic B-spline (4 taps, no ringing) for Y and A; bilinear for the (already smooth) colour ratio |
| Encode | A8R8G8B8, R at bit 16; 4x4 Bayer dither (+-0.5 LSB) on RGB and A; value = Y x ratio x 255 clamped 0..255 |
| Mips | 2x2 box down to 1x1: 11 levels |

The worker waits while 2 results are pending (5.6 MB each), so memory stays bounded. Any exception (out of memory in
the 32-bit process) gives an empty result for that job; an exception in `OnPresent` sets `g_failed` (feature off for
the session, log `[LightmapSmooth] Sem memoria: mapa de luz suavizado desligado`).

### Upload

`Upload`: result generation must equal the entry's (older jobs and anything from before a `Clear` are dropped); creates
a SYSTEMMEM staging texture 1024x1024 with all levels, copies, creates a **D3DPOOL_DEFAULT** texture (video memory: no
32-bit address space used) and `UpdateTexture`. Then `AtlasUpload` of level 1 (512x512).

### The world atlas

| Property | Value |
|---|---|
| Texels per chunk | 512 (`kAtlasPerChunk` = kOut/2): 2 texels per metre |
| Size | (W x 512) x (H x 512), W/H = chunk span + 1 chunk margin each side; max 16 chunks per axis (8192 texels), else no atlas ("unusually large world") |
| Format | A8R8G8B8, 1 level, `D3DUSAGE_RENDERTARGET`, D3DPOOL_DEFAULT; cleared with `ColorFill` to ARGB(255,0,0,0) |
| Fill | per chunk, 512x512 SYSTEMMEM staging + `UpdateSurface` at (ix*512, iz*512) |
| Growth | when a chunk outside the current rectangle appears, the atlas is recreated larger (union of old and new, +1 margin) and every entry is marked dirty (all reprocessed) |
| Mapping | `LightmapSmooth::Atlas(c)`: uv = world.xz * c.xy + c.zw, c.x = 1/(W*256), c.y = 1/(H*256), c.z = -minX*256*c.x, c.w = -minZ*256*c.y. Returns nullptr until at least one chunk was copied |

Consumers bind it with `SamplerBind` (CLAMP, LINEAR min/mag, **mip NONE**: the atlas has one level) or, for the lot
passes, LINEAR mip on s2/s12.

| Consumer | Handler | Coordinate | Doc |
|---|---|---|---|
| Summer lot grass | lot branch of `OnDrawInner` | VS c14 rewritten to atlas mapping | [lot-light-pass.md](lot-light-pass.md) |
| Snowy lot grass | `DrawLotSnow` | VS c15 rewritten | [snow.md](snow.md) |
| Winter floors, pool edge | `DrawFloor` | TEXCOORD0.zw = world xz | [floors.md](floors.md) |
| Summer outdoor floors | `DrawFloorAtlas` | VS copy writes world xz to first free TEXCOORD >= 7 | [floors.md](floors.md) |
| Snow on floors, door sills | `DrawSnowFloor` | world xz / 2 (scale c.xy x 2) | [snow.md](snow.md) |
| Snow on fence tops | `DrawSnowCover` | TEXCOORD3.xy | [snow.md](snow.md) |
| Snow on stair tops | `DrawSnowRelief` | TEXCOORD4.zw = world xz / 2 (x 2) | [snow.md](snow.md) |
| Fences, railings, stairs | `DrawInstanced` | TEXCOORD1.zw | [fences.md](fences.md) |
| Outdoor rig objects | `DrawObjectLamp` | TEXCOORD8 (VS copy) | [objects-and-rigs.md](objects-and-rigs.md) |

Smoothed per-chunk maps (not the atlas) are used by: the world terrain draw (texture swap in its own sampler), roads
(`LightmapSmooth::Find`, both the road's own copy and the extra sampler), and the lot passes when the atlas is not
ready (`ChunkTexture`).

### Lifecycle

- New world: `NightTerrainRelight::OnPresent` detects a new cells pointer -> `LotLightBridge::OnWorldChanged` ->
  `ClearChunks` -> `LightmapSmooth::Clear` (jobs, results, entries, atlas released).
- Device reset: `LightmapSmooth::OnPreReset` (in `RenderCallbacks::preReset`) releases the atlas and all DEFAULT
  textures and marks every entry dirty (rebuilt afterwards).
- Bridge hooks unregistered (`UpdateHooks` off): `ClearChunks`.

### `floor_atlas_table.h`

The table of 261 ExteriorFloors pixel shaders (size + FNV-1a) that read the atlas through `PatchBakedAtlasPs`; see
[floors.md](floors.md) for the patch. It lives here conceptually because it is the largest atlas consumer by shader
count.

## Files and functions

| File | Function | Role |
|---|---|---|
| lightmap_smooth.cpp | `DecodeDxt5`, `ReadSource`, `Fnv` | read the game map |
| | `CheckEntry`, `MarkDirtyAround` | change detection |
| | `Process`, `BSplineWeights`, `WorkerMain`, `EnsureWorker` | smoothing on the worker |
| | `Upload`, `EnsureAtlas`, `AtlasUpload`, `ReleaseAtlas`, `ChunkIndex` | GPU side |
| | `LightmapSmooth::Get / Find / Atlas / OnPresent / OnPreReset / Clear / Status / SetEnabled / Enabled` | API |
| lot_light_bridge.cpp | `RecordWorldChunk`, `ChunkTexture`, `ClearChunks`, `SamplerBind` | registration and binding |
| patches/night_terrain_relight_patch.cpp | Present hook: `SetEnabled(g_smoothMaps)`, `OnPresent(device)`; `RenderCallbacks::Add(preReset, OnPreReset)` in `Install` | driver |

## Game addresses and patterns

None patched. Facts relied on:

| Fact | Evidence |
|---|---|
| Chunk light map = 256x256 DXT5, 4 mips when baked with the world, 1 when rebuilt; MANAGED pool | `RecordWorldChunk` comment; `ReadSource` requirements; captures |
| Chunk = 256 m, uv = (pos - centre)/256 + 0.5, centre in VS c8.w / c10.w | notes section 1 |
| The terrain map is `FUN_00C292B0` ("staticTerrainLightmap") output, chunk+0xD8 ("Terrain/LightmapTexture") | smooth_streaming_patch.cpp localized relight comment; [../../engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md) |

## Shader details

No shader is changed by this module. It only provides textures; see the consumers.

## Interactions

- Terrain relight: every game rebuild changes chunk maps; the round-robin check picks them up one chunk per frame, so
  after a full rebuild the smoothed maps catch up over several frames (world load: visible chunks first, since 25/09
  16:25).
- Removed HDR build: the notes observe the atlas and smoothed maps are 8-bit and clamp at 1.0
  ([../../removed-features.md](../../removed-features.md)).

## Known limitations

- The atlas is the light of the **ground**: no height, no occlusion. Railings of a balcony get the ground light under
  them; stair rails indoors near a street lamp can glow (notes, fences RCA "Limites conhecidos").
- Worlds larger than 16x16 chunks get no atlas: every atlas consumer then falls back to the game (or to the home chunk
  map for lot passes).
- The multi-pass summer terrain light pass (475E594D/756) never registers chunks, so its maps are not smoothed and it
  keeps the blocky DXT5 look next to smoothed patches of the same chunk (ground_report.md section 4).
- TerrainLow (distant terrain, DXT5 map in s3, census FF6760A8) is not smoothed.

## Pitfalls and failed approaches

- Review 25/09 (items 7, 8): the result queue had no limit (5.6 MB each) and 1 MB of floats per chunk was kept forever
  -> now at most 2 results waiting, only the 64 KB DXT5 kept per chunk (decoded on the worker), everything cleared on
  world change, generations discard results from before a `Clear`.
- A detached worker is intentional: a joinable `std::thread` would terminate the game at exit.
- Winter seam m73/m74 (hypothesis 1): a stale smoothed map survived when the game swapped in an unreadable new map ->
  unreadable maps now drop the old smoothed map.
- m76 (hypothesis 2 of the same seam): lots reading their home chunk with CLAMP -> lots read the atlas.
- Load order (25/09 16:25): the user waited more than 20 s for snow to look right -> visible chunks are processed first
  (`lastUse`).

## Testing in game

- Dev > Status > "Smoothed map: chunks smoothed: R of N | queued: Q | uploaded: U | unreadable: X | world map: WxH
  chunks (C copies)". After load, R should approach N and Q drop to 0; C counts atlas copies.
- Toggle "Smooth light on the ground" at night on snow: specks and 1 m blocks disappear when on.
- F7 on world grass: the terrain draw's light-map sampler shows a 1024x1024 A8R8G8B8 texture with 11 mips (smoothed);
  atlas consumers show the 2D render target (sizes seen: 2560x2560, 4096x4096).
- Log: the `ilegivel` line names chunks the game rebuilt in a non-lockable format.

## Open items

- PASSO3 increment 5: re-render the terrain stamp at 4 texels/m with the game's formula captured from
  `FUN_00C292B0` (`E = c0.rgb * sat(c0.w / d^2) * (N.L >= 0 ? sat(sqrt(N.L)) : 0)`, PASSO3-PLANO section 4), delivered
  through `LightmapSmooth::Find`, validated against the original at 256^2.
- Persistent staging texture (performance item 5 of the 25/09 review).
