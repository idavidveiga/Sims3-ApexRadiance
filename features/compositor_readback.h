#pragma once
// Compositor tile readback without the GPU wait (Apex Radiance; a part of "Faster Sim building", feature "FastCasSort";
// docs/features/performance/README.md).
//
// The game builds Sim, object and thumbnail textures with the texture compositor ("CAS/TextureBuilder"). Its queue loop
// FUN_00608270 runs the head builder's state machine FUN_005fdef0 until a step returns 2 or 3 (stop for this frame) or the
// time slice ends. Per tile, state 2 (FUN_005fdde0) renders the tile and then FUN_005fdbf0 copies it to the builder's
// system-memory staging texture [b+0x20] (GetRenderTargetData in FUN_005fbce0) and locks it right away (FUN_00618df0
// at 0x005FDCB2). That lock waits until the GPU has finished the copy: under DXVK a flush and a full wait each tile, in
// the "CAS TextureCompositor" hitches (sampling 30/09: 56% of their excess time waiting in ntdll).
//
// This part splits the step over frames, with the game's own functions only:
//   - 0x005FDC8A (the readback call in FUN_005fdbf0), reached from the queue loop's state 2: the game's readback runs,
//     then a D3D9 EVENT query is issued and 2 returned, which FUN_005fdbf0 / FUN_005fdde0 hand back unchanged. The
//     builder moves to state 3 and the queue stops for this frame (the game's own meaning of 2).
//   - 0x006082F8 (the queue loop's call of the state machine): while that builder's query has not signalled, 2 is
//     returned without running it (up to kMaxWaitFrames frames, then the game's path, which may wait as before).
//   - 0x005FDFFF (state 3's call of FUN_005fd420): FUN_005fdbf0 runs again with the readback skipped, so its LockRects
//     find the copy done, then FUN_005fd420 as before. A 3 or 4 from FUN_005fdbf0 is returned as the game would have.
// The bytes read are those the game reads; each tile only finishes one or more frames later. Calls from elsewhere (the
// synchronous loop at 0x005FE0FB, state 1) are never deferred, and a deferred tile is always completed in state 3.
#include <cstdint>
#include <string>

namespace CompositorReadback {

bool Start(std::string* error);
void Stop();
bool Running();

struct Stats {
    uint64_t deferred = 0;   // tiles whose lock moved to a later frame
    uint64_t resumed = 0;    // deferred tiles completed in state 3
    uint64_t waitFrames = 0; // queue steps skipped while a query was pending
    uint64_t timedOut = 0;   // resumed before the query signalled (kMaxWaitFrames)
    uint64_t dropped = 0;    // pending tiles abandoned because the builder was reset
    uint64_t gamePath = 0;   // state-2 readbacks left to the game (not from the queue, query unavailable)
    double resumeMs = 0.0, resumeMaxMs = 0.0; // the deferred lock (FUN_005fdbf0 again) in state 3
    double state2Ms = 0.0;   // state-2 calls that were deferred (render + readback, no lock)
};
Stats GetStats();
std::string StatusText();
void RenderDeveloperUI();

} // namespace CompositorReadback
