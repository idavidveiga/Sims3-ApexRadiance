#pragma once
// One binary. Normal mode by default; developer mode is selected at startup from [ui].
// Atomic because D3D callbacks can run while the initialization thread loads settings.
#include <atomic>
#define S3SS_TR(pt, en) en
inline std::atomic<bool> kPublicBuild{true}; // legacy name: true means developer mode is off

// Two flavours (06/10, after 2.7.0): the players' build (ApexFlavorDefines=APEX_NO_DEV_TOOLS, PublicApexRadiance.asi) leaves
// out the developer tools and never turns developer mode on; the maintainer's build (ReleaseApexRadiance.asi) has them all.
#ifdef APEX_NO_DEV_TOOLS
inline constexpr bool kDevToolsBuild = false;
#else
inline constexpr bool kDevToolsBuild = true;
#endif

// Light through doors and windows (08/10, experimental, LevelLightShare::SetRealisticOpenings): off by default until it is
// approved in game; a build made with APEX_REALISTIC_OPENINGS in ApexFlavorDefines (the maintainer's "realistic" test build)
// turns it on by default
#ifdef APEX_REALISTIC_OPENINGS
inline constexpr bool kRealisticOpeningsDefault = true;
#else
inline constexpr bool kRealisticOpeningsDefault = false;
#endif
