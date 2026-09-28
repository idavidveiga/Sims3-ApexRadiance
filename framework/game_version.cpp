#include "game_version.h"
#include <windows.h>

GameVersion g_gameVersion = GameVersion::Unknown;
uint32_t g_exeTimestamp = 0;

namespace {
struct KnownBuild {
    uint32_t timestamp;
    GameVersion version;
    const char* name;
};
// Steam's value checked against S3SS-dev\re\TS3W.exe (0x52DEC247); the others are the published values of those builds.
constexpr KnownBuild kBuilds[] = {
    {0x52D872DAu, GameVersion::Retail, "Retail 1.67.2.024002"},
    {0x52DEC247u, GameVersion::Steam, "Steam 1.67.2.024037"},
    {0x6707155Cu, GameVersion::EA, "EA 1.69.47.024017"},
    {0x568D4BACu, GameVersion::EA_1_69_43, "EA 1.69.43.024017"},
};
} // namespace

bool DetectGameVersion() {
    const auto* image = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
    g_gameVersion = GameVersion::Unknown;
    g_exeTimestamp = 0;
    if (!image) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    g_exeTimestamp = nt->FileHeader.TimeDateStamp;
    for (const KnownBuild& b : kBuilds) {
        if (b.timestamp == g_exeTimestamp) {
            g_gameVersion = b.version;
            return true;
        }
    }
    return false;
}

const char* GetGameVersionName() {
    for (const KnownBuild& b : kBuilds)
        if (b.version == g_gameVersion) return b.name;
    return "Unknown";
}

bool IsVersionSupported(GameVersionMask mask) {
    if (mask == VERSION_ALL) return true;
    if (g_gameVersion == GameVersion::Unknown) return false;
    return (mask & VersionBit(g_gameVersion)) != 0;
}
