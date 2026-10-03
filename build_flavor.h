#pragma once
// One binary. Normal mode by default; developer mode is selected at startup from [ui].
// Atomic because D3D callbacks can run while the initialization thread loads settings.
#include <atomic>
#define S3SS_TR(pt, en) en
inline std::atomic<bool> kPublicBuild{true}; // legacy name: true means developer mode is off
