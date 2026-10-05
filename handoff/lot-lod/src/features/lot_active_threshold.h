#pragma once
// Internal lot-LoD transition threshold used by the game's "Throttle Lot LoD Transitions" live setting.
// This is NOT Options.ini maxactivelots. S3SS sets "Throttle Lot LoD Transitions Max Active Lot Threshold" to 12.
//
// Apex resolves the live setting from its UTF-16 registration name in the game executable, validates the pointed value,
// writes 12 only after that validation, maintains 12 while enabled, and restores the value that existed before Apex
// took ownership.

#include <string>

namespace LotActiveThreshold {

inline constexpr int kDesiredThreshold = 12;

bool Start(std::string* error = nullptr);
void Stop();
void Tick();

bool Running();
bool HandledByS3SS();
std::string StatusText();

} // namespace LotActiveThreshold
