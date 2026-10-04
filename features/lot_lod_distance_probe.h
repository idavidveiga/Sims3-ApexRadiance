#pragma once
// Diagnostic for the native lot LOD visibility metric used by LotLodScoring.
// Dedicated test builds can temporarily override the Lot LOD distance and, when explicitly requested,
// WorldManager Max Active Lots. Each controlled write is guarded against the observed baseline,
// maintained only while Apex still owns the value, and restored on a clean unload.
//
// The metric function is derived from the verified camera-bias JZ and accepted only if LotLodScoring
// contains exactly one CALL to it.

#include <string>

namespace LotLodDistanceProbe {
bool Start(std::string* error = nullptr);
void Stop();
bool Running();
} // namespace LotLodDistanceProbe
