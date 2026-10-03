#pragma once
// Controlled diagnostic for the native lot LOD distance live setting.
// Built only in the dedicated Lot LOD probe flavour. The current test validates baseline 70,
// temporarily applies 100 to WorldManager+0xDC, logs Detailed View transitions, and restores safely.

#include <string>

namespace LotLodDistanceProbe {
bool Start(std::string* error = nullptr);
void Stop();
bool Running();
} // namespace LotLodDistanceProbe
