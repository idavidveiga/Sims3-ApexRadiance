#pragma once
// CAS fill probe (development build only, Steam 1.67.2): measures the CAS model builder's vertex packing, FUN_005d1010
// (called once per mesh part from FUN_005d3760 "CAS/ModelBuilder/FillDrawable"), which the frame profiler's sampler found
// taking 83% of the "CAS SimService" hitches (30/09: 38 hitches, ~80 ms each). Read-only: the game's function runs as
// always; the probe times each call and notes the vertex count, the stride and the kind of memory it writes to
// (VirtualQuery of the destination: write-combined / uncached GPU-mapped memory makes its byte writes and the read-back
// of the weights element very slow). A summary goes to the log every 10 s while calls come in.
//
// Part of Apex Radiance. Credits: @loinyx

namespace CasFillProbe {

// Hooks FUN_005d1010 (Detours) after checking its first bytes; development build on Steam 1.67.2 only. Call before the
// game can build a Sim (start-up).
void Start();
// Pump thread: the periodic summary line
void Tick();

} // namespace CasFillProbe
