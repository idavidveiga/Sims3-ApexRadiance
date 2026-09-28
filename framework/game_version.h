#pragma once
// Which TS3W.exe build is running, from the PE header timestamp. Game-code features carry the builds whose addresses
// they were verified on (Steam 1.67.2 for everything today); D3D-level features work on any build (VERSION_ALL).
#include <cstdint>

enum class GameVersion : uint8_t {
    Retail = 0,     // 1.67.2.024002 (disc)
    Steam = 1,      // 1.67.2.024037
    EA = 2,         // 1.69.47.024017 (EA app)
    EA_1_69_43 = 3, // 1.69.43.024017 (Origin / EA app)
    Unknown = 255
};

using GameVersionMask = uint32_t;
constexpr GameVersionMask VersionBit(GameVersion v) { return 1u << static_cast<unsigned>(v); }
constexpr GameVersionMask VERSION_RETAIL = VersionBit(GameVersion::Retail);
constexpr GameVersionMask VERSION_STEAM = VersionBit(GameVersion::Steam);
constexpr GameVersionMask VERSION_EA = VersionBit(GameVersion::EA) | VersionBit(GameVersion::EA_1_69_43);
constexpr GameVersionMask VERSION_ALL = 0xFFFFFFFFu; // any build, including unknown ones (no game-code addresses)

extern GameVersion g_gameVersion;
extern uint32_t g_exeTimestamp;

// Reads the timestamp of the main module and sets g_gameVersion. True when the build is a known one.
bool DetectGameVersion();
const char* GetGameVersionName();
bool IsVersionSupported(GameVersionMask mask);
