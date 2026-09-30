#pragma once
// Faster Sim building (Apex Radiance, feature "FastCasSort"; docs/features/performance.md, "Faster Sim Building").
//
// When the game builds a Sim's meshes (Create a Sim, and in play when a Sim changes outfits), FUN_005d3760
// ("CAS/ModelBuilder/FillDrawable") sorts the triangles of the parts that need it (hair and other alpha layers) with
// FUN_005d1960 ("CAS/ModelBuilder/TriangleSortDataList"): every triangle tested against every vertex of the part, with a
// division and a reciprocal square root each time. The frame profiler's sampler (30/09) found it in 83% of the "CAS
// SimService" hitches (~80 ms each). This feature answers that function (the entry chain, layer FastCas) with
// features/cas_tri_sort.h's Fast path: the same counts and the same stable sort, so the same index order bit for bit, with
// every vertex position computed once, four vertices per SSE instruction and the triangles split over a few worker threads
// (the calling thread's MXCSR copied to them). Offline (tools/cas_sort_test): a 2013-vertex, 3000-triangle part 88 ms ->
// 2.5 ms.
// Checks: the first 16 calls of each session (both builds) and, in the development build, 1 in 16 afterwards run the
// game's own function on a copy of the indices and compare: a difference is logged, the game's result is used and the
// feature turns itself off for the session.
#include <cstdint>
#include <string>

namespace FastCas {

bool Start(std::string* error);
void Stop();
bool Running();

struct Stats {
    uint64_t calls = 0, triangles = 0, tests = 0; // tests = triangles x vertices
    uint64_t passedThrough = 0;                   // calls the game's function answered (off, turned off)
    uint64_t parallel = 0, busy = 0;              // split over the workers; on the calling thread because the pool was busy
    uint64_t checked = 0, mismatches = 0;
    double fastMs = 0.0;                          // every call answered by Apex (checked ones included)
    double checkedGameMs = 0.0, checkedFastMs = 0.0;
    double maxMs = 0.0;                           // slowest call answered by Apex
    uint32_t workers = 0;
    bool selfDisabled = false;
    std::string lastMismatch;
};
Stats GetStats();
std::string StatusText();
void RenderDeveloperUI();

} // namespace FastCas
