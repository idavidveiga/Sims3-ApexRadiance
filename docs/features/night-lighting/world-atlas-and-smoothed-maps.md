# World light atlas and smoothed terrain light maps

> **Status in the standalone:** the smoothing itself (decode, colour cleanup, 4x B-spline, mips) is the v0.1.0 baseline
> (b84d5f1). The scheduling around it was rewritten on 28/09 ("correct first", section "Update path" below): stale
> smoothed maps are never shown, the atlas gets a plain copy of a changed chunk at once, the atlas keeps its contents when
> it grows, jobs are coalesced by key, uploads reuse persistent textures, and the game's own per-chunk re-render tells the
> module which chunk changed. **Not yet tested in game** (the user compiles and tests).
>
> **GPU path (28/09, default):** the same smoothing done by pixel shaders on the render thread, straight from the game's
> chunk maps, in the frame the game's map changes (no lock, decode, worker or upload); the CPU path below stays as the
> automatic fallback and as a developer A/B (`mapaDeLuzSuavizadoNaGpu`). See "GPU path". **Not compiled, not run**:
> the HLSL has not been through fxc yet and nothing of it was seen in game.

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
| Smooth the ground light maps on the GPU (A/B) | `mapaDeLuzSuavizadoNaGpu` | bool | true | **Dev build only** (registered under `if constexpr (!kPublicBuild)`; also a checkbox under the status line in Developer > Lighting). Pushed every frame (`SetGpuPreferred`), applied at the next Present: a switch calls `Clear()` and the new path rebuilds every map. The public build always prefers the GPU and falls back to the CPU by itself |

## How it works

### Registration (render thread, per draw)

This branch of `OnDrawInner` sits after the `luzDoPosteNaGramaDoLote` gate, so with "Street lamps light inside lots"
off no chunk is registered and neither smoothed maps nor the atlas exist (this is the real reason the UI requires both
options for the atlas consumers).

`OnDrawInner` -> `RecordWorldChunk` (lot_light_bridge.cpp) finds the chunk light map of each world terrain draw
(see [lot-light-pass.md](lot-light-pass.md)) and calls `LightmapSmooth::Get(key, gameTexture)`:
- creates or updates the `Entry` for `key` = chunk centre (x, z) (256-unit steps; centre = 256*i + 128);
- stores the game texture AddRef'd; when the game's texture pointer changes, resets `hash = 0` (map unknown until read);
- sets `lastUse = g_frame` (chunks in view are processed first);
- returns the smoothed texture **only while it was built from the game's current map** (`hash == doneHash`), else
  nullptr; the world draw then swaps it into the sampler for that draw only, or keeps the game's map. `Find` (roads, lot
  passes without the atlas) follows the same rule.
- GPU path: a new texture pointer bumps the entry's version (`gver`) instead, and a map whose version was not built yet
  is built right there, before this draw (see "GPU path"); "current" = `gBuiltVer == gver`.

### GPU path (default since 28/09)

Goal: the smoothed map of the game's current map on screen in the same frame the game's map changes, the same look as
the CPU path (within about 1/255 per channel), no CPU readback/decode, no upload, no worker thread. Code:
`lightmap_smooth.cpp` section "GPU path"; shaders: `shaders/lightmap_smooth_ps.hlsl` (the mod compiles the identical copy
in `shaders/lightmap_smooth_hlsl.h` at run time with `D3DCompile`, ps_3_0, O3: keep the two identical).

**Passes per chunk** (`BuildOne`; one `DrawPrimitiveUP` quad each, `D3DFVF_XYZRHW | D3DFVF_TEX1` with ps_3_0 as in Depth
Blur; every fetch a point sample at a texel centre with `tex2Dlod`, level 0; samplers POINT / POINT / mip NONE, CLAMP,
sRGB off, MAXMIPLEVEL 0). P grid = source texels -4..259 of the chunk (P = source + 4, 264x264): the widest reach of the
CPU math is 4 texels into the neighbours (blur 3 + B-spline 1), the CPU's `kBorder` = 6 only pads.

| # | Entry point | Input -> target | Math (identical to `Process`) |
|---|---|---|---|
| 1 | `GatherPS` | the game's DXT5 maps -> RAW (264x264 float) | 9 quads, one per neighbour region (x: [0,4) left, [4,260) chunk, [260,264) right; same in z), each with that map bound on s0. A neighbour that is not registered draws the **centre** map with texture coordinates outside [0, 1]: CLAMP = the CPU's "clamp to the centre chunk" (a missing diagonal clamps both axes, like the CPU). The GPU's own DXT5 decoder replaces `DecodeDxt5` |
| 2 | `HBlurPS` | RAW -> HB (264x264 float) | 7-tap Gaussian (0.0366, 0.1112, 0.2167, 0.2710, ...) of (R, G, B, Y), Y = 0.299 R + 0.587 G + 0.114 B |
| 3 | `VBlurPS` | HB -> CH (264x264 float) | vertical 7 taps; ratio = RGB / Y clamped 0..4 (1 when Y <= 1e-4), faded to 1 by saturate(Y / 0.03) |
| 4a | `HUpYAPS` | RAW -> HUYA (1024x264 float) | s = (x + 0.5)/4 - 0.5, f = floor(s): cubic B-spline of Y and A over P f+3..f+6 |
| 4b | `HUpChromaPS` | CH -> HUC (1024x264 float) | linear between P f+4 and f+5 (weight s - f) |
| 5 | `VUpPS` | HUYA + HUC -> chunk level 0 (1024x1024 A8R8G8B8) | vertical B-spline / linear (same weights), v = Y x ratio x 255 + d, d = (bayer + 0.5)/16 - 0.5 with the CPU's 4x4 table computed from the texel position (`4 b2(x&1, y&1) + b2(x>>1&1, y>>1&1)`, b2(a,b) = 2a + 3b - 4ab: fixed per texel, no per-frame noise), clamped 0..255, `floor(v + 0.5) / 255` written (so the target stores exactly the CPU's byte) |
| 6 | `DownPS` | chunk level 0 -> atlas cell (512x512 at the cell, viewport = whole atlas) | 2x2 box of the bytes, `(sum + 2) / 4` rounded down = the CPU's integer mip 1 |
| 7 | `DownPS` / `CopyPS` | level l-1 -> MIP[l] (scratch), MIP[l] -> chunk level l, l = 1..10 | the CPU's integer mip chain; the chunk's levels are written from separate scratch textures so no pass samples the texture it renders to (no D3D9 / DXVK feedback loop) |

The HB / VB split, the horizontal-then-vertical enlargement and the order of every sum follow the CPU code, so the float
results differ only by operation order / FMA (about 1e-7 relative).

**Formats and memory.** Chunk maps: 1024x1024 A8R8G8B8, 11 levels, `D3DUSAGE_RENDERTARGET`, `D3DPOOL_DEFAULT` (5.6 MB of
video memory each, as on the CPU path). Scratch (shared, one chunk at a time): RAW, HB, CH (264x264) and HUYA, HUC
(1024x264) in `A32B32G32R32F` (3 x 1.1 + 2 x 4.3 MB), else `A16B16G16R16F` (half), plus MIP[1..10] A8R8G8B8 (1.4 MB):
about 13.4 MB of video memory (7.4 MB with 16-bit floats). System memory: none (the developer compare allocates about
20 MB for a moment). Formats checked with `CheckDeviceFormat(D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE)`.
`D3DUSAGE_AUTOGENMIPMAP` is not used: its filter is up to the driver (DXVK blits with linear filtering), not the CPU's
integer `(sum + 2) / 4`; the explicit passes are exact and cost little.

**When a chunk is built** (`GpuService`, from the draw hooks, inside the game's scene):
- A change is seen by (a) the re-render notice (`DrainNotices`, on the next draw; a lockable map whose hash did not change
  is not rebuilt: "same map"), (b) a new texture pointer in `Get` (also a brand-new chunk), (c) the fallback hash checks
  at Present (`GpuHashCheck`: round robin 1 per frame, 4 chunks in view per frame while boosted; only lockable managed
  DXT5 maps, `gNoHash` for the others). Each bumps the entry's version `gver`.
- `Get` (world terrain draw of that chunk): if its version was not built yet, it is built right there, before the draw.
  `Find` (roads, lot passes) and `Atlas(forDraw)` (every atlas consumer) run the same service first. The first service
  of a frame, and any later one after a new change, builds **every chunk in view with an own change** (cap 32),
  refills atlas cells from existing smoothed maps (cap 64, after atlas growth or a failed copy), and spends up to
  **1 ms of GPU** (estimate: the measured per-chunk time, 0.5 ms until measured) on chunks out of view with an own change
  and on neighbour-border updates. Held while a rebuild is imminent (`ExpectRebuild`, except chunks in view: there is no
  plain copy on this path); borders wait for the end of a rebuild sweep and 30 quiet frames, as on the CPU path.
- `Current` = `gtex` built from the current version (`gBuiltVer == gver`); neighbour borders may lag (same rule as the
  CPU path). A failed or interrupted build sets `gBuiltVer = 0`: the game's map is shown, retried next frame.
- Present (`OnPresentGpu`): timestamp read back, fallback hash checks, sweep end, `EnsureAtlas` (growth copy unchanged).
  No drawing at Present.

**AtlasRawCopy is dropped on this path**: every consumer of a cell (`Atlas`) and of a chunk map (`Get`, `Find`) runs the
service before it reads, so a changed chunk in view is rebuilt before anything reads it. The exceptions show the game's
map or a slightly late cell, never black: a change seen only by the Present hash check (one frame late, as on the CPU
path), a chunk out of view beyond the 1 ms budget (its atlas cell keeps its older smoothed map for a few frames; nothing
in view reads it in practice), more than 32 chunks in view changing at once (world load).

**Device state.** `PassState` (RAII) saves and restores through the device (so the other modules' hook trackers see the
game's state again): render targets 0..3 (1..3 unbound during the passes, sizes differ), the depth-stencil
(`ExtraHooks::RawGet/RawSetDepthStencilSurface`, unbound: the atlas is larger than the screen), pixel and vertex shader,
declaration / FVF, stream 0 (DrawPrimitiveUP clears it) and the stream frequencies of streams 0 and 1 (instancing),
textures and 8 sampler states of s0 and s1, 14 render states (Z, Z write, blend, separate alpha, alpha test, stencil,
cull, scissor test, fog, sRGB write, clip planes, colour write, fill mode, WRAP0), PS constants c0..c1, viewport and
scissor rect (SetRenderTarget resets both). One save / restore and one timestamp pair per batch. `g_gpuBusy` stops
re-entry. Get / Find / Atlas are called by the handlers before they change any state (checked in lot_light_bridge.cpp).
An exception inside (out of memory) turns the feature off (`g_failed`, released at the next Present) instead of
reaching the bridge's handler.

**Reset.** `OnPreReset` releases the chunk render targets, the scratch, the atlas and the queries (shaders stay: they
are not pool resources); every chunk is rebuilt by the first draws after the reset (in view at once, the rest within
the budget).

**Fallback.** `InitGpu` (first Present with the feature on, once per session): pixel shader 3.0, A8R8G8B8 render-target
textures, a float render-target format, the 8 shaders compile. Any failure: log `[LightmapSmooth] GPU smoothing
unavailable (why): using the CPU path`, status `CPU (GPU unavailable: why)`. Scratch creation failing later: `GPU
smoothing failed (why): switching to the CPU path`. Success: `GPU smoothing ready (intermediates F, PS 3.0 N
instruction slots)` and `Smoothing on the GPU`. A path switch (`ResolveMode`) calls `Clear()`.

**Developer tools.** Developer > Lighting, under the "Smoothed light map" status: the A/B checkbox and "Compare GPU vs
CPU (one chunk)" (`RequestCompare`): at the next ground draw, the most recently drawn chunk whose map and registered
neighbours can be locked is read (`ReadMap`), rebuilt on the GPU, read back (`GetRenderTargetData`, stalls once), smoothed
by `Process` on the render thread (about 30 ms once) and compared level by level. Log / status: `GPU vs CPU, chunk (x, z):
level 0 max diff R r G g B b A a (texels off by 1: n, by more: m of 1048576), levels 1-10 max diff d`.

**Exactness vs the CPU path (expected, not measured).** Everything after the DXT5 decode is the CPU's float math in
float32 with an 8-bit result rounded the same way, and the mips are the same integer box, so the only sources of
difference are (1) the GPU's DXT5 decoder (the hardware may expand RGB565 / interpolate the palette with its own 8-bit
rounding: under 1/255 per decoded texel, D3D allows it), (2) float operation order and division precision (about 1e-7),
(3) with the 16-bit float fallback, 11-bit mantissas in the intermediates (about 1/8 LSB). A difference before rounding
under 1 LSB gives at most 1 LSB after; dim texels amplify a decode difference through the colour ratio (at Y ~0.03 about
1 LSB). Expected: max 1 per channel, most texels 0; the mips inherit the level-0 difference (at most 1). Verify with the
compare button.

**Expected GPU cost per chunk (estimate, not measured).** About 9 M point fetches (8.9 M of 128-bit texels, most in
`VUpPS`: 1 M pixels x 6) and 17 MB of render-target writes: about 0.2-0.5 ms on a mid-range desktop GPU, maybe 1 ms or
more on integrated graphics; plus about 40 draws and render-target switches per chunk on the CPU (DXVK splits render
passes). The status shows the measured value (timestamp queries per batch, EMA per chunk) and the budget uses it.

**Compile / runtime assumptions (unverified).** fxc / D3DCompile accept the HLSL for ps_3_0 within the instruction limits
(the largest, `VUpPS`, is about 6 texld + 60 ALU); DXVK 3.1.1 supports `A32B32G32R32F` (else `A16B16G16R16F`) render-target
textures with point sampling, and mipmapped A8R8G8B8 render-target textures with each level as a render target; sampling
the game's DXT5 maps (managed, or render targets if the game ever uses those) with point filtering; `D3DFVF_XYZRHW` with
ps_3_0 (Depth Blur relies on it) and absolute pre-transformed coordinates with the viewport at the whole target (atlas
cells); drawing and `GetRenderTargetData` from inside the game's draw hooks (the game itself renders its chunk textures
mid-frame); timestamp queries issued several times per frame; NPOT render targets (264, 1024x264) with CLAMP and one
level. `StretchRect` is only used by the atlas growth (unchanged) and `UpdateSurface` not at all on this path.

### Update path: correct first (28/09) (CPU path)

The v0.1.0 path kept showing the smoothed map of the OLD map after the game re-rendered a chunk (dusk, lamp edit), until
the round-robin hash (one chunk per frame) found it, a single worker job ran (25-40 ms, estimated), and one upload per
frame recreated a 5.6 MB staging texture and a video-memory texture. A 3x3 invalidation re-smoothed each chunk up to 9
times during a full rebuild. The atlas was recreated and cleared black whenever the world grew. Now:

| Step | What happens | Where |
|---|---|---|
| Detect | (a) the game's per-chunk texture re-render reports the chunk (call site `0x00C8504C`, see below): read it the same frame; (b) new texture pointers, up to 4 per frame; (c) for 60 frames after a kick and `kSweepFrames` = 300 after a consumed rebuild: 4 chunks **in view** per frame, round robin; (d) one round-robin chunk per frame always. The hash is 64-bit words over the locked level 0 (`HashRows`); the 64 KB is copied only when it changed | `OnPresentBody` 1-4, `CheckEntry`, `ReadMap` |
| Show correct | `Get`/`Find` return nullptr until the smoothed map of the current map exists: the draw uses the game's map (correct, blockier). The atlas cell gets a **plain copy** of the current map (`AtlasRawCopy`: integer DXT5 decode, bilinear 2x at the smoothed mip 1's sample positions), chunks in view first, within 2 ms per frame, through a ring of 3 staging textures locked with `D3DLOCK_DONOTWAIT` | `OnPresentBody` 5 |
| Queue | Only the key is queued (the dirty flag coalesces repeated changes); at most `kMaxInFlight` = 2 jobs queued + processing + waiting upload (32-bit memory). Chunks in view first; a chunk's own change before a neighbour's border change. Held while a rebuild is imminent (`ExpectRebuild`); during a rebuild sweep only chunks already re-rendered (`awaiting` false); neighbour border re-smooths only after the sweep and after 30 frames with no change. Skipped when its inputs (its map and the 8 neighbours' map hashes, `SigOf`) equal those of its smoothed map (`doneSig`) | `OnPresentBody` 6 |
| Process | The worker reads the **latest** maps of the chunk and its neighbours (`g_shared`, under `g_mx`) when it starts the job, not when it was queued; times each job | `WorkerMain` |
| Upload | Dropped if the game changed the map again meanwhile (`centreHash != hash`, "outdated"). Otherwise copied into one persistent SYSTEMMEM staging (1024², 11 levels, locked with `D3DLOCK_DONOTWAIT`: when the GPU still copies the previous upload, the result waits one frame, "later") and `UpdateTexture` **into the chunk's existing texture** (D3D9 orders it after the draws already recorded); a new chunk takes a texture from a pool of up to 4 (textures dropped by unreadable maps) or creates one. Then mip 1 into the atlas | `Upload` |

Light level between the plain copy and the smoothed map: the smoothed map keeps the brightness (luma) of the game map
and only blurs the colour ratio; the cubic B-spline lowers a smooth peak by about h^2/(6 sigma^2) (h = 1 texel, lamp
pools about 10-25 m wide: under 1 %); very dim texels (blurred luma < 0.03) lose their RGB565 tint (neutral grey at the
same luma); dithering is +-0.5 LSB. So the switch plain -> smoothed changes the look (blocks and specks go away) but not
the light level. The 8-bit decode of the plain copy (5/6-bit expanded by bit replication) differs from the float decode
of the smoothing by under 1 LSB.

Not readable (not a managed 256x256 DXT5): `hash = 1` (never retried until the pointer changes); the smoothed map of the
older source is not shown (`hash != doneHash`) and its texture goes to the pool. Log: `[LightmapSmooth] Map (x, z)
unreadable: WxH format F pool P levels L (old smoothed map dropped)`. Its atlas cell keeps the last map read (unchanged
from v0.1.0).

### Change detection and scheduling (`OnPresentBody`, every frame)

1. Take the chunk re-render notices, sort entries by `lastUse` (most recent first).
2. Notices: `CheckEntry` each (and it is no longer `awaiting`).
3. Up to **4 new** textures per frame (`hash == 0`).
4. While boosted: 4 chunks in view per frame; then one round-robin chunk (`g_checkCursor`) when no new texture was read.
5. `EndSweepIfDone` (every chunk re-rendered or 300 frames), `EnsureAtlas(dev)`, plain atlas copies.
6. Queue jobs (rules above), upload one finished result.

A changed map marks its own entry `dirty` and the 8 neighbours `borderDirty` (each smoothed map reads 6 texels of its
neighbours).

### Rebuild sweep (driven by the terrain relight)

`NightTerrainRelight` calls `NoteKick(reason)` when it arms a rebuild (timing, 60 boosted frames), `ExpectRebuild(30)`
every frame while a rebuild is coming (load waiting for the world, dusk delay, lamp change debounce, kick armed and not
consumed: no new jobs, the game's maps are shown) and `OnTerrainRebuilt()` in the frame the game consumes it: every chunk
becomes `awaiting` (except those already re-rendered in that frame), 300 boosted frames. A consumed rebuild re-renders one
chunk per frame, so smoothing a chunk before its own re-render would be wasted: jobs wait per chunk until it changed (or
was reported re-rendered). The sweep ends when no chunk is awaiting or after 300 frames; the developer log then prints
`[LightmapSmooth] Rebuild sweep done: kick -> rebuild X ms, rebuild -> first chunk Y ms, -> last chunk Z ms (N chunks
changed, M not re-rendered)`.

### Chunk re-render notice (call site `0x00C8504C`)

The terrain update's per-chunk loop re-renders one chunk's composited textures per call when chunk+0x54 is set:
`0x00C85041 cmp byte [esi+54h],0; jz; 0x00C85047 push 0; push esi; mov ecx,edi; call 0x00C7E7A0`. `FUN_00C7E7A0` is
`void __thiscall(terrain, chunk, char force)`, `RET 8` (`re/out/dump/asm/00c7e7a0.asm`; decompile `fn_00c7e7a0.c`), and
its full render path ends with `mov byte [esi+54h],0` at `0x00C7E978`, the only write of +0x54 in it. `NightTerrainRelight`
checks the 15 bytes at `0x00C85047` (`6A 00 56 8B CF E8 4F 97 FF FF C6 44 24 0C 01`) and redirects only this CALL to
`ChunkRenderThunk` (`__fastcall` with two stack arguments = the same stack contract); after the original returns, if
+0x54 is 0, it reports `(chunk+0x0C >> 8, chunk+0x10 >> 8)` to `LightmapSmooth::NoteChunkRendered`. The other two callers
(`0x00C8088E`, `0x00C8307E`) are left alone. If the bytes differ, a warning is logged and detection falls back to hashing.
Counter: Developer > "Chunk re-render notices".

### Processing (`Process`, detached worker thread `WorkerMain`)

| Step | Detail |
|---|---|
| Extended source | 268x268 (256 + 2x6 border, `kBorder` = 6). Border texels come from the neighbour chunk when it is registered, else clamped to the centre chunk. Each neighbour is decoded one at a time (`DecodeDxt5`) |
| Planes | luma `Y = 0.299 R + 0.587 G + 0.114 B`, alpha A (the game uses the light map alpha for sun visibility; kept) |
| Chroma cleanup | separable 7-tap Gaussian, sigma 1.5 (weights 0.0366, 0.1112, 0.2167, 0.2710, ...) over RGB and Y; colour ratio = blurred RGB / blurred Y, clamped 0..4, faded to neutral (ratio 1) when blurred Y < 0.03 ("very dim: the noise is all there is") |
| Enlarge 4x | output texel j at source coordinate (j + 0.5)/4 - 0.5; cubic B-spline (4 taps, no ringing) for Y and A; bilinear for the (already smooth) colour ratio |
| Encode | A8R8G8B8, R at bit 16; 4x4 Bayer dither (+-0.5 LSB) on RGB and A; value = Y x ratio x 255 clamped 0..255 |
| Mips | 2x2 box down to 1x1: 11 levels |

The worker waits while 2 results are pending (5.6 MB each), and the render thread keeps at most 2 jobs in flight in
total, so memory stays bounded. Any exception (out of memory in the 32-bit process) gives an empty result for that job;
an exception in `OnPresent` sets `g_failed` (feature off for the session, log `[LightmapSmooth] Out of memory: smoothed
light map off`).

### Upload

See "Update path" above: generation must equal the entry's (older jobs and anything from before a `Clear` are
dropped), outdated results are dropped, one persistent SYSTEMMEM staging (5.6 MB of address space for the session,
released on `Clear`), `UpdateTexture` into the chunk's own **D3DPOOL_DEFAULT** texture (video memory). Then `AtlasUpload`
of level 1 (512x512).

### The world atlas

| Property | Value |
|---|---|
| Texels per chunk | 512 (`kAtlasPerChunk` = kOut/2): 2 texels per metre |
| Size | (W x 512) x (H x 512), W/H = chunk span + 1 chunk margin each side; max 16 chunks per axis (8192 texels), else no atlas ("unusually large world") |
| Format | A8R8G8B8, 1 level, `D3DUSAGE_RENDERTARGET`, D3DPOOL_DEFAULT; a new atlas is cleared with `ColorFill` to ARGB(255,0,0,0) (cells of chunks never seen) |
| Fill | CPU path: per chunk, 512x512 SYSTEMMEM staging (ring of 3) + `UpdateSurface` at (ix*512, iz*512): first a plain 2x copy of the game's current map, then the smoothed mip 1 (`atlasHash` / `atlasSmooth` per entry say what the cell holds). GPU path: `DownPS` from the chunk's level 0 drawn straight into the cell (`WriteCell`, = mip 1; `gAtlasSig` says which build the cell holds), no staging |
| Growth | when a chunk outside the current rectangle appears, the atlas is recreated larger (union of old and new, +1 margin); the old contents are copied on the GPU (`StretchRect` RT -> RT, same size, `D3DTEXF_NONE`) at their new offset, so no valid cell goes black; only the new chunks get plain copies. If the copy fails, every cell is refilled with plain copies and the smoothed ones are smoothed again (log `World map grown to WxH chunks (...)`) |
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
  `ClearChunks` -> `LightmapSmooth::Clear` (jobs, results, shared maps, entries, atlas, pool and staging released).
- Device reset: `LightmapSmooth::OnPreReset` (in `RenderCallbacks::preReset`) releases the atlas, the pool and all DEFAULT
  textures and marks every entry dirty (the atlas is refilled with plain copies at once, then smoothed). The SYSTEMMEM
  staging textures survive a reset. GPU path: also the chunk render targets, the scratch render targets and the queries;
  every chunk is rebuilt by the first draws after the reset (shaders survive).
- Path switch (developer A/B or GPU failure, `ResolveMode` at Present): `Clear()`; the draws register the chunks again.
- Bridge hooks unregistered (`UpdateHooks` off) or bridge switched off: `ClearChunks`.
- Night Lights reinstalled after a developer option change (`ReinstallNow`, render thread): `LotLightBridge::Shutdown(true)`
  keeps the chunk maps, smoothed maps and atlas (same world, no reset possible in between); a real uninstall releases
  them.

### `floor_atlas_table.h`

The table of 261 ExteriorFloors pixel shaders (size + FNV-1a) that read the atlas through `PatchBakedAtlasPs`; see
[floors.md](floors.md) for the patch. It lives here conceptually because it is the largest atlas consumer by shader
count.

## Files and functions

| File | Function | Role |
|---|---|---|
| lightmap_smooth.cpp | `DecodeDxt5`, `ReadMap`, `HashRows`, `SigOf` | read the game map |
| | `CheckEntry`, `MarkBordersAround`, `MarkUnreadable`, `NoteChange`, `EndSweepIfDone` | change detection, sweep |
| | `Process`, `BSplineWeights`, `WorkerMain`, `EnsureWorker` | smoothing on the worker |
| | `Upload`, `EnsureAtlas`, `AtlasUpload`, `AtlasRawCopy`, `DecodeDxt5Argb`, `Blend4`, `LockAtlasStage`, `ReleaseAtlas`, `ChunkIndex`, `PoolPut` | GPU side |
| | `InitGpu`, `RtFormatOk`, `EnsureGpuRes`, `ReleaseGpuRes`, `ReleaseGpuChunks`, `ResolveMode`, `GpuRuntimeFailure` | GPU path: setup, fallback |
| | `GpuService`, `RunBatch`, `BuildOne`, `WriteCell`, `PassState`, `DrawRect`, `GpuSafe` | GPU path: passes (draw hooks) |
| | `DrainNotices`, `BumpVersion`, `GpuHashCheck`, `HashMap`, `GpuSig`, `OnPresentGpu` | GPU path: change detection, Present |
| | `BeginTiming`, `EndTiming`, `ReadTimings`, `RunCompare` | GPU path: timing, developer compare |
| | `LightmapSmooth::Get / Find / Atlas / OnPresent / OnPreReset / Clear / Status / SetEnabled / Enabled` | API |
| | `LightmapSmooth::SetGpuPreferred / GpuActive / RequestCompare / CompareStatus` | GPU path API (developer A/B, compare) |
| | `LightmapSmooth::NoteKick / OnTerrainRebuilt / ExpectRebuild / NoteChunkRendered` | driven by the terrain relight |
| shaders/lightmap_smooth_ps.hlsl, lightmap_smooth_hlsl.h | `GatherPS`, `HBlurPS`, `VBlurPS`, `HUpYAPS`, `HUpChromaPS`, `VUpPS`, `DownPS`, `CopyPS` | GPU path shaders (the .h is the run-time copy) |
| lot_light_bridge.cpp | `RecordWorldChunk`, `ChunkTexture`, `ClearChunks`, `SamplerBind`, `ChunkCount` | registration and binding (the status line calls `Atlas(c, false)`: no GPU work) |
| patches/night_terrain_relight_patch.cpp | Present hook: `SetEnabled(g_smoothMaps)`, `SetGpuPreferred(g_smoothMapsGpu)`, `OnPresent(device)`; `RenderCallbacks::Add(preReset, OnPreReset)` in `Install`; `ChunkRenderThunk` at `0x00C8504C`; dev setting `mapaDeLuzSuavizadoNaGpu`, A/B checkbox and compare button in `RenderDeveloperUI` | driver |

## Game addresses and patterns

One call redirected (by `NightTerrainRelight`, optional): `0x00C8504C` (see "Chunk re-render notice"). Facts relied on:

| Fact | Evidence |
|---|---|
| Chunk light map = 256x256 DXT5, 4 mips when baked with the world, 1 when rebuilt; MANAGED pool | `RecordWorldChunk` comment; `ReadSource` requirements; captures |
| Chunk = 256 m, uv = (pos - centre)/256 + 0.5, centre in VS c8.w / c10.w | notes section 1 |
| The terrain map is `FUN_00C292B0` ("staticTerrainLightmap") output, chunk+0xD8 ("Terrain/LightmapTexture") | smooth_streaming_patch.cpp localized relight comment; [../../engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md) |

## Shader details

No game shader is changed by this module. It only provides textures; see the consumers. Its own pixel shaders (GPU path)
are in `shaders/lightmap_smooth_ps.hlsl`, described in "GPU path". Validation with fxc (no macros, one entry point at a
time): `fxc /T ps_3_0 /E GatherPS /O3 lightmap_smooth_ps.hlsl`, and the same for `HBlurPS`, `VBlurPS`, `HUpYAPS`,
`HUpChromaPS`, `VUpPS`, `DownPS`, `CopyPS`. Constants: c0 (s0 size: 1/w, 1/h, w, h), c1 (s1 size, `VUpPS` only).

## Interactions

- Terrain relight: every game rebuild changes chunk maps, one chunk per frame. Each re-rendered chunk is reported by the
  call-site notice (or found within a few frames by the boosted hash checks), shows the game's map and a plain atlas copy
  at once, and is smoothed right after (visible chunks first). Latency per chunk in view: detection 0-1 frames, plain
  copy the same frame, smoothed map about one job later (25-40 ms estimated, plus up to 2 jobs ahead of it) + 1 frame.
  GPU path: a chunk reported by the notice is smoothed before its next draw, i.e. in the frame the game re-rendered it
  (1 chunk per frame during a rebuild: one build per frame, about 0.2-0.5 ms estimated); found by the Present hash check
  instead: the next frame. Neighbour borders follow after the sweep within the 1 ms budget.
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
- 28/09 (before "correct first"): after a rebuild the OLD smoothed map stayed on screen until the round robin reached
  the chunk (1 chunk per frame over every registered chunk), and atlas growth cleared the atlas black and re-smoothed
  every chunk: black ground light on lots, floors and fences while panning. Do not bring back "keep the old smoothed map
  until the new one is ready" or a black clear on growth.
- Do not re-smooth on streaming churn: the post-0.1.0 per-second relight reconciliation caused 12-60 ms hitches
  ([../../changes-since-0.1.0.md](../../changes-since-0.1.0.md) 2.1). This module only reacts to maps the game really
  re-rendered, and a neighbour's border change waits until chunks stop changing.

## Testing in game

GPU path (default):
- Log at the first world: `[LightmapSmooth] GPU smoothing ready (intermediates A32B32G32R32F, ...)` and `Smoothing on
  the GPU`; otherwise `GPU smoothing unavailable (why): using the CPU path`.
- Dev > Status > "Smoothed light map: GPU (F intermediates) | chunks smoothed: R of N | waiting: W (neighbour borders B)
  | built: X (in view, out of view, borders), failed Y | atlas cells written | GPU time: T ms per chunk, last batch, max
  batch (n timed) | changes seen (render notices, same map), hash checks | world map ... | GPU vs CPU: ...". "failed" must
  stay 0; W returns to 0 right after each change in view.
- Press "Compare GPU vs CPU (one chunk)" looking at lit ground: expected level 0 max diff 0 or 1 per channel, "by more: 0";
  levels 1-10 max diff at most 1. A larger difference means a math mismatch (report the line).
- Dusk / lamp edit at night looking at a lamp: the ground switches to the new light **smooth** at once (no blocky frame),
  lots / floors / fences next to it in the same frame. Untick the A/B checkbox: the CPU behaviour (blocky for a moment).
- Frame Profiler / hitches during a dusk rebuild: GPU per chunk time in the status; no hitch from smoothing.
- Alt+Tab / resolution change (device reset): the ground light comes back smooth at once (no black cells on lots).

CPU path (A/B off, or fallback):
- Dev > Status > "Smoothed light map: CPU | chunks smoothed: R of N | waiting: W (in flight F, queue Q) | jobs: J, avg A ms,
  max M ms | skipped (inputs unchanged): S | uploaded: U (later L, outdated O) | plain ground copies: P | changes seen: C
  (render notices K) | unreadable: X | world map: WxH chunks (copies, grown G x) | [holding: rebuild coming] | [rebuild
  sweep: N chunks not re-rendered yet] | last kick: reason (s ago), last change T ms after it | last sweep: kick -> rebuild,
  -> first chunk, -> last chunk". R counts only smoothed maps of the CURRENT game map. After a rebuild, W returns to 0.
- Developer log (dev build): `Rebuild sweep done: ...` after each rebuild; with verbose logging, one `Chunk (x, z) changed
  (render notice | new texture | fast check | round robin), T ms after the last kick` per chunk in view.
- Dusk / Build-mode lamp edit at night while looking at a lamp: the ground under it switches to the new light at once
  (blocky for a moment, then smooth); lots, floors and fences next to it follow in the same frame (atlas plain copy).
  There must be no frame where the old light (or black) shows after the game's map changed.
- Pan quickly across the world: no black ground light on lots / floors / fences at newly loaded chunks or elsewhere when
  the world map grows ("grown G x" increments, log `World map grown ... (old contents copied)`).
- Toggle "Smooth light on the ground" at night on snow: specks and 1 m blocks disappear when on.
- F7 on world grass: the terrain draw's light-map sampler shows a 1024x1024 A8R8G8B8 texture with 11 mips (smoothed);
  atlas consumers show the 2D render target (sizes seen: 2560x2560, 4096x4096).
- Log: the `ilegivel` line names chunks the game rebuilt in a non-lockable format.

## Open items

- PASSO3 increment 5: re-render the terrain stamp at 4 texels/m with the game's formula captured from
  `FUN_00C292B0` (`E = c0.rgb * sat(c0.w / d^2) * (N.L >= 0 ? sat(sqrt(N.L)) : 0)`, PASSO3-PLANO section 4), delivered
  through `LightmapSmooth::Find`, validated against the original at 256^2.
- Persistent staging texture (performance item 5 of the 25/09 review): done 28/09 (one 1024² for uploads, a ring of 3
  512² for the atlas).
- Unverified at run time (28/09): that `D3DLOCK_DONOTWAIT` on a SYSTEMMEM texture's `LockRect` returns
  `D3DERR_WASSTILLDRAWING` under DXVK 3.1.1 when busy (else it simply waits, which is correct but may stall; any other
  failure falls back to a plain lock); that `StretchRect` between the two atlas render-target textures succeeds under
  DXVK (fallback: plain copies, logged). Check "later" and "grown ... old contents copied" in the status / log.
- GPU path (28/09), not compiled and not run: fxc validation of the 8 entry points; in game, the log lines, the compare
  result, the per-chunk GPU time, that the other modules' trackers are unaffected by the passes drawn inside their draw
  hooks (Frame Capture / census around a rebuild), device reset, and every assumption listed at the end of "GPU path".
  Whether the game's rebuilt maps are managed DXT5 (as `ReadMap` requires) or render targets (the engine doc says "a
  256x256 render target with 1 mip") is still open: the GPU path samples either; only its fallback hash check and the
  compare need a lockable map.
- Pre-sizing the atlas to the whole terrain at world load (terrain chunk vector `terrain+0xB0/+0xB4`, verified in
  full.asm) was not done: it needs the WorldManager global and the chunk coordinates at run time; the GPU copy on growth
  already avoids black cells.
