#pragma once
// Lot visibility override from Sims3SettingsSetter LotStreamingOptimizations.visibilityOverride.
//
// The game's lot visibility/distance metric contains a short JZ controlling a camera-view distance bias. S3SS changes
// that opcode from 0x74 (JZ) to 0xEB (JMP), preventing lots from loading/unloading purely because of camera view angle.
// Apex only restores 0x74 when it can prove Apex itself changed 0x74 -> 0xEB.

#include <string>

namespace LotVisibilityOverride {

bool Start(std::string* error = nullptr);
void Stop();
void Tick();

bool Running();
bool HandledByS3SS();
bool AlreadyPatchedExternally();
std::string StatusText();

} // namespace LotVisibilityOverride
