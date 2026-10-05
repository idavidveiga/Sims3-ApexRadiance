// Light detail (part of Night Lighting, 2026-10-05; user: "the lights look low resolution, create an option in Apex to raise
// the quality of the lights").
//
// The game bakes lamp light into texture maps per story (room solve: walls into an atlas of strips, floors, ceilings and
// objects into maps over the lot), with a detail set by each room's lighting LOD class (room+0xF4, 0..2) from fixed tables
// in TS3W.exe (Steam 1.67.2, research\engine_map\full.asm and dwords.txt):
//  - 0x00FF36AC int[3] = {1, 2, 4}: texels per tile edge. Read by every per-class size: FUN_006a8d20 (the value),
//    FUN_006a8d30 (+1), FUN_006a8d40 (3n+1: the wall rows over 3 m, 4 / 7 / 13), FUN_006a8d50 (+2), FUN_006a8d60 /
//    FUN_006a8de0 (floor, ceiling and object map sizes: n x lot tiles, doubled in width where 0x00FF36A0[class] separates
//    diagonals, rounded up to a power of two by FUN_006a8c40, at most 2048), the wall packer FUN_006a90e0 (rows) and
//    CacheLightingParams FUN_006a4480 (the size the shaders' UV constants come from).
//  - 0x00FF36DC int[3] = {256, 512, 1024} and 0x00FF36E8 int[3] = {128, 256, 512}: the wall atlas per class, made by
//    FUN_006a90e0 -> FUN_006ac4f0(width, height, rows).
// High doubles both, class by class ({2, 4, 8} texels, walls {512, 1024, 2048} x {256, 512, 1024}): every size above
// keeps its proportions (a strip twice as long and twice as tall in an atlas twice as wide and twice as tall), the floor
// maps of the largest lot stay within the 2048 the game rounds to (2 x 8 x 64 = 1024 at class 2), and the lots the game
// lights at a low class keep small maps. Sims3SettingsSetter's Lighting Quality writes the same tables (every class at
// the top value); when another mod has changed them, nothing is written here.
// The tables are read when a lot's lighting is built: they are written once, before any lot is lit (at the main menu),
// and kept for the whole session (a lot lit with one detail must keep it). A change takes effect after a restart.
// Cost: four times the texels per room, so rooms take about four times longer to light (after loading and after a lamp
// changes) and their maps take four times the memory.
#include "light_detail.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "level_light_share.h"
#include "memory_patch.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <format>

namespace {

constexpr uintptr_t kTexelsPerTile = 0x00FF36AC; // int[3]
constexpr uintptr_t kWallAtlasW = 0x00FF36DC;    // int[3]
constexpr uintptr_t kWallAtlasH = 0x00FF36E8;    // int[3]
constexpr uintptr_t kTexelsGetter = 0x006A8D20;  // FUN_006a8d20: mov eax,[ecx+8]; mov eax,[eax*4+0x00FF36AC]; ret
constexpr uintptr_t kAtlasReads = 0x006A90FC;    // in FUN_006a90e0: mov edi,[eax+eax+0x00FF36DC]; mov ebx,[eax+eax+0x00FF36E8]
constexpr DWORD kGameTexels[3] = {1, 2, 4}, kGameAtlasW[3] = {256, 512, 1024}, kGameAtlasH[3] = {128, 256, 512};

int g_active = 0;
bool g_tried = false;
std::string g_status = "the game's own detail";

bool ReadTable(uintptr_t at, DWORD* out) { return MemPatch::ReadBytes(at, out, 3 * sizeof(DWORD)); }

} // namespace

namespace LightDetail {

bool ApplyAtStartup(int level) {
    if (g_tried) return g_active == level;
    g_tried = true;
    if (level <= 0) return true;
    // Steam 1.67.2 only: the table addresses are fixed there, and the code that reads them is checked byte by byte
    const BYTE getter[] = {0x8B, 0x41, 0x08, 0x8B, 0x04, 0x85, 0xAC, 0x36, 0xFF, 0x00, 0xC3};
    const BYTE atlas[] = {0x8B, 0xBC, 0x00, 0xDC, 0x36, 0xFF, 0x00, 0x8B, 0x9C, 0x00, 0xE8, 0x36, 0xFF, 0x00};
    BYTE seen[sizeof atlas] = {};
    if (!GameAddr::IsFixed() || !MemPatch::ReadBytes(kTexelsGetter, seen, sizeof getter) || std::memcmp(seen, getter, sizeof getter) != 0 ||
        !MemPatch::ReadBytes(kAtlasReads, seen, sizeof atlas) || std::memcmp(seen, atlas, sizeof atlas) != 0) {
        g_status = "not available on this game version (Steam 1.67.2 only)";
        LOG_WARNING("[LightDetail] " + g_status);
        return false;
    }
    DWORD texels[3] = {}, w[3] = {}, h[3] = {};
    if (!ReadTable(kTexelsPerTile, texels) || !ReadTable(kWallAtlasW, w) || !ReadTable(kWallAtlasH, h)) {
        g_status = "the game's tables could not be read";
        LOG_WARNING("[LightDetail] " + g_status);
        return false;
    }
    if (std::memcmp(texels, kGameTexels, sizeof texels) != 0 || std::memcmp(w, kGameAtlasW, sizeof w) != 0 || std::memcmp(h, kGameAtlasH, sizeof h) != 0) {
        g_status = std::format("left as it is: another mod already changed it (texels per tile {{{}, {}, {}}}, wall maps {}x{}; Sims3SettingsSetter's Lighting Quality?)",
                               texels[0], texels[1], texels[2], w[2], h[2]);
        LOG_WARNING("[LightDetail] " + g_status);
        return false;
    }
    if (LevelLightShare::LoadedLots() > 0) { // a lot already lit keeps the detail it was built with
        g_status = "takes effect after restarting the game (a lot was already lit)";
        LOG_INFO("[LightDetail] " + g_status);
        return false;
    }
    bool ok = true;
    for (int c = 0; c < 3 && ok; c++) {
        // kept for the whole session: a lot lit with this detail must never meet the game's tables again
        ok = MemPatch::WriteDWORD(kTexelsPerTile + c * 4, kGameTexels[c] * 2, nullptr, &kGameTexels[c]) &&
             MemPatch::WriteDWORD(kWallAtlasW + c * 4, kGameAtlasW[c] * 2, nullptr, &kGameAtlasW[c]) &&
             MemPatch::WriteDWORD(kWallAtlasH + c * 4, kGameAtlasH[c] * 2, nullptr, &kGameAtlasH[c]);
    }
    if (!ok) { // put back what was written (nothing is lit yet)
        for (int c = 0; c < 3; c++) {
            MemPatch::WriteDWORD(kTexelsPerTile + c * 4, kGameTexels[c]);
            MemPatch::WriteDWORD(kWallAtlasW + c * 4, kGameAtlasW[c]);
            MemPatch::WriteDWORD(kWallAtlasH + c * 4, kGameAtlasH[c]);
        }
        g_status = "the game's tables could not be written";
        LOG_WARNING("[LightDetail] " + g_status);
        return false;
    }
    g_active = level;
    g_status = "high: twice the lighting texels per metre on walls and floors (texels per tile {2, 4, 8}, wall maps up to 2048x1024)";
    LOG_INFO("[LightDetail] " + g_status);
    return true;
}

int Active() { return g_active; }

std::string Status() { return g_status; }

} // namespace LightDetail
