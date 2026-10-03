#pragma once
// Read-only diagnostic for the native lot LOD distance live setting.
// Built only in the dedicated Lot LOD probe flavour. It never writes WorldManager+0xDC.

#include <string>

namespace LotLodDistanceProbe {
bool Start(std::string* error = nullptr);
void Stop();
bool Running();
} // namespace LotLodDistanceProbe
