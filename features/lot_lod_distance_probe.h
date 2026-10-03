#pragma once
// Diagnostic for the native lot LOD visibility metric used by LotLodScoring.
// Dedicated test builds can run at the stock Lot LOD dist. 70 or temporarily at 100.
// The metric function is derived from the verified camera-bias JZ and accepted only if LotLodScoring
// contains exactly one CALL to it. The 100 build restores the original value when unloaded cleanly.

#include <string>

namespace LotLodDistanceProbe {
bool Start(std::string* error = nullptr);
void Stop();
bool Running();
} // namespace LotLodDistanceProbe
