#pragma once
// Faster texture compression (Apex Radiance, feature "FastTextureCompression"; docs/features/performance.md).
//
// The game compresses textures to DXT1 / DXT5 on the CPU (0x006152F0 / 0x006154B0 on Steam 1.67.2: terrain normal maps,
// lot LOD views, CAS / Sim textures, thumbnails, scene captures), block by block with a principal-axis fit, ordered
// dithering and a 3/4-colour choice: 20-40% of the measured 50 ms+ hitches were dominated by it. This feature replaces
// both functions with features/dxt_codec.h's four-blocks-at-a-time version, which produces the same bytes for every
// block (bit-identical: same float operations and rounding, same approximate instructions per block); a block whose
// intermediate values are not finite is encoded by the game's own function.
//
// Hooks: the functions' entries through framework/entry_chain.h (a JMP written with every other thread suspended; the
// Frame Profiler's DXT counter is the outer layer and keeps timing every call).
// Checks: the first 16 images of each session (both builds) and, in the development build, 1 image in N (default 8) are
// also encoded by the game into a scratch buffer and compared byte for byte; a difference is logged with the block's
// pixels, the game's bytes are used, and the feature turns itself off for the session.
#include <cstdint>
#include <string>

namespace FastDxt {

// Hooks the two encoders (feature on) / removes the hooks (feature off; false when an entry could not be put back: the
// hook then stays and passes every call through). Any thread.
bool Start(std::string* error);
bool Stop();
bool Running();

// Development build: 1 image in `n` is checked against the game (0 = never; the first 16 of the session always are).
void SetVerifyEvery(int n);
int VerifyEvery();
void VerifyAllFor(double seconds);

struct Stats {
    uint64_t images = 0, pixels = 0, blocks = 0;
    uint64_t delegated = 0;   // blocks the game's function encoded (non-finite intermediate values)
    uint64_t powerAxis = 0;   // flat-luma blocks (power-iteration axis)
    uint64_t solid = 0;
    uint64_t passedThrough = 0; // calls while off / turned off
    uint64_t checked = 0, mismatches = 0, notCheckable = 0;
    double fastMs = 0.0;          // all fast calls (checked ones included)
    double checkedGameMs = 0.0, checkedFastMs = 0.0; // the game's and Apex's time on the checked images
    bool selfDisabled = false;
    std::string lastMismatch;
};
Stats GetStats();
std::string StatusText();
// Development build: counters and the check controls
void RenderDeveloperUI();

} // namespace FastDxt
